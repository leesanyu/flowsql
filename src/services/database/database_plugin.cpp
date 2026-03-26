#include "database_plugin.h"

#include <cstdio>
#include <common/error_code.h>
#include <common/log.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/compute/api.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <yaml-cpp/yaml.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "drivers/sqlite_driver.h"
#include "drivers/mysql_driver.h"
#include "drivers/clickhouse_driver.h"
#include "drivers/postgres_driver.h"

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
// 也支持 "config_file=/path/to/flowsql.yml" 格式
int DatabasePlugin::Option(const char* arg) {
    LOG_INFO("DatabasePlugin::Option called with: %s", arg ? arg : "(null)");

    if (!arg || !*arg) return 0;

    // config_file 参数：指定 YAML 持久化文件路径
    std::string s(arg);
    if (s.find("config_file=") == 0) {
        config_file_ = s.substr(12);
        LOG_INFO("DatabasePlugin::Option: config_file=%s", config_file_.c_str());
        return 0;
    }

    // 按 | 分隔多个配置
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('|', start);
        if (end == std::string::npos) end = s.size();

        std::string single_config = s.substr(start, end - start);
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

int DatabasePlugin::Start() {
    if (config_file_.empty()) return 0;
    return LoadFromYaml();
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
    auto* provider = dynamic_cast<IDbSessionFactoryProvider*>(driver_ptr);
    if (!provider) {
        last_error_ = "driver does not provide session factory: " + std::string(type);
        return nullptr;
    }
    auto session_factory = [provider]() -> std::shared_ptr<IDbSession> {
        return provider->CreateSession();
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

void DatabasePlugin::List(std::function<void(const char* type, const char* name,
                                              const char* config_json)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, params] : configs_) {
        auto type_it = params.find("type");
        auto name_it = params.find("name");
        if (type_it == params.end() || name_it == params.end()) continue;

        // 构造脱敏的 config JSON
        std::string json = "{";
        bool first = true;
        for (const auto& [k, v] : params) {
            if (!first) json += ",";
            json += "\"" + k + "\":\"";
            json += (k == "password") ? "****" : v;
            json += "\"";
            first = false;
        }
        json += "}";

        callback(type_it->second.c_str(), name_it->second.c_str(), json.c_str());
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
    if (type == "postgres") {
        return std::make_unique<PostgresDriver>();
    }
    if (type == "clickhouse") {
        return std::make_unique<ClickHouseDriver>();
    }
    // TODO: 未来添加 postgresql 等驱动
    last_error_ = "unsupported database type: " + type;
    return nullptr;
}

// ==================== Epic 6：动态管理方法 ====================

int DatabasePlugin::AddChannel(const char* config_str) {
    if (!config_str || !*config_str) {
        last_error_ = "config_str is empty";
        return -1;
    }
    if (config_file_.empty()) {
        last_error_ = "config_file not set, cannot persist channel";
        return -1;
    }

    // 解析 config_str，提取 type 和 name
    std::unordered_map<std::string, std::string> params;
    std::string opts(config_str);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();
        params[opts.substr(pos, eq - pos)] = opts.substr(eq + 1, end - eq - 1);
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }

    auto type_it = params.find("type");
    auto name_it = params.find("name");
    if (type_it == params.end() || name_it == params.end()) {
        last_error_ = "missing 'type' or 'name' in config_str";
        return -1;
    }

    std::string key = type_it->second + "." + name_it->second;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configs_.count(key)) {
            last_error_ = "channel already exists: " + key + " (use UpdateChannel to modify)";
            return -1;
        }
        configs_[key] = params;
    }

    if (SaveToYaml() != 0) {
        // 回滚内存
        std::lock_guard<std::mutex> lock(mutex_);
        configs_.erase(key);
        return -1;
    }

    LOG_INFO("DatabasePlugin::AddChannel: added %s", key.c_str());
    return 0;
}

int DatabasePlugin::RemoveChannel(const char* type, const char* name) {
    if (!type || !name) {
        last_error_ = "type and name must not be null";
        return -1;
    }
    if (config_file_.empty()) {
        last_error_ = "config_file not set, cannot persist";
        return -1;
    }

    std::string key = std::string(type) + "." + name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configs_.count(key)) {
            last_error_ = "channel not found: " + key;
            return -1;
        }
        // 关闭连接
        auto ch_it = channels_.find(key);
        if (ch_it != channels_.end()) {
            ch_it->second->Close();
            channels_.erase(ch_it);
        }
        driver_storage_.erase(key);
        configs_.erase(key);
    }

    if (SaveToYaml() != 0) return -1;

    LOG_INFO("DatabasePlugin::RemoveChannel: removed %s", key.c_str());
    return 0;
}

int DatabasePlugin::UpdateChannel(const char* config_str) {
    if (!config_str || !*config_str) {
        last_error_ = "config_str is empty";
        return -1;
    }
    if (config_file_.empty()) {
        last_error_ = "config_file not set, cannot persist";
        return -1;
    }

    std::unordered_map<std::string, std::string> params;
    std::string opts(config_str);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();
        params[opts.substr(pos, eq - pos)] = opts.substr(eq + 1, end - eq - 1);
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }

    auto type_it = params.find("type");
    auto name_it = params.find("name");
    if (type_it == params.end() || name_it == params.end()) {
        last_error_ = "missing 'type' or 'name' in config_str";
        return -1;
    }

    std::string key = type_it->second + "." + name_it->second;
    std::unordered_map<std::string, std::string> old_params;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configs_.count(key)) {
            last_error_ = "channel not found: " + key + " (use AddChannel to create)";
            return -1;
        }
        old_params = configs_[key];
        // 关闭旧连接，下次 Get() 时重建
        auto ch_it = channels_.find(key);
        if (ch_it != channels_.end()) {
            ch_it->second->Close();
            channels_.erase(ch_it);
        }
        driver_storage_.erase(key);
        configs_[key] = params;
    }

    if (SaveToYaml() != 0) {
        // 回滚
        std::lock_guard<std::mutex> lock(mutex_);
        configs_[key] = old_params;
        return -1;
    }

    LOG_INFO("DatabasePlugin::UpdateChannel: updated %s", key.c_str());
    return 0;
}

// ==================== YAML 持久化 ====================

int DatabasePlugin::LoadFromYaml() {
    if (access(config_file_.c_str(), F_OK) != 0) {
        LOG_INFO("DatabasePlugin::LoadFromYaml: %s not found, skipping", config_file_.c_str());
        return 0;  // 首次启动，文件不存在
    }

    try {
        YAML::Node root = YAML::LoadFile(config_file_);
        auto db_channels = root["channels"]["database_channels"];
        if (!db_channels || !db_channels.IsSequence()) return 0;

        for (const auto& ch : db_channels) {
            std::string config_str;
            for (const auto& kv : ch) {
                std::string key = kv.first.as<std::string>();
                std::string val = kv.second.as<std::string>();
                if (key == "password") val = DecryptPassword(val);
                if (!config_str.empty()) config_str += ";";
                config_str += key + "=" + val;
            }
            if (!config_str.empty()) {
                ParseSingleConfig(config_str.c_str());
            }
        }
        LOG_INFO("DatabasePlugin::LoadFromYaml: loaded %zu channel(s) from %s",
                 configs_.size(), config_file_.c_str());
    } catch (const std::exception& e) {
        LOG_ERROR("DatabasePlugin::LoadFromYaml: failed to parse %s: %s",
                  config_file_.c_str(), e.what());
        return -1;
    }
    return 0;
}

int DatabasePlugin::SaveToYaml() {
    // save_mutex_ 保证并发 Add/Remove/Update 时文件写入串行化
    // 锁内重新快照 configs_，确保写入的是最新状态而非调用时的过期快照
    std::lock_guard<std::mutex> save_lock(save_mutex_);

    YAML::Node root;
    // 先读取现有文件，保留其他顶层节点（operators 等）
    if (access(config_file_.c_str(), F_OK) == 0) {
        try {
            root = YAML::LoadFile(config_file_);
        } catch (...) {
            // 文件损坏，从空节点开始
        }
    }

    // 重建 channels.database_channels 节点（在 save_mutex_ 内重新快照）
    YAML::Node db_channels(YAML::NodeType::Sequence);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, params] : configs_) {
            YAML::Node ch;
            auto type_it = params.find("type");
            auto name_it = params.find("name");
            if (type_it != params.end()) ch["type"] = type_it->second;
            if (name_it != params.end()) ch["name"] = name_it->second;
            for (const auto& [k, v] : params) {
                if (k == "type" || k == "name") continue;
                ch[k] = (k == "password") ? EncryptPassword(v) : v;
            }
            db_channels.push_back(ch);
        }
    }
    root["channels"]["database_channels"] = db_channels;

    std::ofstream fout(config_file_);
    if (!fout.is_open()) {
        last_error_ = "failed to open " + config_file_ + " for writing";
        return -1;
    }
    fout << root;
    return fout.good() ? 0 : -1;
}

// ==================== 密码 AES-256-GCM 加解密 ====================
// 密钥从环境变量 FLOWSQL_SECRET_KEY 读取（32字节），未设置时使用内置开发密钥
// 存储格式：ENC:<base64(iv[12] + tag[16] + ciphertext)>

static const unsigned char* GetSecretKey() {
    static unsigned char key[32];
    static bool initialized = false;
    if (!initialized) {
        const char* env_key = getenv("FLOWSQL_SECRET_KEY");
        if (env_key && strlen(env_key) >= 32) {
            memcpy(key, env_key, 32);
        } else {
            // 开发用固定密钥（生产环境必须设置 FLOWSQL_SECRET_KEY）
            const char* dev_key = "flowsql_dev_key_0000000000000000";
            memcpy(key, dev_key, 32);
        }
        initialized = true;
    }
    return key;
}

// base64 编码
static std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (data[i] << 16);
        if (i + 1 < len) b |= (data[i+1] << 8);
        if (i + 2 < len) b |= data[i+2];
        result += table[(b >> 18) & 0x3f];
        result += table[(b >> 12) & 0x3f];
        result += (i + 1 < len) ? table[(b >> 6) & 0x3f] : '=';
        result += (i + 2 < len) ? table[b & 0x3f] : '=';
    }
    return result;
}

// base64 解码
static std::vector<unsigned char> Base64Decode(const std::string& s) {
    static const int table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<unsigned char> result;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        int d = table[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            result.push_back((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    return result;
}

std::string DatabasePlugin::EncryptPassword(const std::string& plain) {
    if (plain.empty()) return plain;

    const unsigned char* key = GetSecretKey();
    unsigned char iv[12];
    RAND_bytes(iv, sizeof(iv));

    std::vector<unsigned char> ciphertext(plain.size() + 16);
    unsigned char tag[16];
    int out_len = 0, final_len = 0;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv);
    EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len,
                      reinterpret_cast<const unsigned char*>(plain.data()), plain.size());
    EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &final_len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    // 拼接 iv + tag + ciphertext
    std::vector<unsigned char> blob;
    blob.insert(blob.end(), iv, iv + 12);
    blob.insert(blob.end(), tag, tag + 16);
    blob.insert(blob.end(), ciphertext.begin(), ciphertext.begin() + out_len + final_len);

    return "ENC:" + Base64Encode(blob.data(), blob.size());
}

std::string DatabasePlugin::DecryptPassword(const std::string& cipher) {
    if (cipher.substr(0, 4) != "ENC:") return cipher;  // 未加密，直接返回

    auto blob = Base64Decode(cipher.substr(4));
    if (blob.size() < 12 + 16) return "";  // 数据损坏

    const unsigned char* key = GetSecretKey();
    unsigned char iv[12];
    unsigned char tag[16];
    memcpy(iv, blob.data(), 12);
    memcpy(tag, blob.data() + 12, 16);
    size_t ct_len = blob.size() - 28;

    std::vector<unsigned char> plain(ct_len);
    int out_len = 0, final_len = 0;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv);
    EVP_DecryptUpdate(ctx, plain.data(), &out_len, blob.data() + 28, ct_len);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    int ok = EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);

    if (ok <= 0) return "";  // 认证失败（密钥错误或数据篡改）
    return std::string(reinterpret_cast<char*>(plain.data()), out_len + final_len);
}

// ==================== IRouterHandle — /channels/database/* ====================

void DatabasePlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    cb({"POST", "/channels/database/add",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleAdd(u, req, rsp);
        }});
    cb({"POST", "/channels/database/remove",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRemove(u, req, rsp);
        }});
    cb({"POST", "/channels/database/modify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleModify(u, req, rsp);
        }});
    cb({"POST", "/channels/database/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQuery(u, req, rsp);
        }});
    cb({"POST", "/channels/database/tables",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleTables(u, req, rsp);
        }});
    cb({"POST", "/channels/database/describe",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleDescribe(u, req, rsp);
        }});
    cb({"POST", "/channels/database/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreview(u, req, rsp);
        }});
}

// POST /channels/database/add — 新增数据库通道
// Body: {"config":"type=mysql;name=mydb;host=..."}
int32_t DatabasePlugin::HandleAdd(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("config")) {
        rsp = R"({"error":"invalid request, expected {\"config\":\"...\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string config = doc["config"].GetString();
    if (AddChannel(config.c_str()) != 0) {
        rsp = "{\"error\":\"" + std::string(LastError()) + "\"}";
        return error::BAD_REQUEST;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

// POST /channels/database/remove — 删除数据库通道
// Body: {"type":"mysql","name":"mydb"}
int32_t DatabasePlugin::HandleRemove(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("type") || !doc.HasMember("name")) {
        rsp = R"({"error":"invalid request, expected {\"type\":\"...\",\"name\":\"...\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string type = doc["type"].GetString();
    std::string name = doc["name"].GetString();
    if (RemoveChannel(type.c_str(), name.c_str()) != 0) {
        rsp = "{\"error\":\"" + std::string(LastError()) + "\"}";
        return error::BAD_REQUEST;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

// POST /channels/database/modify — 修改数据库通道配置
// Body: {"config":"type=mysql;name=mydb;host=..."}
int32_t DatabasePlugin::HandleModify(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("config")) {
        rsp = R"({"error":"invalid request, expected {\"config\":\"...\"}"})" ;
        return error::BAD_REQUEST;
    }
    std::string config = doc["config"].GetString();
    if (UpdateChannel(config.c_str()) != 0) {
        rsp = "{\"error\":\"" + std::string(LastError()) + "\"}";
        return error::BAD_REQUEST;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

// POST /channels/database/query — 查询数据库通道列表
// Body: {} 全部 / {"type":"mysql","name":"mydb"} 单个
int32_t DatabasePlugin::HandleQuery(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    // 允许空 body 或空对象
    std::string filter_type, filter_name;
    if (!doc.HasParseError() && doc.IsObject()) {
        if (doc.HasMember("type") && doc["type"].IsString()) filter_type = doc["type"].GetString();
        if (doc.HasMember("name") && doc["name"].IsString()) filter_name = doc["name"].GetString();
    }

    std::string body = "[";
    bool first = true;
    List([&](const char* type, const char* name, const char* config_json) {
        if (!filter_type.empty() && filter_type != type) return;
        if (!filter_name.empty() && filter_name != name) return;
        if (!first) body += ",";
        body += config_json ? config_json : "{}";
        first = false;
    });
    body += "]";
    rsp = body;
    return error::OK;
}

// ==================== 浏览器端点 ====================

// 将 IBatchReader 读取的所有行转为 JSON
// 格式：{"columns":[...],"types":[...],"data":[[...],...],"rows":N}
static std::string BatchReaderToJson(IBatchReader* reader) {
    // 通过 IPC 反序列化收集所有 RecordBatch
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::shared_ptr<arrow::Schema> schema;
    const uint8_t* buf = nullptr;
    size_t len = 0;

    while (true) {
        int rc = reader->Next(&buf, &len);
        if (rc == 1) break;   // 已读完
        if (rc < 0) break;    // 错误

        auto arrow_buf = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
        auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (!stream_result.ok()) break;
        auto stream_reader = *stream_result;
        if (!schema) schema = stream_reader->schema();

        std::shared_ptr<arrow::RecordBatch> batch;
        while (stream_reader->ReadNext(&batch).ok() && batch) {
            batches.push_back(batch);
        }
    }

    if (batches.empty() || !schema) {
        return R"({"columns":[],"types":[],"data":[],"rows":0})";
    }

    rapidjson::StringBuffer buf_json;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf_json);
    w.StartObject();

    // columns
    w.Key("columns"); w.StartArray();
    for (int i = 0; i < schema->num_fields(); ++i) {
        w.String(schema->field(i)->name().c_str());
    }
    w.EndArray();

    // types
    w.Key("types"); w.StartArray();
    for (int i = 0; i < schema->num_fields(); ++i) {
        w.String(schema->field(i)->type()->ToString().c_str());
    }
    w.EndArray();

    // data
    int64_t total_rows = 0;
    w.Key("data"); w.StartArray();
    for (const auto& batch : batches) {
        for (int64_t r = 0; r < batch->num_rows(); ++r) {
            w.StartArray();
            for (int c = 0; c < batch->num_columns(); ++c) {
                auto scalar_result = batch->column(c)->GetScalar(r);
                if (!scalar_result.ok()) { w.Null(); continue; }
                auto scalar = scalar_result.ValueOrDie();
                if (!scalar->is_valid) { w.Null(); continue; }
                auto type_id = scalar->type->id();
                if (type_id == arrow::Type::BOOL) {
                    w.Bool(std::static_pointer_cast<arrow::BooleanScalar>(scalar)->value);
                } else if (type_id == arrow::Type::INT8  || type_id == arrow::Type::INT16 ||
                           type_id == arrow::Type::INT32 || type_id == arrow::Type::INT64 ||
                           type_id == arrow::Type::UINT8 || type_id == arrow::Type::UINT16 ||
                           type_id == arrow::Type::UINT32|| type_id == arrow::Type::UINT64) {
                    auto cast = arrow::compute::Cast(scalar, arrow::int64());
                    if (cast.ok()) {
                        w.Int64(cast.ValueOrDie().scalar_as<arrow::Int64Scalar>().value);
                    } else {
                        w.String(scalar->ToString().c_str());
                    }
                } else if (type_id == arrow::Type::FLOAT || type_id == arrow::Type::DOUBLE) {
                    auto cast = arrow::compute::Cast(scalar, arrow::float64());
                    if (cast.ok()) {
                        w.Double(cast.ValueOrDie().scalar_as<arrow::DoubleScalar>().value);
                    } else {
                        w.String(scalar->ToString().c_str());
                    }
                } else {
                    w.String(scalar->ToString().c_str());
                }
            }
            w.EndArray();
            ++total_rows;
        }
    }
    w.EndArray();

    w.Key("rows"); w.Int64(total_rows);
    w.EndObject();
    return buf_json.GetString();
}

// 按数据库类型对表名加引号转义（防止 SQL 注入）
// SQLite/PostgreSQL 用双引号，MySQL/ClickHouse 用反引号
static std::string QuoteTableName(const std::string& table, const std::string& db_type) {
    if (table.empty()) return "";
    if (db_type == "sqlite" || db_type == "postgres") {
        // 双引号转义：将内部双引号替换为两个双引号
        std::string result = "\"";
        for (char c : table) {
            if (c == '"') result += "\"\"";
            else result += c;
        }
        result += "\"";
        return result;
    } else {
        // MySQL / ClickHouse：反引号转义
        std::string result = "`";
        for (char c : table) {
            if (c == '`') result += "``";
            else result += c;
        }
        result += "`";
        return result;
    }
}

static std::string QuoteSqlLiteral(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string BuildTablesSql(const std::string& db_type, const std::string& schema) {
    if (db_type == "sqlite") {
        return "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name";
    }
    if (db_type == "postgres") {
        const std::string effective_schema = schema.empty() ? "public" : schema;
        return "SELECT tablename FROM pg_tables WHERE schemaname=" +
               QuoteSqlLiteral(effective_schema) + " ORDER BY tablename";
    }
    return "SHOW TABLES";
}

static std::string BuildDescribeSql(const std::string& db_type,
                                    const std::string& schema,
                                    const std::string& table) {
    const std::string quoted = QuoteTableName(table, db_type);
    if (db_type == "sqlite") {
        return "PRAGMA table_info(" + quoted + ")";
    }
    if (db_type == "postgres") {
        const std::string effective_schema = schema.empty() ? "public" : schema;
        return "SELECT column_name, data_type, is_nullable "
               "FROM information_schema.columns "
               "WHERE table_schema=" + QuoteSqlLiteral(effective_schema) +
               " AND table_name=" + QuoteSqlLiteral(table) +
               " ORDER BY ordinal_position";
    }
    return "DESCRIBE " + quoted;
}

static std::string BuildPreviewSql(const std::string& db_type,
                                   const std::string& schema,
                                   const std::string& table) {
    if (db_type == "postgres") {
        const std::string effective_schema = schema.empty() ? "public" : schema;
        return "SELECT * FROM " + QuoteTableName(effective_schema, db_type) + "." +
               QuoteTableName(table, db_type) + " LIMIT 100";
    }
    return "SELECT * FROM " + QuoteTableName(table, db_type) + " LIMIT 100";
}

// 将 RecordBatch 列表转为 JSON（供 ClickHouse Arrow 路径使用）
static std::string BatchesToJson(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    if (batches.empty()) {
        return R"({"columns":[],"types":[],"data":[],"rows":0})";
    }
    auto schema = batches[0]->schema();
    rapidjson::StringBuffer buf_json;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf_json);
    w.StartObject();
    w.Key("columns"); w.StartArray();
    for (int i = 0; i < schema->num_fields(); ++i) w.String(schema->field(i)->name().c_str());
    w.EndArray();
    w.Key("types"); w.StartArray();
    for (int i = 0; i < schema->num_fields(); ++i) w.String(schema->field(i)->type()->ToString().c_str());
    w.EndArray();
    int64_t total_rows = 0;
    w.Key("data"); w.StartArray();
    for (const auto& batch : batches) {
        for (int64_t r = 0; r < batch->num_rows(); ++r) {
            w.StartArray();
            for (int c = 0; c < batch->num_columns(); ++c) {
                auto scalar_result = batch->column(c)->GetScalar(r);
                if (!scalar_result.ok()) { w.Null(); continue; }
                auto scalar = scalar_result.ValueOrDie();
                if (!scalar->is_valid) { w.Null(); continue; }
                auto type_id = scalar->type->id();
                if (type_id == arrow::Type::BOOL) {
                    w.Bool(std::static_pointer_cast<arrow::BooleanScalar>(scalar)->value);
                } else if (type_id == arrow::Type::INT8  || type_id == arrow::Type::INT16 ||
                           type_id == arrow::Type::INT32 || type_id == arrow::Type::INT64 ||
                           type_id == arrow::Type::UINT8 || type_id == arrow::Type::UINT16 ||
                           type_id == arrow::Type::UINT32|| type_id == arrow::Type::UINT64) {
                    auto cast = arrow::compute::Cast(scalar, arrow::int64());
                    if (cast.ok()) w.Int64(cast.ValueOrDie().scalar_as<arrow::Int64Scalar>().value);
                    else w.String(scalar->ToString().c_str());
                } else if (type_id == arrow::Type::FLOAT || type_id == arrow::Type::DOUBLE) {
                    auto cast = arrow::compute::Cast(scalar, arrow::float64());
                    if (cast.ok()) w.Double(cast.ValueOrDie().scalar_as<arrow::DoubleScalar>().value);
                    else w.String(scalar->ToString().c_str());
                } else {
                    w.String(scalar->ToString().c_str());
                }
            }
            w.EndArray();
            ++total_rows;
        }
    }
    w.EndArray();
    w.Key("rows"); w.Int64(total_rows);
    w.EndObject();
    return buf_json.GetString();
}

// 解析请求中的 type/name/table 字段
static bool ParseBrowseRequest(const std::string& req,
                               std::string& type,
                               std::string& name,
                               std::string& table,
                               std::string& schema,
                               std::string& err) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        err = "invalid JSON"; return false;
    }
    if (!doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        err = "missing 'type' or 'name'"; return false;
    }
    type  = doc["type"].GetString();
    name  = doc["name"].GetString();
    if (doc.HasMember("table") && doc["table"].IsString()) {
        table = doc["table"].GetString();
    }
    if (doc.HasMember("schema") && doc["schema"].IsString()) {
        schema = doc["schema"].GetString();
    }
    return true;
}

// POST /channels/database/tables — 列出所有表
int32_t DatabasePlugin::HandleTables(const std::string&, const std::string& req, std::string& rsp) {
    std::string type, name, table, schema, err;
    if (!ParseBrowseRequest(req, type, name, table, schema, err)) {
        rsp = "{\"error\":\"" + err + "\"}";
        return error::BAD_REQUEST;
    }

    IDatabaseChannel* ch = Get(type.c_str(), name.c_str());
    if (!ch) {
        rsp = "{\"error\":\"channel not found: " + type + "." + name + "\"}";
        return error::NOT_FOUND;
    }

    const std::string sql = BuildTablesSql(type, schema);

    std::vector<std::string> tables;

    if (type == "clickhouse") {
        // ClickHouse 走 Arrow 路径
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (ch->ExecuteQueryArrow(sql.c_str(), &batches) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        for (const auto& batch : batches) {
            if (batch->num_columns() == 0) continue;
            auto col = batch->column(0);
            for (int64_t i = 0; i < col->length(); ++i) {
                auto s = col->GetScalar(i);
                if (s.ok() && s.ValueOrDie()->is_valid) tables.push_back(s.ValueOrDie()->ToString());
            }
        }
    } else {
        // SQLite / MySQL 走 IBatchReader 路径
        IBatchReader* reader = nullptr;
        if (ch->CreateReader(sql.c_str(), &reader) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        std::unique_ptr<IBatchReader, std::function<void(IBatchReader*)>> guard(
            reader, [](IBatchReader* r) { r->Close(); r->Release(); });

        const uint8_t* ibuf = nullptr;
        size_t ilen = 0;
        while (true) {
            int rc = reader->Next(&ibuf, &ilen);
            if (rc == 1) break;
            if (rc < 0) break;
            auto arrow_buf = arrow::Buffer::Wrap(ibuf, static_cast<int64_t>(ilen));
            auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
            auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
            if (!stream_result.ok()) break;
            std::shared_ptr<arrow::RecordBatch> batch;
            while ((*stream_result)->ReadNext(&batch).ok() && batch) {
                if (batch->num_columns() == 0) continue;
                auto col = batch->column(0);
                for (int64_t i = 0; i < col->length(); ++i) {
                    auto s = col->GetScalar(i);
                    if (s.ok() && s.ValueOrDie()->is_valid) tables.push_back(s.ValueOrDie()->ToString());
                }
            }
        }
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("tables"); w.StartArray();
    for (const auto& t : tables) w.String(t.c_str());
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// POST /channels/database/describe — 获取表结构
int32_t DatabasePlugin::HandleDescribe(const std::string&, const std::string& req, std::string& rsp) {
    std::string type, name, table, schema, err;
    if (!ParseBrowseRequest(req, type, name, table, schema, err)) {
        rsp = "{\"error\":\"" + err + "\"}";
        return error::BAD_REQUEST;
    }
    if (table.empty()) {
        rsp = R"({"error":"missing 'table' parameter"})";
        return error::BAD_REQUEST;
    }

    IDatabaseChannel* ch = Get(type.c_str(), name.c_str());
    if (!ch) {
        rsp = "{\"error\":\"channel not found: " + type + "." + name + "\"}";
        return error::NOT_FOUND;
    }

    const std::string sql = BuildDescribeSql(type, schema, table);

    if (type == "clickhouse") {
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (ch->ExecuteQueryArrow(sql.c_str(), &batches) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        rsp = BatchesToJson(batches);
    } else {
        IBatchReader* reader = nullptr;
        if (ch->CreateReader(sql.c_str(), &reader) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        std::unique_ptr<IBatchReader, std::function<void(IBatchReader*)>> guard(
            reader, [](IBatchReader* r) { r->Close(); r->Release(); });
        rsp = BatchReaderToJson(reader);
    }
    return error::OK;
}

// POST /channels/database/preview — 预览前 100 条数据
int32_t DatabasePlugin::HandlePreview(const std::string&, const std::string& req, std::string& rsp) {
    std::string type, name, table, schema, err;
    if (!ParseBrowseRequest(req, type, name, table, schema, err)) {
        rsp = "{\"error\":\"" + err + "\"}";
        return error::BAD_REQUEST;
    }
    if (table.empty()) {
        rsp = R"({"error":"missing 'table' parameter"})";
        return error::BAD_REQUEST;
    }

    IDatabaseChannel* ch = Get(type.c_str(), name.c_str());
    if (!ch) {
        rsp = "{\"error\":\"channel not found: " + type + "." + name + "\"}";
        return error::NOT_FOUND;
    }

    const std::string sql = BuildPreviewSql(type, schema, table);

    if (type == "clickhouse") {
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (ch->ExecuteQueryArrow(sql.c_str(), &batches) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        rsp = BatchesToJson(batches);
    } else {
        IBatchReader* reader = nullptr;
        if (ch->CreateReader(sql.c_str(), &reader) != 0) {
            rsp = "{\"error\":\"" + std::string(ch->GetLastError()) + "\"}";
            return error::INTERNAL_ERROR;
        }
        std::unique_ptr<IBatchReader, std::function<void(IBatchReader*)>> guard(
            reader, [](IBatchReader* r) { r->Close(); r->Release(); });
        rsp = BatchReaderToJson(reader);
    }
    return error::OK;
}

}  // namespace database
}  // namespace flowsql
