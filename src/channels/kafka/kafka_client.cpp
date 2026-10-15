// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_client.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <sasl/sasl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

extern char** environ;
namespace flowsql::channels::kafka {
namespace {
using Clock = std::chrono::steady_clock;
int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string Bytes(const std::string& path, size_t limit) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw Failure(400, "CREDENTIAL_FILE_UNAVAILABLE", "credential");
    struct stat info {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || uint64_t(info.st_size) > limit) {
        close(fd);
        throw Failure(400, "CREDENTIAL_FILE_INVALID", "credential");
    }
    std::string bytes;
    char buffer[4096];
    ssize_t size;
    while ((size = read(fd, buffer, sizeof(buffer))) > 0) {
        bytes.append(buffer, size);
        if (bytes.size() > limit) {
            close(fd);
            throw Failure(400, "CREDENTIAL_FILE_INVALID", "credential");
        }
    }
    close(fd);
    if (size < 0) throw Failure(400, "CREDENTIAL_FILE_UNAVAILABLE", "credential");
    return bytes;
}
void Set(rd_kafka_conf_t* config, const char* name, const std::string& value) {
    char error[512];
    if (rd_kafka_conf_set(config, name, value.c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK)
        throw Failure(503, "CLIENT_CAPABILITY_UNAVAILABLE", "capability");
}
size_t Receive(char* data, size_t size, size_t count, void* pointer) {
    auto* out = static_cast<std::string*>(pointer);
    if (size * count > 65536 - out->size()) return 0;
    out->append(data, size * count);
    return size * count;
}
int Progress(void* pointer, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(pointer)->load() ? 1 : 0;
}
}  // namespace
std::string Client::File(const std::string& reference, size_t limit) {
    if (reference.rfind("file:", 0) != 0) throw Failure(400, "INVALID_CREDENTIAL_REFERENCE", "credential");
    auto i = deployment_.files.find(reference.substr(5));
    if (i == deployment_.files.end()) throw Failure(400, "CREDENTIAL_ALIAS_UNAVAILABLE", "credential");
    return Bytes(i->second, limit);
}
std::string Client::Secret(const std::string& reference) {
    if (reference.rfind("file:", 0) == 0) {
        auto value = File(reference, 65536);
        if (!value.empty() && value.back() == '\n') value.pop_back();
        return value;
    }
    if (reference.rfind("env:", 0) != 0) throw Failure(400, "INVALID_CREDENTIAL_REFERENCE", "credential");
    auto i = deployment_.environments.find(reference.substr(4));
    if (i == deployment_.environments.end()) throw Failure(400, "CREDENTIAL_ALIAS_UNAVAILABLE", "credential");
    auto value = getenv(i->second.c_str());
    if (!value || strlen(value) > 65536) throw Failure(400, "CREDENTIAL_ALIAS_UNAVAILABLE", "credential");
    return value;
}
Client::Client(Binding binding, const Deployment& deployment) : binding_(std::move(binding)), deployment_(deployment) {
    static const int initialized = []() { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (initialized) throw Failure(503, "HTTP_CLIENT_UNAVAILABLE", "capability");
    std::vector<std::pair<std::string, std::string>> resolved;
    for (auto& [key, value] : binding_.secrets)
        if (key.rfind("ref:", 0) == 0) resolved.emplace_back(key.substr(4), Secret(value));
    for (auto& pair : resolved) binding_.secrets[pair.first] = std::move(pair.second);
    auto config =
        std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)>(rd_kafka_conf_new(), rd_kafka_conf_destroy);
    try {
        Configure(config.get());
        // rd_kafka_new consumes/desensitizes PEM keys. Freeze the consumer configuration first.
        if (binding_.snapshot.usage == "source") consumer_config_.reset(rd_kafka_conf_dup(config.get()));
        char error[512];
        client_ = rd_kafka_new(RD_KAFKA_PRODUCER, config.get(), error, sizeof(error));
        if (!client_) throw Failure(503, "CLIENT_CREATION_FAILED", "capability");
        config.release();
        maintenance_ = std::thread([this] {
            while (!cancelled_) {
                bool ready = Ticket(Clock::now() + std::chrono::seconds(1));
                ticket_ready_.store(ready);
                rd_kafka_poll(client_, 0);
                std::unique_lock<std::mutex> lock(wait_mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(25));
            }
        });
    } catch (...) {
        if (client_) rd_kafka_destroy(client_);
        if (!temporary_.empty()) std::filesystem::remove_all(temporary_);
        throw;
    }
}
void Client::Configure(rd_kafka_conf_t* config) {
    auto& c = binding_.connection;
    std::string brokers;
    for (auto& server : c["bootstrap_servers"].GetArray()) {
        if (!brokers.empty()) brokers += ',';
        brokers += server.GetString();
    }
    Set(config, "bootstrap.servers", brokers);
    Set(config, "security.protocol", Text(c, "security_protocol"));
    Set(config, "allow.auto.create.topics", "false");
    Set(config, "enable.metrics.push", "false");
    Set(config, "log_level", "0");
    Set(config, "socket.connection.setup.timeout.ms", "1000");
    Set(config, "socket.timeout.ms", "1000");
    Set(config, "reconnect.backoff.max.ms", "1000");
    rd_kafka_conf_set_opaque(config, this);
    rd_kafka_conf_set_log_cb(config, Log);
    rd_kafka_conf_set_error_cb(config, Error);
    if (c.HasMember("tls")) {
        auto& tls = c["tls"];
        if (!Text(tls, "ca_ref").empty()) Set(config, "ssl.ca.pem", File(Text(tls, "ca_ref"), 1048576));
        if (!Text(tls, "certificate_ref").empty()) {
            Set(config, "ssl.certificate.pem", File(Text(tls, "certificate_ref"), 1048576));
            Set(config, "ssl.key.pem", File(Text(tls, "private_key_ref"), 1048576));
            if (binding_.secrets.count("private_key_password"))
                Set(config, "ssl.key.password", binding_.secrets.at("private_key_password"));
        }
    }
    Set(config, "enable.ssl.certificate.verification", "true");
    Set(config, "ssl.endpoint.identification.algorithm", "https");
    auto mechanism = Text(c, "sasl_mechanism");
    if (mechanism.empty()) return;
    auto& sasl = c["sasl"];
    Set(config, "sasl.mechanism", mechanism);
    if (mechanism == "PLAIN" || mechanism.rfind("SCRAM-", 0) == 0) {
        Set(config, "sasl.username", Text(sasl, "username"));
        Set(config, "sasl.password", binding_.secrets.at("password"));
    }
    if (mechanism == "GSSAPI") {
        std::filesystem::create_directories(deployment_.runtime_directory);
        auto pattern = (deployment_.runtime_directory / "session-XXXXXX").string();
        std::vector<char> mutable_path(pattern.begin(), pattern.end());
        mutable_path.push_back(0);
        auto directory = mkdtemp(mutable_path.data());
        if (!directory) throw Failure(500, "PRIVATE_RUNTIME_UNAVAILABLE", "credential");
        temporary_ = directory;
        auto keytab = File(Text(sasl, "keytab_ref"), 1048576);
        auto path = temporary_ / "keytab";
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, keytab.data(), keytab.size()) != ssize_t(keytab.size())) {
            if (fd >= 0) close(fd);
            throw Failure(500, "PRIVATE_RUNTIME_UNAVAILABLE", "credential");
        }
        close(fd);
        ccache_ = "FILE:" + (temporary_ / "ticket").string();
        Set(config, "sasl.kerberos.principal", Text(sasl, "principal"));
        Set(config, "sasl.kerberos.service.name", Text(sasl, "service_name"));
        Set(config, "sasl.kerberos.min.time.before.relogin", "0");
        Set(config, "sasl.kerberos.kinit.cmd", "");
        Set(config, "sasl.kerberos.flowsql.ccache", ccache_);
    }
    if (mechanism == "OAUTHBEARER") {
        Set(config, "sasl.oauthbearer.method", "default");
        Set(config, "enable.sasl.oauthbearer.unsecure.jwt", "false");
        rd_kafka_conf_set_oauthbearer_token_refresh_cb(config, OAuth);
        if (!Text(sasl, "ca_ref").empty()) binding_.secrets["oauth_ca"] = File(Text(sasl, "ca_ref"), 1048576);
    }
}
Client::~Client() {
    Cancel();
    if (maintenance_.joinable()) maintenance_.join();
    if (consumer_) {
        auto* queue = rd_kafka_queue_new(consumer_);
        auto* error = rd_kafka_consumer_close_queue(consumer_, queue);
        if (error) rd_kafka_error_destroy(error);
        auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!rd_kafka_consumer_closed(consumer_) && Clock::now() < deadline) {
            auto* event = rd_kafka_queue_poll(queue, 25);
            if (event) {
                // The event API transfers rebalance handling to the application during close.
                if (rd_kafka_event_type(event) == RD_KAFKA_EVENT_REBALANCE) rd_kafka_assign(consumer_, nullptr);
                rd_kafka_event_destroy(event);
            }
        }
        rd_kafka_queue_destroy(queue);
        rd_kafka_destroy_flags(consumer_, RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE);
    }
    if (client_) rd_kafka_destroy(client_);
    for (auto& [key, value] : binding_.secrets) OPENSSL_cleanse(value.data(), value.size());
    if (!temporary_.empty()) std::filesystem::remove_all(temporary_);
}
void Client::Cancel() {
    cancelled_.store(true);
    wake_.notify_all();
}
int Client::QueryInterface(const Guid& iid, void** capability) {
    if (!capability) return EINVAL;
    *capability = nullptr;
    if (std::memcmp(&iid, &IID_KAFKA_CONSUMER_SESSION_V1, sizeof(Guid)) == 0 && binding_.snapshot.usage == "source") {
        *capability = static_cast<IKafkaConsumerSessionV1*>(this);
        return 0;
    }
    return ENOTSUP;
}
int Client::StartConsumer(const char* reset, uint32_t max_message_bytes) {
    std::lock_guard<std::mutex> operation(operation_mutex_);
    if (cancelled_) return ECANCELED;
    if (consumer_) return EALREADY;
    if (binding_.snapshot.usage != "source" || binding_.snapshot.consumer_group.empty()) return EINVAL;
    if (!reset ||
        (std::string(reset) != "earliest" && std::string(reset) != "latest" && std::string(reset) != "error") ||
        !max_message_bytes || max_message_bytes > 16777216)
        return EINVAL;
    if (!ccache_.empty()) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        std::unique_lock<std::mutex> lock(wait_mutex_);
        while (!ticket_ready_.load() && !cancelled_ && Clock::now() < deadline)
            wake_.wait_for(lock, std::chrono::milliseconds(10));
        if (cancelled_) return ECANCELED;
        if (!ticket_ready_.load()) return EACCES;
    }
    auto config = std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)>(
        rd_kafka_conf_dup(consumer_config_.get()), rd_kafka_conf_destroy);
    try {
        rd_kafka_conf_set_error_cb(config.get(), ConsumerError);
        Set(config.get(), "group.id", binding_.snapshot.consumer_group);
        Set(config.get(), "enable.auto.commit", "false");
        Set(config.get(), "enable.auto.offset.store", "false");
        Set(config.get(), "auto.offset.reset", reset);
        Set(config.get(), "queued.min.messages", "1");
        Set(config.get(), "queued.max.messages.kbytes", "32768");
        Set(config.get(), "fetch.wait.max.ms", "50");
        Set(config.get(), "fetch.message.max.bytes", std::to_string(max_message_bytes));
        Set(config.get(), "fetch.max.bytes", "16777216");
        char error[512];
        consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, config.get(), error, sizeof(error));
        if (!consumer_) return EIO;
        config.release();
        rd_kafka_poll_set_consumer(consumer_);
        auto* topics = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(topics, binding_.snapshot.topic.c_str(), RD_KAFKA_PARTITION_UA);
        const auto rc = rd_kafka_subscribe(consumer_, topics);
        rd_kafka_topic_partition_list_destroy(topics);
        if (rc) return EIO;
        consumer_config_.reset();
        message_limit_ = max_message_bytes;
        return 0;
    } catch (...) {
        return EINVAL;
    }
}
int Client::PollMessage(uint32_t timeout_ms, KafkaMessageV1* message) {
    if (!message || timeout_ms > 30000) return EINVAL;
    std::lock_guard<std::mutex> operation(operation_mutex_);
    *message = {};
    if (!consumer_) return EBADF;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        if (cancelled_) return ECANCELED;
        if (oauth_failed_ || (!ccache_.empty() && !ticket_ready_.load())) return EACCES;
        const auto callback = consumer_error_.load();
        if (callback == RD_KAFKA_RESP_ERR__AUTHENTICATION || callback == RD_KAFKA_RESP_ERR__SSL ||
            callback == RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED)
            return EACCES;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        auto* record = rd_kafka_consumer_poll(consumer_, std::max<int64_t>(0, std::min<int64_t>(25, remaining)));
        if (record) {
            std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)> owner(record,
                                                                                           rd_kafka_message_destroy);
            if (record->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) continue;
            if (record->err) return EIO;
            if (record->len > message_limit_ || record->key_len > message_limit_) return EMSGSIZE;
            message->topic = rd_kafka_topic_name(record->rkt);
            message->partition = record->partition;
            message->offset = record->offset;
            message->timestamp_ms = rd_kafka_message_timestamp(record, nullptr);
            message->value_is_null = record->payload == nullptr;
            message->key_is_null = record->key == nullptr;
            if (record->payload) message->value.assign(static_cast<const char*>(record->payload), record->len);
            if (record->key) message->key.assign(static_cast<const char*>(record->key), record->key_len);
            return 0;
        }
        if (consumer_error_.load() == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN) return EHOSTUNREACH;
    } while (Clock::now() < deadline);
    return cancelled_ ? ECANCELED : EAGAIN;
}
int Client::CommitOffsets(const std::vector<KafkaOffsetV1>& offsets, uint32_t timeout_ms) {
    if (offsets.empty() || !timeout_ms || timeout_ms > 30000) return EINVAL;
    std::lock_guard<std::mutex> operation(operation_mutex_);
    if (cancelled_) return ECANCELED;
    if (!consumer_) return EBADF;
    auto* positions = rd_kafka_topic_partition_list_new(offsets.size());
    for (const auto& offset : offsets) {
        if (offset.partition < 0 || offset.next_offset < 0) {
            rd_kafka_topic_partition_list_destroy(positions);
            return EINVAL;
        }
        rd_kafka_topic_partition_list_add(positions, binding_.snapshot.topic.c_str(), offset.partition)->offset =
            offset.next_offset;
    }
    auto* queue = rd_kafka_queue_new(consumer_);
    auto rc = rd_kafka_commit_queue(consumer_, positions, queue, nullptr, nullptr);
    rd_kafka_topic_partition_list_destroy(positions);
    int result = rc ? EIO : ETIMEDOUT;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!rc && !cancelled_ && Clock::now() < deadline) {
        auto* event = rd_kafka_queue_poll(queue, 25);
        if (!event) continue;
        if (rd_kafka_event_type(event) == RD_KAFKA_EVENT_OFFSET_COMMIT) {
            result = rd_kafka_event_error(event) ? EIO : 0;
            const auto* partitions = rd_kafka_event_topic_partition_list(event);
            if (partitions)
                for (int i = 0; i < partitions->cnt; ++i)
                    if (partitions->elems[i].err) result = EIO;
            rd_kafka_event_destroy(event);
            break;
        }
        rd_kafka_event_destroy(event);
    }
    rd_kafka_queue_destroy(queue);
    return cancelled_ ? ECANCELED : result;
}
void Client::Error(rd_kafka_t*, int error, const char*, void* opaque) {
    static_cast<Client*>(opaque)->callback_error_.store(error);
}
void Client::ConsumerError(rd_kafka_t*, int error, const char*, void* opaque) {
    static_cast<Client*>(opaque)->consumer_error_.store(error);
}
bool Client::Ticket(Clock::time_point deadline) {
    if (ccache_.empty() || Now() < ticket_expires_) return true;
    auto& sasl = binding_.connection["sasl"];
    std::vector<std::string> arguments = {
        "/usr/bin/kinit",       "-c", ccache_, "-k", "-t", (temporary_ / "keytab").string(), "-l", "60s",
        Text(sasl, "principal")};
    std::vector<char*> argv;
    for (auto& arg : arguments) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** item = environ; item && *item; ++item)
        if (strncmp(*item, "KRB5CCNAME=", 11)) environment.emplace_back(*item);
    environment.push_back("KRB5CCNAME=" + ccache_);
    std::vector<char*> env;
    for (auto& entry : environment) env.push_back(entry.data());
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t process;
    int rc = posix_spawn(&process, argv[0], &actions, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc) return false;
    int status;
    while (waitpid(process, &status, WNOHANG) == 0) {
        if (cancelled_ || Clock::now() >= deadline) {
            kill(process, SIGKILL);
            waitpid(process, &status, 0);
            return false;
        }
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    callback_error_.store(0);
    ticket_expires_ = Now() + 30000;
    return true;
}
void Client::OAuth(rd_kafka_t* kafka, const char*, void* opaque) {
    auto* self = static_cast<Client*>(opaque);
    auto& sasl = self->binding_.connection["sasl"];
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
        self->oauth_failed_.store(true);
        rd_kafka_oauthbearer_set_token_failure(kafka, "OAuth client unavailable");
        return;
    }
    auto escape = [&](const std::string& value) {
        char* data = curl_easy_escape(curl.get(), value.data(), value.size());
        std::string result = data ? data : "";
        curl_free(data);
        return result;
    };
    auto body = "grant_type=client_credentials&client_id=" + escape(Text(sasl, "client_id")) +
                "&client_secret=" + escape(self->binding_.secrets.at("client_secret"));
    if (!Text(sasl, "scope").empty()) body += "&scope=" + escape(Text(sasl, "scope"));
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, Text(sasl, "token_endpoint").c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    long timeout = 1000;
    auto deadline = self->probe_deadline_ms_.load();
    auto steady_now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (deadline > 0) timeout = std::max<int64_t>(1, std::min<int64_t>(1000, deadline - steady_now));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, Receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &self->cancelled_);
    if (self->binding_.secrets.count("oauth_ca")) {
        auto& ca = self->binding_.secrets.at("oauth_ca");
        curl_blob blob{ca.data(), ca.size(), CURL_BLOB_COPY};
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO_BLOB, &blob);
    }
    auto rc = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    OPENSSL_cleanse(body.data(), body.size());
    bool accepted = false;
    try {
        if (rc == CURLE_OK && status == 200) {
            auto token = Parse(response);
            auto access = Text(token, "access_token");
            auto kind = Text(token, "token_type");
            if (!access.empty() && (kind == "Bearer" || kind == "bearer") && token.HasMember("expires_in") &&
                token["expires_in"].IsInt64() && token["expires_in"].GetInt64() > 0 &&
                token["expires_in"].GetInt64() <= 86400) {
                std::string principal = Text(sasl, "client_id");
                char error[512];
                accepted = rd_kafka_oauthbearer_set_token(
                               kafka, access.data(), Now() + token["expires_in"].GetInt64() * 1000, principal.c_str(),
                               nullptr, 0, error, sizeof(error)) == RD_KAFKA_RESP_ERR_NO_ERROR;
            }
        }
    } catch (...) {
    }
    OPENSSL_cleanse(response.data(), response.size());
    self->oauth_failed_.store(!accepted);
    if (!accepted) rd_kafka_oauthbearer_set_token_failure(kafka, "OAuth token acquisition failed");
}
int Client::Probe(uint32_t timeout_ms, std::string* output) {
    if (!output) return EINVAL;
    std::lock_guard<std::mutex> operation(operation_mutex_);
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    probe_deadline_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count());
    callback_error_.store(0);
    bool success = false;
    std::vector<std::string> checked;
    std::string stage = "network", code = "CONNECTION_FAILED";
    if (timeout_ms < 1 || timeout_ms > 30000) {
        probe_deadline_ms_.store(0);
        *output = R"({"status":"failed","code":"INVALID_TIMEOUT","stage":"configuration"})";
        return EINVAL;
    }
    while (!cancelled_ && Clock::now() < deadline) {
        if (oauth_failed_) {
            stage = "oauth";
            code = "OAUTH_TOKEN_ACQUISITION_FAILED";
            break;
        }
        if (!ccache_.empty() && !ticket_ready_.load()) {
            stage = "credential";
            code = "KERBEROS_TICKET_FAILED";
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        const rd_kafka_metadata_t* metadata = nullptr;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) break;
        auto result = rd_kafka_metadata(client_, 1, nullptr, &metadata, std::min<int64_t>(100, remaining));
        if (result == RD_KAFKA_RESP_ERR_NO_ERROR && metadata) {
            success = metadata->broker_cnt > 0;
            bool found_topic = binding_.snapshot.topic.empty();
            for (int i = 0; i < metadata->topic_cnt; ++i) {
                if (binding_.snapshot.topic != metadata->topics[i].topic) continue;
                found_topic = true;
                if (metadata->topics[i].err) {
                    success = false;
                    stage = "metadata";
                    code = "TOPIC_METADATA_FAILED";
                }
            }
            if (!found_topic) {
                success = false;
                stage = "metadata";
                code = "TOPIC_NOT_FOUND";
            }
            if (success) {
                if (metadata->orig_broker_name) checked.emplace_back(metadata->orig_broker_name);
                for (int i = 0; i < metadata->broker_cnt && success; ++i) {
                    const auto& broker = metadata->brokers[i];
                    if (broker.id == metadata->orig_broker_id) continue;
                    auto config = rd_kafka_conf_dup(rd_kafka_conf(client_));
                    std::string host = broker.host;
                    std::string address = (host.find(':') == std::string::npos ? host : "[" + host + "]") + ":" +
                                          std::to_string(broker.port);
                    Set(config, "bootstrap.servers", address);
                    char creation_error[512];
                    auto peer = rd_kafka_new(RD_KAFKA_PRODUCER, config, creation_error, sizeof(creation_error));
                    if (!peer) {
                        rd_kafka_conf_destroy(config);
                        success = false;
                        break;
                    }
                    bool peer_ok = false;
                    while (!cancelled_ && Clock::now() < deadline) {
                        rd_kafka_poll(peer, 0);
                        const rd_kafka_metadata_t* peer_metadata = nullptr;
                        auto left =
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                        if (left <= 0) break;
                        auto rc = rd_kafka_metadata(peer, 1, nullptr, &peer_metadata, std::min<int64_t>(100, left));
                        if (rc == RD_KAFKA_RESP_ERR_NO_ERROR && peer_metadata)
                            peer_ok = peer_metadata->orig_broker_id == broker.id;
                        if (peer_metadata) rd_kafka_metadata_destroy(peer_metadata);
                        if (peer_ok) break;
                    }
                    rd_kafka_destroy(peer);
                    if (peer_ok)
                        checked.push_back(address);
                    else {
                        success = false;
                        stage = "network";
                        code = "ADVERTISED_BROKER_UNVERIFIED";
                    }
                }
            }
            rd_kafka_metadata_destroy(metadata);
            metadata = nullptr;
            if (success) break;
            if (stage == "metadata") break;
        }
        if (metadata) rd_kafka_metadata_destroy(metadata);
        auto error = callback_error_.load();
        if (error == RD_KAFKA_RESP_ERR__AUTHENTICATION || error == RD_KAFKA_RESP_ERR__SSL) {
            stage = error == RD_KAFKA_RESP_ERR__SSL ? "tls" : "sasl";
            code = "AUTHENTICATION_FAILED";
            break;
        }
    }
    if (cancelled_) {
        success = false;
        stage = "cancel";
        code = "CANCELLED";
    } else if (!success && Clock::now() >= deadline)
        code = "TIMEOUT";
    Json result;
    result.SetObject();
    auto& a = result.GetAllocator();
    String(result, "status", success ? "passed" : "failed", a);
    String(result, "stage", success ? "metadata" : stage, a);
    String(result, "code", success ? "OK" : code, a);
    String(result, "read_permission", "unverified", a);
    String(result, "write_permission", "unverified", a);
    rapidjson::Value brokers(rapidjson::kArrayType);
    for (const auto& address : checked) brokers.PushBack(rapidjson::Value(address.c_str(), a), a);
    result.AddMember("checked_brokers", brokers, a);
    result.AddMember("timeout_ms", timeout_ms, a);
    result.AddMember("verified_at", Now(), a);
    probe_deadline_ms_.store(0);
    *output = Encode(result);
    return success ? 0 : (cancelled_ ? ECANCELED : EIO);
}
Json Client::Capabilities() {
    Json result;
    result.SetObject();
    auto& a = result.GetAllocator();
    String(result, "client_version", rd_kafka_version_str(), a);
    char features[2048];
    size_t size = sizeof(features);
    auto config = rd_kafka_conf_new();
    rd_kafka_conf_get(config, "builtin.features", features, &size);
    rd_kafka_conf_destroy(config);
    String(result, "features", features, a);
    static int sasl_initialized = sasl_client_init(nullptr);
    bool gssapi = false;
    sasl_conn_t* sasl = nullptr;
    if (sasl_initialized == SASL_OK &&
        sasl_client_new("kafka", "localhost", nullptr, nullptr, nullptr, 0, &sasl) == SASL_OK) {
        const char* mechanisms = nullptr;
        if (sasl_listmech(sasl, nullptr, "", ",", "", &mechanisms, nullptr, nullptr) == SASL_OK && mechanisms)
            gssapi = std::string(mechanisms).find("GSSAPI") != std::string::npos;
        sasl_dispose(&sasl);
    }
    gssapi = gssapi && access("/usr/bin/kinit", X_OK) == 0;
    rapidjson::Value mechanisms(rapidjson::kArrayType);
    for (auto name : {"PLAIN", "SCRAM-SHA-256", "SCRAM-SHA-512", "GSSAPI", "OAUTHBEARER"}) {
        rapidjson::Value item(rapidjson::kObjectType);
        String(item, "name", name, a);
        bool available = std::string(name) != "GSSAPI" || gssapi;
        if (std::string(name) == "OAUTHBEARER")
            available = (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_SSL) != 0;
        item.AddMember("available", available, a);
        String(item, "reason", available ? "" : "RUNTIME_AUTH_COMPONENT_UNAVAILABLE", a);
        mechanisms.PushBack(item, a);
    }
    result.AddMember("sasl_mechanisms", mechanisms, a);
    return result;
}
}  // namespace flowsql::channels::kafka
