#include "python_operator_bridge.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/statvfs.h>

#include "arrow_ipc_serializer.h"
#include "shared_memory_guard.h"
#include "framework/core/dataframe.h"
#include "framework/interfaces/idataframe_channel.h"

namespace {

// 生成 UUID（Linux 标准方式）
std::string GenerateUUID() {
    std::ifstream ifs("/proc/sys/kernel/random/uuid");
    std::string uuid;
    if (ifs.is_open()) {
        std::getline(ifs, uuid);
    }
    return uuid;
}

// 选择共享内存目录：/dev/shm 可用且空间足够则优先，否则回退 /tmp
std::string ChooseShmDir() {
    struct statvfs stat;
    if (statvfs("/dev/shm", &stat) == 0) {
        // /dev/shm 可用且剩余空间 > 256MB
        uint64_t free_bytes = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
        if (free_bytes > 256ULL * 1024 * 1024) {
            return "/dev/shm";
        }
    }
    return "/tmp";
}

}  // namespace

namespace flowsql {
namespace bridge {

PythonOperatorBridge::PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port)
    : meta_(meta), host_(host), port_(port) {
    client_ = std::make_unique<httplib::Client>(host, port);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(30);
    client_->set_write_timeout(10);
    client_->set_keep_alive(true);
}

int PythonOperatorBridge::Work(IChannel* in, IChannel* out) {
    last_error_.clear();

    // 1. dynamic_cast 到 IDataFrameChannel
    auto* df_in = dynamic_cast<IDataFrameChannel*>(in);
    auto* df_out = dynamic_cast<IDataFrameChannel*>(out);
    if (!df_in || !df_out) {
        last_error_ = "channel type mismatch (expected IDataFrameChannel)";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 2. 从输入通道读取 DataFrame
    DataFrame in_frame;
    if (df_in->Read(&in_frame) != 0) {
        last_error_ = "Read from input channel failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 3. 序列化 Arrow IPC 到共享内存文件
    auto batch = in_frame.ToArrow();
    if (!batch) {
        last_error_ = "ToArrow() returned null";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    std::string shm_dir = ChooseShmDir();
    std::string uuid = GenerateUUID();
    if (uuid.empty()) {
        last_error_ = "Failed to generate UUID";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    std::string in_path = shm_dir + "/flowsql_" + uuid + "_in";
    std::string out_path = shm_dir + "/flowsql_" + uuid + "_out";
    SharedMemoryGuard guard(in_path, out_path);

    if (ArrowIpcSerializer::SerializeToFile(batch, in_path) != 0) {
        last_error_ = "Arrow IPC serialize to file failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 4. HTTP POST JSON 路径到 Python Worker
    std::string path = "/operators/python/work/" + meta_.category + "/" + meta_.name;

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> json_writer(sb);
    json_writer.StartObject();
    json_writer.Key("input");
    json_writer.String(in_path.c_str());
    json_writer.EndObject();

    auto res = client_->Post(path, sb.GetString(), "application/json");

    if (!res) {
        last_error_ = "HTTP POST to Python Worker failed (connection error)";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    if (res->status != 200) {
        last_error_ = "Python Worker returned HTTP " + std::to_string(res->status);
        if (!res->body.empty()) {
            rapidjson::Document doc;
            doc.Parse(res->body.c_str());
            if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("detail") && doc["detail"].IsString()) {
                last_error_ += ": " + std::string(doc["detail"].GetString());
            } else {
                last_error_ += ": " + res->body;
            }
        }
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    // 5. 解析响应 JSON，从共享内存文件反序列化结果
    rapidjson::Document res_doc;
    res_doc.Parse(res->body.c_str());
    if (res_doc.HasParseError() || !res_doc.IsObject() || !res_doc.HasMember("output") ||
        !res_doc["output"].IsString()) {
        last_error_ = "Invalid JSON response from Python Worker: " + res->body;
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    std::string output_path = res_doc["output"].GetString();
    std::shared_ptr<arrow::RecordBatch> result_batch;
    if (ArrowIpcSerializer::DeserializeFromFile(output_path, &result_batch) != 0) {
        last_error_ = "Deserialize Arrow IPC from file failed: " + output_path;
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    DataFrame out_frame;
    out_frame.FromArrow(result_batch);

    if (df_out->Write(&out_frame) != 0) {
        last_error_ = "Write to output channel failed";
        printf("PythonOperatorBridge[%s.%s]: %s\n",
               meta_.category.c_str(), meta_.name.c_str(), last_error_.c_str());
        return -1;
    }

    return 0;
}

int PythonOperatorBridge::Configure(const char* key, const char* value) {
    if (!key || !value) return -1;

    std::string path = "/operators/python/configure/" + meta_.category + "/" + meta_.name;
    std::string body = std::string("{\"key\":\"") + key + "\",\"value\":\"" + value + "\"}";
    auto res = client_->Post(path, body, "application/json");

    if (!res || res->status != 200) {
        printf("PythonOperatorBridge[%s.%s]: Configure failed\n",
               meta_.category.c_str(), meta_.name.c_str());
        return -1;
    }
    return 0;
}

}  // namespace bridge
}  // namespace flowsql