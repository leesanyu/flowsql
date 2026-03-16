#ifndef _FLOWSQL_SERVICES_DATABASE_RELATION_ADAPTERS_H_
#define _FLOWSQL_SERVICES_DATABASE_RELATION_ADAPTERS_H_

#include <framework/interfaces/idatabase_channel.h>
#include "db_session.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// RelationBatchReader — 行式数据库通用批量读取器
// 将 IResultSet（行式游标）适配为 IBatchReader（Arrow IPC 流）
// MySQL 和 SQLite 共用此实现，无需各自重复
class RelationBatchReader : public IBatchReader {
public:
    RelationBatchReader(std::shared_ptr<IDbSession> session,
                        IResultSet* result,
                        std::shared_ptr<arrow::Schema> schema,
                        int batch_size = 1024)
        : session_(std::move(session)), result_(result),
          schema_(std::move(schema)), batch_size_(batch_size) {}

    ~RelationBatchReader() override {
        delete result_;
    }

    int GetSchema(const uint8_t** data, size_t* size) override {
        if (schema_buffer_) {
            *data = schema_buffer_->data();
            *size = schema_buffer_->size();
            return 0;
        }
        auto out = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(out, schema_).ValueOrDie();
        if (!writer->Close().ok()) return -1;
        auto buf = out->Finish();
        if (!buf.ok()) return -1;
        schema_buffer_ = *buf;
        *data = schema_buffer_->data();
        *size = schema_buffer_->size();
        return 0;
    }

    int Next(const uint8_t** data, size_t* size) override {
        if (done_) { *data = nullptr; *size = 0; return 1; }

        // 为每列创建 Builder
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        for (const auto& field : schema_->fields()) {
            std::unique_ptr<arrow::ArrayBuilder> b;
            if (!arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &b).ok())
                return -1;
            builders.push_back(std::move(b));
        }

        int row_count = 0;
        for (int i = 0; i < batch_size_; ++i) {
            if (!result_->Next()) { done_ = true; break; }

            for (int col = 0; col < schema_->num_fields(); ++col) {
                auto* b = builders[col].get();
                if (result_->IsNull(col)) { (void)b->AppendNull(); continue; }

                switch (b->type()->id()) {
                    case arrow::Type::INT32: {
                        int v; if (result_->GetInt(col, &v) == 0)
                            (void)static_cast<arrow::Int32Builder*>(b)->Append(v);
                        else (void)b->AppendNull();
                        break;
                    }
                    case arrow::Type::INT64: {
                        int64_t v; if (result_->GetInt64(col, &v) == 0)
                            (void)static_cast<arrow::Int64Builder*>(b)->Append(v);
                        else (void)b->AppendNull();
                        break;
                    }
                    case arrow::Type::FLOAT: {
                        double v; if (result_->GetDouble(col, &v) == 0)
                            (void)static_cast<arrow::FloatBuilder*>(b)->Append(static_cast<float>(v));
                        else (void)b->AppendNull();
                        break;
                    }
                    case arrow::Type::DOUBLE: {
                        double v; if (result_->GetDouble(col, &v) == 0)
                            (void)static_cast<arrow::DoubleBuilder*>(b)->Append(v);
                        else (void)b->AppendNull();
                        break;
                    }
                    case arrow::Type::BINARY: {
                        const char* s; size_t len;
                        if (result_->GetString(col, &s, &len) == 0)
                            (void)static_cast<arrow::BinaryBuilder*>(b)->Append(
                                reinterpret_cast<const uint8_t*>(s), len);
                        else (void)b->AppendNull();
                        break;
                    }
                    default: {  // STRING / UTF8 及其他文本类型
                        const char* s; size_t len;
                        if (result_->GetString(col, &s, &len) == 0)
                            (void)static_cast<arrow::StringBuilder*>(b)->Append(s, len);
                        else (void)b->AppendNull();
                        break;
                    }
                }
            }
            ++row_count;
        }

        if (row_count == 0) { *data = nullptr; *size = 0; return 1; }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& b : builders) {
            std::shared_ptr<arrow::Array> arr;
            if (!b->Finish(&arr).ok()) return -1;
            arrays.push_back(arr);
        }

        auto batch = arrow::RecordBatch::Make(schema_, row_count, arrays);
        auto out = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(out, schema_).ValueOrDie();
        if (!writer->WriteRecordBatch(*batch).ok()) return -1;
        if (!writer->Close().ok()) return -1;
        auto buf = out->Finish();
        if (!buf.ok()) return -1;
        batch_buffer_ = *buf;
        *data = batch_buffer_->data();
        *size = batch_buffer_->size();
        return 0;
    }

    void Cancel() override { cancelled_ = true; }
    void Close() override {}
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    IResultSet* result_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::Buffer> schema_buffer_;
    std::shared_ptr<arrow::Buffer> batch_buffer_;
    std::string last_error_;
    int batch_size_ = 1024;
    bool done_ = false;
    bool cancelled_ = false;
};

// RelationBatchWriterBase — 行式数据库通用批量写入器基类
// 提取 Write/Flush/Close/事务管理等公共逻辑，子类只需实现三个钩子：
//   - QuoteIdentifier：标识符引用风格（MySQL 用反引号，SQLite 用双引号）
//   - CreateTable：建表 DDL（类型映射因数据库而异）
//   - InsertBatch：插入策略（MySQL 分块多值 INSERT，SQLite 逐行 INSERT）
class RelationBatchWriterBase : public IBatchWriter {
public:
    RelationBatchWriterBase(std::shared_ptr<IDbSession> session, const char* table)
        : session_(std::move(session)), table_(table) {}

    ~RelationBatchWriterBase() override {
        if (transaction_started_ && !committed_) {
            session_->RollbackTransaction();
        }
    }

    int Write(const uint8_t* data, size_t size) override {
        auto buffer = arrow::Buffer::Wrap(data, size);
        auto buf_reader = std::make_shared<arrow::io::BufferReader>(buffer);
        auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buf_reader);
        if (!reader_result.ok()) return -1;
        auto reader = *reader_result;

        std::shared_ptr<arrow::RecordBatch> batch;
        if (!reader->ReadNext(&batch).ok() || !batch) return -1;

        if (!transaction_started_) {
            if (session_->BeginTransaction() != 0) {
                last_error_ = std::string("BeginTransaction failed: ") + session_->GetLastError();
                return -1;
            }
            transaction_started_ = true;
            if (CreateTable(batch->schema()) != 0) return -1;
        }

        return InsertBatch(batch);
    }

    int Flush() override { return 0; }

    void Close(BatchWriteStats* stats) override {
        if (transaction_started_ && !committed_) {
            if (session_->CommitTransaction() != 0) {
                last_error_ = std::string("CommitTransaction failed: ") + session_->GetLastError();
            } else {
                committed_ = true;
            }
        }
        if (stats) {
            stats->rows_written = rows_written_;
            stats->bytes_written = bytes_written_;
            stats->elapsed_ms = 0;
        }
    }

    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

protected:
    // 标识符引用（子类实现各自的 SQL 方言）
    virtual std::string QuoteIdentifier(const std::string& name) = 0;

    // 建表 DDL（子类实现各自的类型映射）
    virtual int CreateTable(std::shared_ptr<arrow::Schema> schema) = 0;

    // 插入一批数据（子类实现各自的插入策略）
    virtual int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) = 0;

    // 构造单行值列表字符串，供子类的 InsertBatch 使用
    // 子类传入自己的 QuoteString 函数（因转义规则不同）
    static std::string BuildRowValues(
        const std::shared_ptr<arrow::RecordBatch>& batch,
        int64_t row,
        const std::function<std::string(const std::string&)>& quote_string) {
        std::string values = "(";
        for (int col = 0; col < batch->num_columns(); ++col) {
            if (col > 0) values += ", ";
            auto array = batch->column(col);
            if (array->IsNull(row)) {
                values += "NULL";
            } else if (array->type()->id() == arrow::Type::INT32) {
                values += std::to_string(
                    std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
            } else if (array->type()->id() == arrow::Type::INT64) {
                values += std::to_string(
                    std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
            } else if (array->type()->id() == arrow::Type::FLOAT) {
                values += std::to_string(
                    std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
            } else if (array->type()->id() == arrow::Type::DOUBLE) {
                values += std::to_string(
                    std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
            } else {
                auto str_array = std::static_pointer_cast<arrow::StringArray>(array);
                values += quote_string(str_array->GetString(row));
            }
        }
        values += ")";
        return values;
    }

protected:
    std::shared_ptr<IDbSession> session_;
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    bool transaction_started_ = false;
    bool committed_ = false;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_RELATION_ADAPTERS_H_
