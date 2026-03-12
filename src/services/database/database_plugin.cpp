#include "database_plugin.h"

#include <cstdio>
#include <common/log.h>
#include <cstdlib>
#include <cstring>

#include "drivers/sqlite_driver.h"
#include "drivers/mysql_driver.h"
#include "drivers/clickhouse_driver.h"

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
    LOG_INFO("DatabasePlugin::Option called with: %s", arg ? arg : "(null)");

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
        LOG_INFO("DatabasePlugin::ParseSingleConfig: missing 'type' or 'name' in: %s", arg);
        return -1;
    }

    std::string key = type_it->second + "." + name_it->second;

    std::lock_guard<std::mutex> lock(mutex_);
    configs_[key] = std::move(params);
    LOG_INFO("DatabasePlugin::ParseSingleConfig: configured %s (total configs: %zu)", key.c_str(), configs_.size());
    return 0;
}

int DatabasePlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("DatabasePlugin::Load: %zu database(s) configured", configs_.size());
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
    LOG_INFO("DatabasePlugin::Stop: all connections closed");
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
            LOG_INFO("DatabasePlugin: connection lost for %s, reconnecting...", key.c_str());
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

    // 3. 创建驱动并连接到连接池
    auto driver = CreateDriver(type);
    if (!driver) {
        last_error_ = "unsupported database type: " + std::string(type);
        return nullptr;
    }

    // 使用驱动参数连接（初始化连接池）
    if (driver->Connect(cfg_it->second) != 0) {
        last_error_ = "connection failed: " + key;
        return nullptr;
    }

    // 4. 创建 Session 工厂
    IDbDriver* driver_ptr = driver.get();  // 弱引用，driver 由 channels_ 中的 Channel 持有
    auto session_factory = [driver_ptr]() -> std::shared_ptr<IDbSession> {
        // 使用 dynamic_cast 调用具体驱动的 CreateSession
        if (auto* d = dynamic_cast<SqliteDriver*>(driver_ptr))      return d->CreateSession();
        if (auto* d = dynamic_cast<MysqlDriver*>(driver_ptr))       return d->CreateSession();
        if (auto* d = dynamic_cast<ClickHouseDriver*>(driver_ptr))  return d->CreateSession();
        return nullptr;
    };

    // 5. 创建通道并打开连接
    auto channel = std::make_shared<DatabaseChannel>(type, name, driver_ptr, session_factory);
    if (channel->Open() != 0) {
        last_error_ = "connection failed: " + key;
        return nullptr;
    }

    // 6. 保存驱动所有权到 Channel（通过一个成员变量）
    // 这里需要将驱动附加到 Channel 上
    // 为简化，我们将 driver 释放并通过 channel 内部管理
    // 实际上，driver 的生命周期应该与 channel 绑定
    // 这里使用一个简单的方案：将 driver 存储在一个全局映射中
    driver_storage_[key] = std::move(driver);

    // 7. 加入通道池
    channels_[key] = channel;
    LOG_INFO("DatabasePlugin: connected to %s", key.c_str());
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
    driver_storage_.erase(key);
    LOG_INFO("DatabasePlugin: released %s", key.c_str());
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
    if (type == "clickhouse") {
        return std::make_unique<ClickHouseDriver>();
    }
    // TODO: 未来添加 postgresql 等驱动
    last_error_ = "unsupported database type: " + type;
    return nullptr;
}

}  // namespace database
}  // namespace flowsql
