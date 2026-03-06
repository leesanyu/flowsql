#include "row_based_db_driver_base.h"

#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <cstdio>

namespace flowsql {
namespace database {

// RowBasedBatchReader — 通用批量读取器
class RowBasedBatchReader : public IBatchReader {
public:
    RowBasedBatchReader(RowBasedDbDriverBase* driver, void* result,
                        std::shared_ptr<arrow::Schema> schema)
        : driver_(driver), result_(result), schema_(std::move(schema)) {}

    ~RowBasedBatchReader() override {
        if (result_) {
            driver_->FreeResultImpl(result_);
        }
    }

    int GetSchema(const uint8_t** data, size_t* size) override {
        if (schema_buffer_) {
            *data = schema_buffer_->data();
            *size = schema_buffer_->size();
            return 0;
        }

        // 序列化 Schema 到 IPC 格式（只写 Schema，不写 RecordBatch）
        auto buffer_output = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(buffer_output, schema_).ValueOrDie();
        // 注意：这里只创建 writer 并关闭，会写入 Schema 消息
        if (!writer->Close().ok()) {
            return -1;
        }
        schema_buffer_ = buffer_output->Finish().ValueOrDie();
        *data = schema_buffer_->data();
        *size = schema_buffer_->size();
        return 0;
    }

    int Next(const uint8_t** data, size_t* size) override {
        if (done_) {
            *data = nullptr;
            *size = 0;
            return 1;  // 返回 1 表示已读完
        }

        // 创建 Arrow builders
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        for (const auto& field : schema_->fields()) {
            std::unique_ptr<arrow::ArrayBuilder> builder;
            arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder);
            builders.push_back(std::move(builder));
        }

        // 批量读取行（最多 1024 行）
        const int batch_size = 1024;
        int row_count = 0;
        std::string error;

        for (int i = 0; i < batch_size; ++i) {
            int ret = driver_->FetchRowImpl(result_, builders, &error);
            if (ret < 0) {
                last_error_ = "FetchRowImpl failed: " + error;
                printf("RowBasedBatchReader: %s\n", last_error_.c_str());
                return -1;
            }
            if (ret == 0) {
                // 没有更多行
                done_ = true;
                break;
            }
            ++row_count;
        }

        if (row_count == 0) {
            *data = nullptr;
            *size = 0;
            return 1;  // 返回 1 表示已读完（没有更多数据）
        }

        // 构建 RecordBatch
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& builder : builders) {
            std::shared_ptr<arrow::Array> array;
            if (!builder->Finish(&array).ok()) {
                return -1;
            }
            arrays.push_back(array);
        }
        auto batch = arrow::RecordBatch::Make(schema_, row_count, arrays);

        // 序列化到 IPC 格式
        auto buffer_output = arrow::io::BufferOutputStream::Create().ValueOrDie();
        auto writer = arrow::ipc::MakeStreamWriter(buffer_output, schema_).ValueOrDie();
        if (!writer->WriteRecordBatch(*batch).ok()) {
            return -1;
        }
        if (!writer->Close().ok()) {
            return -1;
        }

        batch_buffer_ = buffer_output->Finish().ValueOrDie();
        *data = batch_buffer_->data();
        *size = batch_buffer_->size();
        return 0;
    }

    void Cancel() override { cancelled_ = true; }

    void Close() override {
        if (result_) {
            driver_->FreeResultImpl(result_);
            result_ = nullptr;
        }
    }

    const char* GetLastError() override { return last_error_.c_str(); }

    void Release() override { delete this; }

private:
    RowBasedDbDriverBase* driver_;
    void* result_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::Buffer> schema_buffer_;
    std::shared_ptr<arrow::Buffer> batch_buffer_;
    std::string last_error_;
    bool done_ = false;
    bool cancelled_ = false;
};

// RowBasedBatchWriter — 通用批量写入器
class RowBasedBatchWriter : public IBatchWriter {
public:
    RowBasedBatchWriter(RowBasedDbDriverBase* driver, const char* table)
        : driver_(driver), table_(table) {}

    ~RowBasedBatchWriter() override {
        // 确保事务提交
        if (transaction_started_ && !committed_) {
            std::string error;
            driver_->RollbackTransaction(&error);
        }
    }

    int Write(const uint8_t* data, size_t size) override {
        // 反序列化 Arrow IPC
        auto buffer = arrow::Buffer::Wrap(data, size);
        auto buffer_reader = std::make_shared<arrow::io::BufferReader>(buffer);
        auto reader = arrow::ipc::RecordBatchStreamReader::Open(buffer_reader).ValueOrDie();

        std::shared_ptr<arrow::RecordBatch> batch;
        if (!reader->ReadNext(&batch).ok() || !batch) {
            return -1;
        }

        // 开始事务（首次写入）
        if (!transaction_started_) {
            std::string error;
            if (driver_->BeginTransaction(&error) != 0) {
                last_error_ = "BeginTransaction failed: " + error;
                printf("RowBasedBatchWriter: %s\n", last_error_.c_str());
                return -1;
            }
            transaction_started_ = true;

            // 自动建表（简化实现：只支持基本类型）
            if (CreateTable(batch->schema()) != 0) {
                return -1;
            }
        }

        // 批量 INSERT
        if (InsertBatch(batch) != 0) {
            return -1;
        }

        return 0;
    }

    int Flush() override { return 0; }

    void Close(BatchWriteStats* stats) override {
        if (transaction_started_ && !committed_) {
            std::string error;
            if (driver_->CommitTransaction(&error) != 0) {
                last_error_ = "CommitTransaction failed: " + error;
                printf("RowBasedBatchWriter: %s\n", last_error_.c_str());
            } else {
                committed_ = true;
            }
        }

        if (stats) {
            stats->rows_written = rows_written_;
            stats->bytes_written = bytes_written_;
            stats->elapsed_ms = 0;  // TODO: 计算实际耗时
        }
    }

    const char* GetLastError() override { return last_error_.c_str(); }

    void Release() override { delete this; }

private:
    int CreateTable(std::shared_ptr<arrow::Schema> schema) {
        // 生成 CREATE TABLE 语句
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + table_ + " (";
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (i > 0) ddl += ", ";
            auto field = schema->field(i);
            ddl += field->name() + " ";

            // Arrow 类型 → SQL 类型映射
            auto type_id = field->type()->id();
            if (type_id == arrow::Type::INT64) {
                ddl += "BIGINT";
            } else if (type_id == arrow::Type::DOUBLE) {
                ddl += "DOUBLE";
            } else if (type_id == arrow::Type::STRING) {
                ddl += "TEXT";
            } else {
                ddl += "TEXT";  // 默认 TEXT
            }
        }
        ddl += ")";

        std::string error;
        if (driver_->ExecuteSqlImpl(ddl.c_str(), &error) != 0) {
            last_error_ = "CREATE TABLE failed: " + error;
            printf("RowBasedBatchWriter: %s\n", last_error_.c_str());
            return -1;
        }
        return 0;
    }

    int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) {
        // 生成 INSERT 语句（简化实现：逐行插入）
        // TODO: 使用预编译语句提升性能
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            std::string sql = "INSERT INTO " + table_ + " VALUES (";
            for (int col = 0; col < batch->num_columns(); ++col) {
                if (col > 0) sql += ", ";
                auto array = batch->column(col);

                if (array->IsNull(row)) {
                    sql += "NULL";
                } else if (array->type()->id() == arrow::Type::INT64) {
                    auto int_array = std::static_pointer_cast<arrow::Int64Array>(array);
                    sql += std::to_string(int_array->Value(row));
                } else if (array->type()->id() == arrow::Type::DOUBLE) {
                    auto double_array = std::static_pointer_cast<arrow::DoubleArray>(array);
                    sql += std::to_string(double_array->Value(row));
                } else if (array->type()->id() == arrow::Type::STRING) {
                    auto string_array = std::static_pointer_cast<arrow::StringArray>(array);
                    std::string value = string_array->GetString(row);
                    // 简单转义（TODO: 使用参数化查询）
                    sql += "'";
                    for (char c : value) {
                        if (c == '\'') sql += "''";
                        else sql += c;
                    }
                    sql += "'";
                } else {
                    sql += "NULL";
                }
            }
            sql += ")";

            std::string error;
            if (driver_->ExecuteSqlImpl(sql.c_str(), &error) != 0) {
                last_error_ = "INSERT failed: " + error;
                printf("RowBasedBatchWriter: %s\n", last_error_.c_str());
                return -1;
            }
            ++rows_written_;
        }
        return 0;
    }

    RowBasedDbDriverBase* driver_;
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    bool transaction_started_ = false;
    bool committed_ = false;
};

// 实现模板方法
int RowBasedDbDriverBase::CreateReader(const char* query, IBatchReader** reader) {
    std::string error;
    void* result = ExecuteQueryImpl(query, &error);
    if (!result) {
        last_error_ = error;
        printf("RowBasedDbDriverBase::CreateReader: ExecuteQueryImpl failed: %s\n", error.c_str());
        return -1;
    }

    auto schema = InferSchemaImpl(result, &error);
    if (!schema) {
        last_error_ = error;
        printf("RowBasedDbDriverBase::CreateReader: InferSchemaImpl failed: %s\n", error.c_str());
        FreeResultImpl(result);
        return -1;
    }

    *reader = new RowBasedBatchReader(this, result, schema);
    return 0;
}

int RowBasedDbDriverBase::CreateWriter(const char* table, IBatchWriter** writer) {
    *writer = new RowBasedBatchWriter(this, table);
    return 0;
}

}  // namespace database
}  // namespace flowsql
