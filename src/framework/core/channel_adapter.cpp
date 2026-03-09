#include "channel_adapter.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <cstdio>

#include "dataframe.h"

namespace flowsql {

// 从 Arrow Schema 构建 Field 列表的辅助函数
static std::vector<Field> SchemaToFields(const std::shared_ptr<arrow::Schema>& schema) {
    std::vector<Field> fields;
    for (int i = 0; i < schema->num_fields(); ++i) {
        Field f;
        f.name = schema->field(i)->name();
        f.type = FromArrowType(schema->field(i)->type());
        fields.push_back(f);
    }
    return fields;
}

// 将 RecordBatch 追加到 DataFrame（合并行）
static void AppendBatchToDataFrame(const std::shared_ptr<arrow::RecordBatch>& batch,
                                    DataFrame& result, bool& schema_set) {
    if (!batch) return;

    if (!schema_set) {
        result.SetSchema(SchemaToFields(batch->schema()));
        schema_set = true;
    }

    // 通过 FromArrow 反序列化后逐行追加
    DataFrame tmp;
    tmp.FromArrow(batch);
    for (int32_t r = 0; r < tmp.RowCount(); ++r) {
        result.AppendRow(tmp.GetRow(r));
    }
}

int ChannelAdapter::ReadToDataFrame(IDatabaseChannel* db, const char* query,
                                     IDataFrameChannel* df_out, std::string* error) {
    if (!db || !df_out) return -1;

    IBatchReader* reader = nullptr;
    if (db->CreateReader(query, &reader) != 0 || !reader) {
        if (error) *error = "CreateReader failed for query: " + std::string(query ? query : "");
        return -1;
    }

    // 读取所有批次，反序列化为 DataFrame 写入输出通道
    DataFrame result;
    bool schema_set = false;
    const uint8_t* buf = nullptr;
    size_t len = 0;

    while (true) {
        int rc = reader->Next(&buf, &len);
        if (rc == 1) break;  // 已读完
        if (rc < 0) {
            if (error) *error = std::string(reader->GetLastError());
            reader->Close();
            reader->Release();
            return -1;
        }

        // 反序列化 Arrow IPC buffer → RecordBatch（尝试 stream 格式）
        auto arrow_buf = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
        auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (stream_result.ok()) {
            auto stream_reader = *stream_result;
            if (!schema_set) {
                result.SetSchema(SchemaToFields(stream_reader->schema()));
                schema_set = true;
            }
            std::shared_ptr<arrow::RecordBatch> batch;
            while (stream_reader->ReadNext(&batch).ok() && batch) {
                AppendBatchToDataFrame(batch, result, schema_set);
            }
        } else {
            // IPC 反序列化失败，报错并终止读取
            if (error) *error = "IPC deserialize failed: " + stream_result.status().ToString();
            reader->Close();
            reader->Release();
            return -1;
        }
    }

    reader->Close();
    reader->Release();

    // 写入输出通道
    return df_out->Write(&result);
}

int64_t ChannelAdapter::WriteFromDataFrame(IDataFrameChannel* df_in,
                                           IDatabaseChannel* db, const char* table,
                                           std::string* error) {
    if (!df_in || !db || !table) return -1;

    // 从 DataFrame 通道读取数据
    DataFrame data;
    if (df_in->Read(&data) != 0 || data.RowCount() == 0) {
        if (error) *error = "no data to write";
        return -1;
    }

    // 创建写入器
    IBatchWriter* writer = nullptr;
    if (db->CreateWriter(table, &writer) != 0 || !writer) {
        if (error) *error = "CreateWriter failed for table: " + std::string(table);
        return -1;
    }

    // DataFrame → Arrow RecordBatch → IPC 序列化 → Writer
    auto batch = data.ToArrow();
    if (!batch) {
        if (error) *error = "ToArrow conversion failed";
        writer->Close(nullptr);
        writer->Release();
        return -1;
    }

    // 序列化为 IPC stream 格式
    auto sink_stream = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_writer = arrow::ipc::MakeStreamWriter(sink_stream, batch->schema()).ValueOrDie();
    auto status = ipc_writer->WriteRecordBatch(*batch);
    if (!status.ok()) {
        if (error) *error = "IPC serialize failed: " + status.ToString();
        writer->Close(nullptr);
        writer->Release();
        return -1;
    }
    ipc_writer->Close();

    auto buffer = sink_stream->Finish().ValueOrDie();
    if (writer->Write(buffer->data(), static_cast<size_t>(buffer->size())) != 0) {
        if (error) *error = std::string(writer->GetLastError());
        writer->Close(nullptr);
        writer->Release();
        return -1;
    }

    writer->Flush();
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();

    printf("ChannelAdapter::WriteFromDataFrame: wrote %ld rows, %ld bytes in %ld ms\n",
           stats.rows_written, stats.bytes_written, stats.elapsed_ms);

    // 返回写入的行数
    return stats.rows_written;
}

int ChannelAdapter::CopyDataFrame(IDataFrameChannel* src, IDataFrameChannel* dst) {
    if (!src || !dst) return -1;

    DataFrame data;
    if (src->Read(&data) != 0) return -1;
    return dst->Write(&data);
}

}  // namespace flowsql