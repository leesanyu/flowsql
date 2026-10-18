// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_plugin.h"
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace flowsql::channels::kafka {
namespace {
void CheckDeployment(bool condition, const char* code) {
    if (!condition) throw std::runtime_error(code);
}
struct Descriptor {
    int value;
    ~Descriptor() {
        if (value >= 0) close(value);
    }
};
void PrivateDirectory(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    struct stat info {};
    CheckDeployment(lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid(),
                    "DEFAULT_STATE_DIRECTORY_INVALID");
    CheckDeployment(chmod(path.c_str(), 0700) == 0 && stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700,
                    "DEFAULT_STATE_REQUIRES_PRIVATE_FILESYSTEM");
}
void PrivateFile(const std::filesystem::path& path, const std::string& value) {
    Descriptor file{open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    CheckDeployment(file.value >= 0, "DEFAULT_FILE_CREATE_FAILED");
    size_t offset = 0;
    while (offset < value.size()) {
        const auto written = write(file.value, value.data() + offset, value.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        CheckDeployment(written > 0, "DEFAULT_FILE_WRITE_FAILED");
        offset += static_cast<size_t>(written);
    }
    CheckDeployment(fsync(file.value) == 0, "DEFAULT_FILE_SYNC_FAILED");
}
std::string ReadPrivateFile(const std::filesystem::path& path) {
    struct stat info {};
    CheckDeployment(lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                        (info.st_mode & 0077) == 0,
                    "DEFAULT_FILE_MISSING_OR_NOT_PRIVATE");
    std::ifstream file(path, std::ios::binary);
    CheckDeployment(file.good(), "DEFAULT_FILE_UNREADABLE");
    return {std::istreambuf_iterator<char>(file), {}};
}
std::string DefaultConfiguration(int* owner) {
    std::filesystem::path root;
    const auto environment = [](const char* name) {
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    };
    if (auto configured = environment("FLOWSQL_KAFKA_STATE_DIR"); !configured.empty())
        root = configured;
    else if (auto state = environment("XDG_STATE_HOME"); !state.empty())
        root = std::filesystem::path(state) / "flowsql/kafka";
    else {
        const auto home = environment("HOME");
        CheckDeployment(!home.empty(), "DEFAULT_STATE_HOME_MISSING");
        root = std::filesystem::path(home) / ".local/state/flowsql/kafka";
    }
    CheckDeployment(root.is_absolute(), "DEFAULT_STATE_PATH_MUST_BE_ABSOLUTE");
    PrivateDirectory(root);
    Descriptor lock{open((root / ".owner.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)};
    CheckDeployment(lock.value >= 0 && flock(lock.value, LOCK_EX | LOCK_NB) == 0, "DEFAULT_STATE_ALREADY_IN_USE");
    for (const auto* directory : {"config", "config/kafka", "secrets", "meta", "runtime", "runtime/kafka"})
        PrivateDirectory(root / directory);
    const auto configuration = root / "config/kafka/provider.json";
    const auto key_path = root / "secrets/kafka-master.key";
    const auto db_path = root / "meta/kafka.db";
    if (!std::filesystem::exists(configuration)) {
        CheckDeployment(!std::filesystem::exists(db_path), "DEFAULT_CONFIG_MISSING_FOR_EXISTING_DATABASE");
        auto options = Parse("{}");
        auto& allocator = options.GetAllocator();
        String(options, "db_path", db_path.string(), allocator);
        String(options, "master_key_file", key_path.string(), allocator);
        String(options, "runtime_directory", (root / "runtime/kafka").string(), allocator);
        options.AddMember("files", rapidjson::Value(rapidjson::kObjectType), allocator);
        options.AddMember("environments", rapidjson::Value(rapidjson::kObjectType), allocator);
        PrivateFile(configuration, Encode(options) + "\n");
    }
    const auto content = ReadPrivateFile(configuration);
    auto options = Parse(content);
    for (const char* field : {"db_path", "master_key_file", "runtime_directory"})
        CheckDeployment(std::filesystem::path(Text(options, field)).is_absolute(),
                        "DEFAULT_CONFIG_PATH_MUST_BE_ABSOLUTE");
    const auto configured_key = std::filesystem::path(Text(options, "master_key_file"));
    if (!std::filesystem::exists(configured_key)) {
        CheckDeployment(configured_key == key_path && Text(options, "db_path") == db_path.string() &&
                            !std::filesystem::exists(db_path),
                        "MASTER_KEY_MISSING_FOR_EXISTING_DEPLOYMENT");
        std::string key(32, '\0');
        CheckDeployment(RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), key.size()) == 1,
                        "MASTER_KEY_GENERATION_FAILED");
        PrivateFile(key_path, key);
        OPENSSL_cleanse(key.data(), key.size());
    }
    CheckDeployment(ReadPrivateFile(configured_key).size() == 32, "MASTER_KEY_MUST_BE_32_BYTES");
    PrivateDirectory(Text(options, "runtime_directory"));
    *owner = lock.value;
    lock.value = -1;
    return content;
}
}  // namespace
int Plugin::Option(const char* options) {
    try {
        if (default_lock_ >= 0) return EALREADY;
        std::string configuration = options ? options : "";
        if (configuration.empty() || configuration == "{}") configuration = DefaultConfiguration(&default_lock_);
        constexpr const char* file_prefix = "config_file=";
        if (configuration.rfind(file_prefix, 0) == 0) {
            std::ifstream file(configuration.substr(std::strlen(file_prefix)), std::ios::binary);
            if (!file) return EINVAL;
            configuration.assign(std::istreambuf_iterator<char>(file), {});
        }
        options_ = Parse(configuration);
        deployment_ = Deployment{};
        for (auto i = options_.MemberBegin(); i != options_.MemberEnd(); ++i) {
            auto key = std::string(i->name.GetString());
            if (key != "db_path" && key != "master_key_file" && key != "runtime_directory" && key != "files" &&
                key != "environments")
                return EINVAL;
        }
        if (Text(options_, "db_path").empty() || Text(options_, "master_key_file").empty() ||
            Text(options_, "runtime_directory").empty())
            return EINVAL;
        deployment_.runtime_directory = Text(options_, "runtime_directory");
        for (auto kind : {"files", "environments"})
            if (options_.HasMember(kind)) {
                if (!options_[kind].IsObject()) return EINVAL;
                for (auto i = options_[kind].MemberBegin(); i != options_[kind].MemberEnd(); ++i) {
                    if (!i->value.IsString()) return EINVAL;
                    (std::string(kind) == "files" ? deployment_.files : deployment_.environments)[i->name.GetString()] =
                        i->value.GetString();
                }
            }
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "Kafka deployment configuration failed: %s\n", failure.what());
        if (default_lock_ >= 0) close(default_lock_);
        default_lock_ = -1;
        return EINVAL;
    }
}
int Plugin::Load(IQuerier* querier) {
    querier_ = querier;
    try {
        std::ifstream file(Text(options_, "master_key_file"), std::ios::binary);
        std::string value((std::istreambuf_iterator<char>(file)), {});
        if (value.size() != 32) return EINVAL;
        std::array<unsigned char, 32> key;
        std::copy(value.begin(), value.end(), key.begin());
        OPENSSL_cleanse(value.data(), value.size());
        store_.Open(Text(options_, "db_path"), key);
        OPENSSL_cleanse(key.data(), key.size());
        capabilities_ = Client::Capabilities();
        if (Text(capabilities_, "client_version") != "2.16.0") return ENOTSUP;
        const auto features = Text(capabilities_, "features");
        for (const char* required : {"ssl", "sasl_plain", "sasl_scram", "sasl_gssapi", "sasl_oauthbearer", "http",
                                     "oidc", "gzip", "snappy", "lz4", "zstd"})
            if (features.find(required) == std::string::npos) return ENOTSUP;
        tests_stopping_ = false;
        stopping_ = false;
        return 0;
    } catch (...) {
        return EIO;
    }
}
int Plugin::Stop() {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        stopping_ = true;
        for (auto* session : sessions_) session->Cancel();
    }
    StopTests();
    std::unique_lock<std::mutex> lock(sessions_mutex_);
    sessions_drained_.wait(lock, [this] { return sessions_.empty() && pending_ == 0; });
    return 0;
}
int Plugin::Unload() {
    if (Stop() != 0) return EBUSY;
    store_.Close();
    if (default_lock_ >= 0) close(default_lock_);
    default_lock_ = -1;
    return 0;
}
int Plugin::OpenSession(const char* name, const char* usage, IKafkaClientSessionV1** session, std::string* error) {
    if (!session) return EINVAL;
    *session = nullptr;
    if (!name || !usage) return EINVAL;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (stopping_) return ESHUTDOWN;
        ++pending_;
    }
    int rc = 0;
    KafkaBindingSnapshotV1 snapshot;
    bool acquired = false;
    try {
        auto binding = store_.Acquire(name, usage);
        snapshot = binding.snapshot;
        acquired = true;
        auto client = std::make_unique<Client>(std::move(binding), deployment_);
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (stopping_) {
                rc = ESHUTDOWN;
            } else {
                sessions_.insert(client.get());
                *session = client.release();
            }
        }
    } catch (const Failure& failure) {
        if (error) *error = failure.code;
        rc = failure.status == 404 ? ENOENT : (failure.status == 503 ? ENOTSUP : EINVAL);
    } catch (...) {
        if (error) *error = "CLIENT_CREATION_FAILED";
        rc = EIO;
    }
    if (acquired && !*session) store_.Release(snapshot);
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        --pending_;
        sessions_drained_.notify_all();
    }
    return rc;
}
void Plugin::ReleaseSession(IKafkaClientSessionV1* session) {
    Client* client;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        client = dynamic_cast<Client*>(session);
        auto i = sessions_.find(client);
        if (i == sessions_.end()) return;
        sessions_.erase(i);
        ++pending_;
    }
    auto snapshot = client->Snapshot();
    delete client;
    store_.Release(snapshot);
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        --pending_;
        sessions_drained_.notify_all();
    }
}

}  // namespace flowsql::channels::kafka
