#ifndef _FLOWSQL_BRIDGE_ARROW_IPC_SERIALIZER_H_
#define _FLOWSQL_BRIDGE_ARROW_IPC_SERIALIZER_H_

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <string>

namespace flowsql {
namespace bridge {

class ArrowIpcSerializer {
 public:
    // 序列化 RecordBatch 为 Arrow IPC stream 格式
    // 成功返回 0，失败返回 -1
    static int Serialize(const std::shared_ptr<arrow::RecordBatch>& batch, std::string* out);

    // 反序列化 Arrow IPC stream 为 RecordBatch
    // 成功返回 0，失败返回 -1
    static int Deserialize(const std::string& data, std::shared_ptr<arrow::RecordBatch>* out);

    // 序列化 RecordBatch 为 Arrow IPC stream 写入文件
    static int SerializeToFile(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& path);

    // 从文件 memory_map 零拷贝读取 Arrow IPC stream 为 RecordBatch
    static int DeserializeFromFile(const std::string& path, std::shared_ptr<arrow::RecordBatch>* out);
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_ARROW_IPC_SERIALIZER_H_
