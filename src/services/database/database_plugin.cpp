#include "database_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drivers/sqlite_driver.h"
#include "drivers/mysql_driver.h"

namespace flowsql {
namespace database {

thread_local std::string DatabasePlugin::last_error_;

// 替换字符串中的 ${VAR} 为环境变量值
static std::string ExpandEnvVars(const std::string& input) {
    std::string result;
    size_t pos = 0;
    while (pos < input.size()) {
        if (pos + 1 < input.size() && input[pos] == '$' && input[pos + 1] == '{') {
            size_t end = input.find('}', pos + 2);
            if (end != std::string::npos) {
                std::string var_name = input.substr(pos + 2, end - pos - 2);
                const char* val = std::getenv(var_name.c_str());
                result += (val ? val : "");
                pos = end + 1;
                continue;
            }
        }
        result += input[pos++];
    }
    return result;
}

// 解析 "type=sqlite;name=mydb;path=/data/test.db" 格式的配置
// 支持多个配置用 | 分隔：config1|config2|config3
int DatabasePlugin::Option(const char* arg) {
    printf("DatabasePlugin::Option called with: %s\n", arg ? arg : "(null)");

    if (!arg || !*arg) return 0;

    // 按 | 分隔多个配置
    std::string all_configs(arg);
    size_t start = 0;
    while (start < all_configs.size()) {
        size_t end = all_configs.find('|', start);
        if (end == std::string::npos) end = all_configs.size();

        std::string single_config = all_configs.substr(start, end - start);
        if (!single_config.empty()) {
            if (ParseSingleConfig(single_config.c_str()) != 0) {
                return -1;
            }
        }

        start = end + 1;
    }

    return 0;
}

// 解析单个数据库配置
int DatabasePlugin::ParseSingleConfig(const char* arg) {
    std::unordered_map<std::string, std::string> params;
    std::string opts(arg);
    size_t pos = 0;

    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);
        // 支持 ${VAR} 环境变量替换
        params[key] = ExpandEnvVars(val);

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }

    // 必须有 type 和 name
    auto type_it = params.find("type");
    auto name_it = params.find("name");
    if (type_it == params.end() || name_it == params.end()) {
        printf("DatabasePlugin::ParseSingleConfig: missing 'type' or 'name' in: %s\n", arg);
        return -1;
    }

    std::string key = type_it->second + "." + name_it->second;

    std::lock_guard<std::mutex> lock(mutex_);
    configs_[key] = std::move(params);
    printf("DatabasePlugin::ParseSingleConfig: configured %s (total configs: %zu)\n", key.c_str(), configs_.size());
    return 0;
}

int DatabasePlugin::Load(IQuerier* querier) {
    querier_ = querier;
    printf("DatabasePlugin::Load: %zu database(s) configured\n", configs_.size());
    return 0;
}

int DatabasePlugin::Unload() {
    Stop();
    return 0;
}

int DatabasePlugin::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, ch] : channels_) {
        ch->Close();
    }
    channels_.clear();
    printf("DatabasePlugin::Stop: all connections closed\n");
    return 0;
}

// 懒加载获取通道（含断线重连）
IDatabaseChannel* DatabasePlugin::Get(const char* type, const char* name) {
    if (!type || !name) {
        last_error_ = "type and name must not be null";
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = std::string(type) + "." + name;

    // 1. 查找已存在的通道
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        // 检查连接是否仍然有效，断开则移除并重建
        if (!it->second->IsConnected()) {
            printf("DatabasePlugin: connection lost for %s, reconnecting...\n", key.c_str());
            channels_.erase(it);
        } else {
            return it->second.get();
        }
    }

    // 2. 查找配置
    auto cfg_it = configs_.find(key);
    if (cfg_it == configs_.end()) {
        last_error_ = "database not configured: " + key;
        return nullptr;
    }

    // 3. 创建驱动
    auto driver = CreateDriver(type);
    if (!driver) {
        last_error_ = "unsupported database type: " + std::string(type);
        return nullptr;
    }

    // 4. 创建通道并打开连接
    auto channel = std::make_shared<DatabaseChannel>(type, name, std::move(driver), cfg_it->second);
    if (channel->Open() != 0) {
        last_error_ = "connection failed: " + key;
        return nullptr;
    }

    // 5. 加入通道池
    channels_[key] = channel;
    printf("DatabasePlugin: connected to %s\n", key.c_str());
    return channel.get();
}

void DatabasePlugin::List(std::function<void(const char* type, const char* name)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, params] : configs_) {
        auto type_it = params.find("type");
        auto name_it = params.find("name");
        if (type_it != params.end() && name_it != params.end()) {
            callback(type_it->second.c_str(), name_it->second.c_str());
        }
    }
}

int DatabasePlugin::Release(const char* type, const char* name) {
    if (!type || !name) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = std::string(type) + "." + name;

    auto it = channels_.find(key);
    if (it == channels_.end()) return -1;

    it->second->Close();
    channels_.erase(it);
    printf("DatabasePlugin: released %s\n", key.c_str());
    return 0;
}

const char* DatabasePlugin::LastError() {
    return last_error_.c_str();
}

// 驱动工厂
std::unique_ptr<IDbDriver> DatabasePlugin::CreateDriver(const std::string& type) {
    if (type == "sqlite") {
        return std::make_unique<SqliteDriver>();
    }
    if (type == "mysql") {
        return std::make_unique<MysqlDriver>();
    }
    // TODO: 未来添加 postgresql, clickhouse 等驱动
    last_error_ = "unsupported database type: " + type;
    return nullptr;
}

}  // namespace database
}  // namespace flowsql
