#include "bridge_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <thread>

#include <httplib.h>
#include <rapidjson/document.h>

namespace flowsql {
namespace bridge {

// 清理指定目录下的 flowsql_* 残留文件
static void CleanupShmFiles(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(d)) != nullptr) {
        if (strncmp(entry->d_name, "flowsql_", 8) == 0) {
            std::string full_path = std::string(dir) + "/" + entry->d_name;
            if (std::remove(full_path.c_str()) == 0) {
                count++;
            }
        }
    }
    closedir(d);

    if (count > 0) {
        printf("BridgePlugin: cleaned up %d stale shm files in %s\n", count, dir);
    }
}

int BridgePlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "worker_host") host_ = val;
        else if (key == "worker_port") port_ = std::stoi(val);

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int BridgePlugin::Load(IQuerier* querier) {
    querier_ = querier;

    // 从环境变量获取 Gateway 地址，通过 Gateway 路由发现 PyWorker
    const char* gw = std::getenv("FLOWSQL_GATEWAY_ADDR");
    if (gw) {
        gateway_addr_ = gw;
        std::string gw_host = "127.0.0.1";
        int gw_port = 18800;
        size_t colon = gateway_addr_.find(':');
        if (colon != std::string::npos) {
            gw_host = gateway_addr_.substr(0, colon);
            gw_port = std::stoi(gateway_addr_.substr(colon + 1));
        }

        // 重试查询路由表（PyWorker 可能还没注册）
        for (int retry = 0; retry < 10; ++retry) {
            httplib::Client client(gw_host, gw_port);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);
            auto res = client.Get("/gateway/routes");
            if (res && res->status == 200) {
                rapidjson::Document doc;
                doc.Parse(res->body.c_str());
                if (!doc.HasParseError() && doc.IsArray()) {
                    for (auto& item : doc.GetArray()) {
                        std::string prefix = item["prefix"].GetString();
                        if (prefix.find("/operators/python") == 0) {
                            std::string addr = item["address"].GetString();
                            size_t c = addr.find(':');
                            if (c != std::string::npos) {
                                host_ = addr.substr(0, c);
                                port_ = std::stoi(addr.substr(c + 1));
                            }
                            printf("BridgePlugin::Load: discovered PyWorker at %s:%d via Gateway\n",
                                   host_.c_str(), port_);
                            goto found;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
found:
    printf("BridgePlugin::Load: worker=%s:%d, gateway=%s\n", host_.c_str(), port_,
           gateway_addr_.empty() ? "(none)" : gateway_addr_.c_str());
    return 0;
}

int BridgePlugin::Unload() {
    return 0;
}

int BridgePlugin::Start() {
    CleanupShmFiles("/dev/shm");
    CleanupShmFiles("/tmp");

    for (int retry = 0; retry < 30; ++retry) {
        if (DiscoverOperators() == 0) {
            printf("BridgePlugin::Start: %zu Python operators registered\n", registered_operators_.size());
            return 0;
        }
        printf("BridgePlugin::Start: waiting for Python Worker... (%d/30)\n", retry + 1);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    printf("BridgePlugin::Start: Python Worker not available, continuing without Python operators\n");
    return 0;
}

int BridgePlugin::Stop() {
    registered_operators_.clear();
    return 0;
}

int BridgePlugin::DiscoverOperators() {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);

    auto res = client.Get("/operators/python/list");
    if (!res || res->status != 200) {
        return -1;
    }

    rapidjson::Document doc;
    doc.Parse(res->body.c_str());
    if (doc.HasParseError() || !doc.IsArray()) {
        printf("BridgePlugin: invalid operators response\n");
        return -1;
    }

    for (auto& item : doc.GetArray()) {
        OperatorMeta meta;
        meta.catelog = item.HasMember("catelog") ? item["catelog"].GetString() : "";
        meta.name = item.HasMember("name") ? item["name"].GetString() : "";
        meta.description = item.HasMember("description") ? item["description"].GetString() : "";

        if (meta.catelog.empty() || meta.name.empty()) continue;

        auto bridge = std::make_shared<PythonOperatorBridge>(meta, host_, port_);
        std::string key = meta.catelog + "." + meta.name;

        // 只存内部，不注册到 PluginLoader
        registered_operators_.push_back(bridge);
        printf("BridgePlugin: Discovered operator [%s]\n", key.c_str());
    }

    return 0;
}

// IBridge::FindOperator — 按 catelog + name 查找 Python 算子
std::shared_ptr<IOperator> BridgePlugin::FindOperator(const std::string& catelog, const std::string& name) {
    for (auto& op : registered_operators_) {
        if (op->Catelog() == catelog && op->Name() == name) return op;
    }
    return nullptr;
}

// IBridge::TraverseOperators — 遍历所有已发现的 Python 算子
void BridgePlugin::TraverseOperators(std::function<int(IOperator*)> fn) {
    for (auto& op : registered_operators_) {
        if (fn(op.get()) == -1) break;
    }
}

// IBridge::Refresh — 重新从 Python Worker 发现算子
int BridgePlugin::Refresh() {
    registered_operators_.clear();
    return DiscoverOperators();
}

}  // namespace bridge
}  // namespace flowsql
