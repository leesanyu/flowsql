// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_plugin.h"
#include <openssl/crypto.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>

namespace flowsql::channels::kafka {
int Plugin::Option(const char* options) {
    try {
        std::string configuration = options ? options : "";
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
    } catch (...) {
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
