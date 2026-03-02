#include "arrow_ipc_serializer.h"

#include <cstdio>
#include <cstring>

namespace flowsql {
namespace bridge {

int ArrowIpcSerializer::Serialize(const std::shared_ptr<arrow::RecordBatch>& batch, std::string* out) {
    if (!batch || !out) return -1;

    auto buffer_stream = arrow::io::BufferOutputStream::Create().ValueOrDie();

    auto writer_result = arrow::ipc::MakeStreamWriter(buffer_stream, batch->schema());
    if (!writer_result.ok()) {
        printf("ArrowIpcSerializer::Serialize: MakeStreamWriter failed: %s\n",
               writer_result.status().ToString().c_str());
        return -1;
    }
    auto writer = *writer_result;

    auto status = writer->WriteRecordBatch(*batch);
    if (!status.ok()) {
        printf("ArrowIpcSerializer::Serialize: WriteRecordBatch failed: %s\n", status.ToString().c_str());
        return -1;
    }

    status = writer->Close();
    if (!status.ok()) {
        printf("ArrowIpcSerializer::Serialize: Close failed: %s\n", status.ToString().c_str());
        return -1;
    }

    auto buffer_result = buffer_stream->Finish();
    if (!buffer_result.ok()) {
        printf("ArrowIpcSerializer::Serialize: Finish failed: %s\n", buffer_result.status().ToString().c_str());
        return -1;
    }
    auto buffer = *buffer_result;

    out->assign(reinterpret_cast<const char*>(buffer->data()), buffer->size());
    return 0;
}

int ArrowIpcSerializer::Deserialize(const std::string& data, std::shared_ptr<arrow::RecordBatch>* out) {
    if (data.empty() || !out) return -1;

    // 必须使用拥有所有权的 buffer：Arrow IPC 反序列化产生的 RecordBatch 会零拷贝引用底层内存，
    // 如果使用 Buffer::Wrap（非拥有），当 data 被销毁后 RecordBatch 会持有悬空指针
    auto alloc_result = arrow::AllocateBuffer(static_cast<int64_t>(data.size()));
    if (!alloc_result.ok()) {
        printf("ArrowIpcSerializer::Deserialize: AllocateBuffer failed: %s\n",
               alloc_result.status().ToString().c_str());
        return -1;
    }
    auto owned_buffer = std::move(*alloc_result);
    memcpy(owned_buffer->mutable_data(), data.data(), data.size());
    auto buffer_reader = std::make_shared<arrow::io::BufferReader>(std::move(owned_buffer));

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buffer_reader);
    if (!reader_result.ok()) {
        printf("ArrowIpcSerializer::Deserialize: Open failed: %s\n", reader_result.status().ToString().c_str());
        return -1;
    }
    auto reader = *reader_result;

    auto batch_result = reader->Next();
    if (!batch_result.ok()) {
        printf("ArrowIpcSerializer::Deserialize: Next failed: %s\n", batch_result.status().ToString().c_str());
        return -1;
    }
    *out = *batch_result;

    if (!*out) {
        printf("ArrowIpcSerializer::Deserialize: empty stream\n");
        return -1;
    }
    return 0;
}

int ArrowIpcSerializer::SerializeToFile(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& path) {
    if (!batch) return -1;

    auto file_result = arrow::io::FileOutputStream::Open(path);
    if (!file_result.ok()) {
        printf("ArrowIpcSerializer::SerializeToFile: Open failed: %s\n", file_result.status().ToString().c_str());
        return -1;
    }
    auto file_stream = *file_result;

    auto writer_result = arrow::ipc::MakeStreamWriter(file_stream, batch->schema());
    if (!writer_result.ok()) {
        printf("ArrowIpcSerializer::SerializeToFile: MakeStreamWriter failed: %s\n",
               writer_result.status().ToString().c_str());
        return -1;
    }
    auto writer = *writer_result;

    auto status = writer->WriteRecordBatch(*batch);
    if (!status.ok()) {
        printf("ArrowIpcSerializer::SerializeToFile: WriteRecordBatch failed: %s\n", status.ToString().c_str());
        return -1;
    }

    status = writer->Close();
    if (!status.ok()) {
        printf("ArrowIpcSerializer::SerializeToFile: Close failed: %s\n", status.ToString().c_str());
        return -1;
    }

    status = file_stream->Close();
    if (!status.ok()) {
        printf("ArrowIpcSerializer::SerializeToFile: FileClose failed: %s\n", status.ToString().c_str());
        return -1;
    }

    return 0;
}

int ArrowIpcSerializer::DeserializeFromFile(const std::string& path, std::shared_ptr<arrow::RecordBatch>* out) {
    if (path.empty() || !out) return -1;

    // memory_map 零拷贝读取
    auto mmap_result = arrow::io::MemoryMappedFile::Open(path, arrow::io::FileMode::READ);
    if (!mmap_result.ok()) {
        printf("ArrowIpcSerializer::DeserializeFromFile: MemoryMappedFile::Open failed: %s\n",
               mmap_result.status().ToString().c_str());
        return -1;
    }
    auto mmap = *mmap_result;

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(mmap);
    if (!reader_result.ok()) {
        printf("ArrowIpcSerializer::DeserializeFromFile: Open reader failed: %s\n",
               reader_result.status().ToString().c_str());
        return -1;
    }
    auto reader = *reader_result;

    auto batch_result = reader->Next();
    if (!batch_result.ok()) {
        printf("ArrowIpcSerializer::DeserializeFromFile: Next failed: %s\n",
               batch_result.status().ToString().c_str());
        return -1;
    }
    *out = *batch_result;

    if (!*out) {
        printf("ArrowIpcSerializer::DeserializeFromFile: empty stream\n");
        return -1;
    }
    return 0;
}

}  // namespace bridge
}  // namespace flowsql
