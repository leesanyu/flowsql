// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "kafka_store.h"
#include <framework/interfaces/ipacket_family.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <algorithm>
#include <common/json_depth.hpp>
#include <cstring>
#include <memory>
#include <regex>
#include <set>
#include "kafka_packet.h"
#include "kafka_result_codec.h"
#include "kafka_structured.h"

namespace flowsql::channels::kafka {
namespace {
void Require(bool value, const char* code = "INVALID_CONFIGURATION", int status = 400) {
    if (!value) throw Failure(status, code);
}
void Keys(const rapidjson::Value& value, std::initializer_list<const char*> allowed) {
    Require(value.IsObject());
    for (auto i = value.MemberBegin(); i != value.MemberEnd(); ++i) {
        Require(std::any_of(allowed.begin(), allowed.end(), [&](const char* key) { return i->name == key; }),
                "UNKNOWN_FIELD");
    }
}
void Unique(const rapidjson::Value& value) {
    if (value.IsObject()) {
        std::set<std::string> keys;
        for (auto i = value.MemberBegin(); i != value.MemberEnd(); ++i) {
            Require(keys.emplace(i->name.GetString(), i->name.GetStringLength()).second, "DUPLICATE_FIELD");
            Unique(i->value);
        }
    } else if (value.IsArray()) {
        for (auto& item : value.GetArray()) Unique(item);
    } else if (value.IsString())
        Require(std::strlen(value.GetString()) == value.GetStringLength(), "INVALID_STRING");
}
bool Named(const std::string& name) { return std::regex_match(name, std::regex("[a-z][a-z0-9_-]{0,63}")); }
bool Ref(const std::string& text, bool file_only = false) {
    return std::regex_match(text,
                            std::regex(file_only ? "file:[a-z][a-z0-9_-]{0,63}" : "(file|env):[a-z][a-z0-9_-]{0,63}"));
}
bool HostPort(const std::string& server) {
    std::smatch match;
    if (!std::regex_match(server, match, std::regex("(\\[[0-9a-fA-F:]+\\]|[A-Za-z0-9_.-]+):([0-9]{1,5})")))
        return false;
    const int port = std::stoi(match[2]);
    return port > 0 && port <= 65535;
}
std::set<std::string> RequiredSecrets(const rapidjson::Value& connection) {
    const auto mech = Text(connection, "sasl_mechanism");
    if (mech == "PLAIN" || mech == "SCRAM-SHA-256" || mech == "SCRAM-SHA-512") return {"password"};
    if (mech == "OAUTHBEARER") return {"client_secret"};
    return {};
}
std::set<std::string> AllowedSecrets(const rapidjson::Value& connection) {
    auto fields = RequiredSecrets(connection);
    if (connection.HasMember("tls") && !Text(connection["tls"], "private_key_ref").empty())
        fields.insert("private_key_password");
    return fields;
}
class Statement {
 public:
    Statement(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value_, nullptr) != SQLITE_OK)
            throw Failure(500, "PERSISTENCE_FAILURE", "persistence");
    }
    ~Statement() { sqlite3_finalize(value_); }
    void Bind(int index, const std::string& value) {
        if (sqlite3_bind_blob(value_, index, value.data(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK)
            throw Failure(500, "PERSISTENCE_FAILURE", "persistence");
    }
    void TextBind(int index, const std::string& value) {
        if (sqlite3_bind_text(value_, index, value.data(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK)
            throw Failure(500, "PERSISTENCE_FAILURE", "persistence");
    }
    bool Row() {
        const int result = sqlite3_step(value_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) throw Failure(500, "PERSISTENCE_FAILURE", "persistence");
        return result == SQLITE_ROW;
    }
    std::string At(int index) {
        const char* data = reinterpret_cast<const char*>(sqlite3_column_blob(value_, index));
        return data ? std::string(data, sqlite3_column_bytes(value_, index)) : "";
    }

 private:
    sqlite3_stmt* value_ = nullptr;
};
}  // namespace
std::string Encode(const rapidjson::Value& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}
Json Parse(const std::string& text) {
    Require(text.size() <= 65536, "REQUEST_TOO_LARGE", 413);
    Require(JsonNestingWithin(text, 32), "INVALID_JSON");
    Json value;
    value.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
    Require(!value.HasParseError() && value.IsObject(), "INVALID_JSON");
    Unique(value);
    return value;
}
std::string Text(const rapidjson::Value& object, const char* key, const std::string& fallback) {
    Require(object.IsObject());
    auto item = object.FindMember(key);
    if (item == object.MemberEnd()) return fallback;
    Require(item->value.IsString(), "INVALID_FIELD_TYPE");
    return {item->value.GetString(), item->value.GetStringLength()};
}
void String(rapidjson::Value& object, const char* key, const std::string& value, Json::AllocatorType& allocator) {
    object.RemoveMember(key);
    object.AddMember(rapidjson::Value(key, allocator), rapidjson::Value(value.data(), value.size(), allocator),
                     allocator);
}
Store::~Store() { Close(); }
void Store::Exec(const char* sql) {
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Failure(500, "PERSISTENCE_FAILURE", "persistence");
}
std::string Store::Encrypt(const std::string& plain) {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::string result(12 + plain.size() + 16, '\0');
    int size = 0, final = 0;
    Require(context && RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), 12) == 1, "CRYPTO_FAILURE", 500);
    Require(EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key_.data(),
                               reinterpret_cast<unsigned char*>(result.data())) == 1,
            "CRYPTO_FAILURE", 500);
    Require(EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(result.data() + 12), &size,
                              reinterpret_cast<const unsigned char*>(plain.data()), plain.size()) == 1,
            "CRYPTO_FAILURE", 500);
    Require(
        EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(result.data() + 12 + size), &final) == 1,
        "CRYPTO_FAILURE", 500);
    Require(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, result.data() + 12 + size) == 1,
            "CRYPTO_FAILURE", 500);
    return result;
}
std::string Store::Decrypt(const std::string& cipher) {
    Require(cipher.size() >= 28, "SECRET_DECRYPTION_FAILED", 500);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::string plain(cipher.size() - 28, '\0');
    int size = 0, final = 0;
    Require(context && EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key_.data(),
                                          reinterpret_cast<const unsigned char*>(cipher.data())) == 1,
            "SECRET_DECRYPTION_FAILED", 500);
    Require(EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plain.data()), &size,
                              reinterpret_cast<const unsigned char*>(cipher.data() + 12), plain.size()) == 1,
            "SECRET_DECRYPTION_FAILED", 500);
    Require(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<char*>(cipher.data() + cipher.size() - 16)) == 1 &&
                EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(plain.data() + size), &final) == 1,
            "SECRET_DECRYPTION_FAILED", 500);
    return plain;
}
void Store::Open(const std::string& path, const std::array<unsigned char, 32>& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(!db_, "STORE_ALREADY_OPEN", 409);
    key_ = key;
    try {
        Require(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                nullptr) == SQLITE_OK,
                "PERSISTENCE_FAILURE", 500);
        if (path != ":memory:") Require(chmod(path.c_str(), 0600) == 0, "PERSISTENCE_FAILURE", 500);
        sqlite3_busy_timeout(db_, 2000);
        Exec(
            "PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON; CREATE TABLE IF NOT EXISTS connections(name TEXT PRIMARY "
            "KEY,definition TEXT NOT NULL); CREATE TABLE IF NOT EXISTS channels(name TEXT PRIMARY KEY,connection_name "
            "TEXT NOT NULL REFERENCES connections(name),definition TEXT NOT NULL); CREATE TABLE IF NOT EXISTS "
            "secrets(id TEXT PRIMARY KEY,cipher BLOB NOT NULL); CREATE TABLE IF NOT EXISTS key_check(id INTEGER "
            "PRIMARY KEY,cipher BLOB NOT NULL);");
        Statement check(db_, "SELECT cipher FROM key_check WHERE id=1");
        if (check.Row())
            Require(Decrypt(check.At(0)) == "flowsql-kafka-key-v1", "SECRET_DECRYPTION_FAILED", 500);
        else {
            Statement add(db_, "INSERT INTO key_check VALUES(1,?)");
            add.Bind(1, Encrypt("flowsql-kafka-key-v1"));
            add.Row();
        }
        Statement secrets(db_, "SELECT cipher FROM secrets");
        while (secrets.Row()) Decrypt(secrets.At(0));
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        OPENSSL_cleanse(key_.data(), key_.size());
        throw;
    }
}
void Store::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    OPENSSL_cleanse(key_.data(), key_.size());
    connections_in_use_.clear();
    channels_in_use_.clear();
}
void Store::ValidateConnection(const rapidjson::Value& c) {
    Keys(c, {"name", "revision", "bootstrap_servers", "security_protocol", "sasl_mechanism", "tls", "sasl",
             "credential_bindings"});
    Require(Named(Text(c, "name")), "INVALID_NAME");
    Require(c.HasMember("bootstrap_servers") && c["bootstrap_servers"].IsArray() && c["bootstrap_servers"].Size() > 0 &&
                c["bootstrap_servers"].Size() <= 32,
            "INVALID_BOOTSTRAP_SERVERS");
    std::set<std::string> servers;
    for (auto& server : c["bootstrap_servers"].GetArray())
        Require(server.IsString() && HostPort(server.GetString()) && servers.insert(server.GetString()).second,
                "INVALID_BOOTSTRAP_SERVERS");
    const auto protocol = Text(c, "security_protocol"), mech = Text(c, "sasl_mechanism");
    Require(protocol == "PLAINTEXT" || protocol == "SSL" || protocol == "SASL_PLAINTEXT" || protocol == "SASL_SSL",
            "INVALID_SECURITY_PROTOCOL");
    const bool tls = protocol == "SSL" || protocol == "SASL_SSL",
               sasl = protocol == "SASL_SSL" || protocol == "SASL_PLAINTEXT";
    if (!tls) Require(!c.HasMember("tls"), "UNEXPECTED_TLS_FIELDS");
    if (c.HasMember("tls")) {
        auto& t = c["tls"];
        Keys(t, {"ca_ref", "certificate_ref", "private_key_ref"});
        for (auto i = t.MemberBegin(); i != t.MemberEnd(); ++i)
            Require(i->value.IsString() && Ref(i->value.GetString(), true), "INVALID_CREDENTIAL_REFERENCE");
        Require(Text(t, "certificate_ref").empty() == Text(t, "private_key_ref").empty(),
                "CERTIFICATE_KEY_PAIR_REQUIRED");
    }
    if (!sasl)
        Require(mech.empty() && !c.HasMember("sasl") && !c.HasMember("sasl_mechanism"), "UNEXPECTED_SASL_FIELDS");
    if (sasl) {
        Require(c.HasMember("sasl"), "SASL_FIELDS_REQUIRED");
        auto& s = c["sasl"];
        if (mech == "PLAIN" || mech == "SCRAM-SHA-256" || mech == "SCRAM-SHA-512") {
            Keys(s, {"username"});
            Require(!Text(s, "username").empty(), "USERNAME_REQUIRED");
        } else if (mech == "GSSAPI") {
            Keys(s, {"principal", "service_name", "keytab_ref"});
            Require(
                !Text(s, "principal").empty() && !Text(s, "service_name").empty() && Ref(Text(s, "keytab_ref"), true),
                "KERBEROS_FIELDS_REQUIRED");
        } else if (mech == "OAUTHBEARER") {
            Keys(s, {"token_endpoint", "client_id", "scope", "ca_ref"});
            auto url = Text(s, "token_endpoint");
            Require(url.rfind("https://", 0) == 0 && url.size() > 8 &&
                        url.find_first_of("@?#\r\n 	") == std::string::npos && !Text(s, "client_id").empty(),
                    "OAUTH_FIELDS_REQUIRED");
            if (s.HasMember("scope")) Text(s, "scope");
            if (s.HasMember("ca_ref")) Require(Ref(Text(s, "ca_ref"), true), "INVALID_CREDENTIAL_REFERENCE");
        } else
            throw Failure(400, "INVALID_SASL_MECHANISM");
    }
    auto required = RequiredSecrets(c), allowed = AllowedSecrets(c);
    if (c.HasMember("credential_bindings")) {
        auto& credentials = c["credential_bindings"];
        Require(credentials.IsObject());
        for (auto i = credentials.MemberBegin(); i != credentials.MemberEnd(); ++i)
            Require(allowed.count(i->name.GetString()), "UNEXPECTED_SECRET_FIELD");
    }
    for (auto& field : required)
        Require(c.HasMember("credential_bindings") && c["credential_bindings"].HasMember(field.c_str()),
                "SECRET_REQUIRED");
}
void Store::ValidateChannel(const rapidjson::Value& c) {
    Keys(c,
         {"name", "revision", "connection_name", "role", "topic", "consumer_group", "payload_kind", "payload_options"});
    Require(Named(Text(c, "name")) && Named(Text(c, "connection_name")), "INVALID_NAME");
    auto role = Text(c, "role"), topic = Text(c, "topic"), kind = Text(c, "payload_kind");
    Require(role == "source" || role == "sink" || role == "both", "INVALID_ROLE");
    Require(topic != "." && topic != ".." && std::regex_match(topic, std::regex("[A-Za-z0-9._-]{1,249}")),
            "INVALID_TOPIC");
    Require(kind == "structured" || kind == "packet", "INVALID_PAYLOAD_KIND");
    if (role == "sink")
        Require(!c.HasMember("consumer_group"), "UNEXPECTED_CONSUMER_GROUP");
    else
        Require(!Text(c, "consumer_group").empty() && Text(c, "consumer_group").size() <= 255,
                "CONSUMER_GROUP_REQUIRED");
    if (c.HasMember("payload_options")) {
        auto& options = c["payload_options"];
        Keys(options, {"source", "sink"});
        for (auto i = options.MemberBegin(); i != options.MemberEnd(); ++i) {
            Require(role == "both" || role == i->name.GetString(), "INVALID_PAYLOAD_DIRECTION");
            Require(i->value.IsObject(), "INVALID_PAYLOAD_OPTIONS");
            if (i->value.ObjectEmpty()) continue;
            const bool source = std::string(i->name.GetString()) == "source";
            Require(kind == "structured" || source, "KAFKA_DATA_ADAPTER_UNAVAILABLE");
            try {
                if (kind == "packet")
                    ParsePacketSourceConfig(i->value);
                else if (source)
                    ParseStructuredConfig(i->value);
                else
                    ParseSinkConfig(i->value);
            } catch (const std::invalid_argument&) {
                throw Failure(400, kind == "packet" ? "INVALID_PACKET_SOURCE"
                                   : source         ? "INVALID_STRUCTURED_SOURCE"
                                                    : "INVALID_STRUCTURED_SINK");
            }
        }
    }
}
Json Store::Get(const std::string& kind, const std::string& name) {
    Statement statement(db_, "SELECT definition FROM " + kind + " WHERE name=?");
    statement.TextBind(1, name);
    if (!statement.Row()) throw Failure(404, "CONFIGURATION_NOT_FOUND");
    return Parse(statement.At(0));
}
void Store::Credentials(Json& request, const Json& previous) {
    Json bindings;
    bindings.SetObject();
    auto allowed = AllowedSecrets(request);
    if (request.HasMember("credential_bindings")) {
        Require(request["credential_bindings"].IsObject());
        for (auto i = request["credential_bindings"].MemberBegin(); i != request["credential_bindings"].MemberEnd();
             ++i)
            Require(allowed.count(i->name.GetString()), "UNEXPECTED_SECRET_FIELD");
    }
    for (auto& field : allowed) {
        rapidjson::Value value(rapidjson::kObjectType);
        const auto* incoming =
            request.HasMember("credential_bindings") && request["credential_bindings"].HasMember(field.c_str())
                ? &request["credential_bindings"][field.c_str()]
                : nullptr;
        const auto* old =
            previous.HasMember("credential_bindings") && previous["credential_bindings"].HasMember(field.c_str())
                ? &previous["credential_bindings"][field.c_str()]
                : nullptr;
        std::string operation = "keep";
        if (incoming) {
            Keys(*incoming, {"operation", "value", "reference"});
            operation = Text(*incoming, "operation", "keep");
        }
        if (operation == "keep") {
            Require(!incoming || (!incoming->HasMember("value") && !incoming->HasMember("reference")),
                    "INVALID_SECRET_OPERATION");
            if (old)
                value.CopyFrom(*old, bindings.GetAllocator());
            else
                continue;
        } else if (operation == "clear") {
            Require(!incoming->HasMember("value") && !incoming->HasMember("reference"), "INVALID_SECRET_OPERATION");
            continue;
        } else if (operation == "replace") {
            Require(incoming->HasMember("value") != incoming->HasMember("reference"), "INVALID_SECRET_OPERATION");
            if (incoming->HasMember("reference")) {
                auto ref = Text(*incoming, "reference");
                Require(Ref(ref), "INVALID_CREDENTIAL_REFERENCE");
                String(value, "reference", ref, bindings.GetAllocator());
            } else {
                auto plain = Text(*incoming, "value");
                Require(!plain.empty() && plain != "********" && plain.size() <= 65536, "INVALID_SECRET_VALUE");
                unsigned char random[16];
                Require(RAND_bytes(random, 16) == 1, "CRYPTO_FAILURE", 500);
                std::string id;
                const char hex[] = "0123456789abcdef";
                for (auto byte : random) {
                    id += hex[byte >> 4];
                    id += hex[byte & 15];
                }
                Statement add(db_, "INSERT INTO secrets VALUES(?,?)");
                add.TextBind(1, id);
                add.Bind(2, Encrypt(plain));
                add.Row();
                OPENSSL_cleanse(plain.data(), plain.size());
                String(value, "secret_id", id, bindings.GetAllocator());
            }
        } else
            throw Failure(400, "INVALID_SECRET_OPERATION");
        bindings.AddMember(rapidjson::Value(field.c_str(), bindings.GetAllocator()), value, bindings.GetAllocator());
    }
    request.RemoveMember("credential_bindings");
    request.AddMember("credential_bindings", rapidjson::Value(bindings, request.GetAllocator()),
                      request.GetAllocator());
    if (previous.HasMember("credential_bindings"))
        for (auto i = previous["credential_bindings"].MemberBegin(); i != previous["credential_bindings"].MemberEnd();
             ++i) {
            auto id = Text(i->value, "secret_id");
            if (id.empty()) continue;
            if (bindings.HasMember(i->name) && Text(bindings[i->name], "secret_id") == id) continue;
            Statement remove(db_, "DELETE FROM secrets WHERE id=?");
            remove.TextBind(1, id);
            remove.Row();
        }
}
Json Store::Public(Json definition, const std::string& kind) {
    auto& a = definition.GetAllocator();
    auto name = Text(definition, "name");
    definition.AddMember("in_use", (kind == "connections" ? connections_in_use_[name] : channels_in_use_[name]) > 0, a);
    String(definition, "config_status", "saved", a);
    if (kind == "connections") {
        if (definition.HasMember("credential_bindings"))
            for (auto i = definition["credential_bindings"].MemberBegin();
                 i != definition["credential_bindings"].MemberEnd(); ++i) {
                i->value.RemoveMember("secret_id");
                i->value.AddMember("configured", true, a);
            }
        rapidjson::Value refs(rapidjson::kArrayType);
        Statement rows(db_, "SELECT name FROM channels WHERE connection_name=? ORDER BY name");
        rows.TextBind(1, name);
        while (rows.Row()) refs.PushBack(rapidjson::Value(rows.At(0).c_str(), a), a);
        definition.AddMember("references", refs, a);
    } else {
        auto connection = Get("connections", Text(definition, "connection_name"));
        String(definition, "security_protocol", Text(connection, "security_protocol"), a);
        String(definition, "sasl_mechanism", Text(connection, "sasl_mechanism"), a);
        definition.AddMember("connection_revision", connection["revision"].GetUint64(), a);
        rapidjson::Value status(rapidjson::kObjectType);
        auto role = Text(definition, "role");
        for (auto direction : {"source", "sink"})
            if (role == "both" || role == direction) {
                std::string state = "not_installed";
                const bool packet_source =
                    Text(definition, "payload_kind") == "packet" && std::string(direction) == "source";
                if (Text(definition, "payload_kind") == "structured" || packet_source) {
                    const bool configured = definition.HasMember("payload_options") &&
                                            definition["payload_options"].HasMember(direction) &&
                                            !definition["payload_options"][direction].ObjectEmpty();
                    state = configured ? "ready" : "configuration_required";
                    if (configured && std::string(direction) == "source")
                        String(definition, "source_schema",
                               (packet_source
                                    ? packet::PacketFamilySchema()
                                    : StructuredSchema(ParseStructuredConfig(definition["payload_options"]["source"])))
                                   ->ToString(true),
                               a);
                }
                String(status, direction, state, a);
            }
        definition.AddMember("adapter_status", status, a);
        String(definition, "reference", "kafka." + name, a);
    }
    return definition;
}
Json Store::Mutate(const std::string& kind, const std::string& operation, Json request) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(db_, "PROVIDER_UNAVAILABLE", 503);
    Require(kind == "connections" || kind == "channels");
    Require(operation == "add" || operation == "modify" || operation == "remove");
    auto name = Text(request, "name");
    Require(Named(name), "INVALID_NAME");
    Require(!request.HasMember("revision"), "UNKNOWN_FIELD");
    Json previous;
    previous.SetObject();
    if (operation != "add") {
        previous = Get(kind, name);
        Require(request.HasMember("expected_revision") && request["expected_revision"].IsUint64(),
                "EXPECTED_REVISION_REQUIRED");
        Require(request["expected_revision"].GetUint64() == previous["revision"].GetUint64(), "REVISION_CONFLICT", 409);
        Require((kind == "connections" ? connections_in_use_[name] : channels_in_use_[name]) == 0,
                "CONFIGURATION_IN_USE", 409);
    } else {
        Require(!request.HasMember("expected_revision"), "UNKNOWN_FIELD");
        Statement exists(db_, "SELECT name FROM " + kind + " WHERE name=?");
        exists.TextBind(1, name);
        Require(!exists.Row(), "NAME_CONFLICT", 409);
    }
    if (operation == "remove") {
        Keys(request, {"name", "expected_revision"});
        if (kind == "connections") {
            Statement refs(db_, "SELECT name FROM channels WHERE connection_name=? LIMIT 1");
            refs.TextBind(1, name);
            Require(!refs.Row(), "CONNECTION_REFERENCED", 409);
        }
    }
    Exec("BEGIN IMMEDIATE");
    try {
        if (operation == "remove") {
            if (kind == "connections" && previous.HasMember("credential_bindings"))
                for (auto i = previous["credential_bindings"].MemberBegin();
                     i != previous["credential_bindings"].MemberEnd(); ++i) {
                    auto id = Text(i->value, "secret_id");
                    Statement remove(db_, "DELETE FROM secrets WHERE id=?");
                    remove.TextBind(1, id);
                    remove.Row();
                }
            Statement remove(db_, "DELETE FROM " + kind + " WHERE name=?");
            remove.TextBind(1, name);
            remove.Row();
            Exec("COMMIT");
            return Parse("{\"removed\":true}");
        }
        request.RemoveMember("expected_revision");
        if (kind == "connections") {
            Credentials(request, previous);
            ValidateConnection(request);
        } else {
            ValidateChannel(request);
            Get("connections", Text(request, "connection_name"));
        }
        auto& a = request.GetAllocator();
        request.AddMember("revision", operation == "add" ? uint64_t(1) : previous["revision"].GetUint64() + 1, a);
        if (kind == "connections") {
            Statement insert(
                db_,
                "INSERT INTO connections VALUES(?,?) ON CONFLICT(name) DO UPDATE SET definition=excluded.definition");
            insert.TextBind(1, name);
            insert.TextBind(2, Encode(request));
            insert.Row();
        } else {
            Statement insert(db_,
                             "INSERT INTO channels VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET "
                             "connection_name=excluded.connection_name,definition=excluded.definition");
            insert.TextBind(1, name);
            insert.TextBind(2, Text(request, "connection_name"));
            insert.TextBind(3, Encode(request));
            insert.Row();
        }
        Exec("COMMIT");
        return Public(std::move(request), kind);
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}
Json Store::Query(const std::string& kind, const std::string& name, const std::string& after, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(db_, "PROVIDER_UNAVAILABLE", 503);
    Require(kind == "connections" || kind == "channels");
    Require(limit > 0 && limit <= 100, "INVALID_PAGE_LIMIT");
    Json result;
    result.SetObject();
    rapidjson::Value items(rapidjson::kArrayType);
    Statement rows(db_, "SELECT definition FROM " + kind + " WHERE name>? AND (?='' OR name=?) ORDER BY name LIMIT " +
                            std::to_string(limit));
    rows.TextBind(1, after);
    rows.TextBind(2, name);
    rows.TextBind(3, name);
    while (rows.Row()) {
        auto public_row = Public(Parse(rows.At(0)), kind);
        items.PushBack(rapidjson::Value(public_row, result.GetAllocator()), result.GetAllocator());
    }
    result.AddMember("items", items, result.GetAllocator());
    return result;
}
Binding Store::Acquire(const std::string& name, const std::string& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(db_, "PROVIDER_UNAVAILABLE", 503);
    Require(usage == "source" || usage == "sink", "INVALID_USAGE");
    auto channel = Get("channels", name);
    auto role = Text(channel, "role");
    Require(role == "both" || role == usage, "ROLE_MISMATCH");
    auto connection_name = Text(channel, "connection_name");
    Binding binding;
    binding.connection = Get("connections", connection_name);
    auto& s = binding.snapshot;
    s.channel_name = name;
    s.connection_name = connection_name;
    s.channel_revision = channel["revision"].GetUint64();
    s.connection_revision = binding.connection["revision"].GetUint64();
    s.topic = Text(channel, "topic");
    s.consumer_group = usage == "source" ? Text(channel, "consumer_group") : "";
    s.payload_kind = Text(channel, "payload_kind");
    if (channel.HasMember("payload_options") && channel["payload_options"].HasMember("source"))
        binding.source_options_json = Encode(channel["payload_options"]["source"]);
    if (channel.HasMember("payload_options") && channel["payload_options"].HasMember("sink"))
        binding.sink_options_json = Encode(channel["payload_options"]["sink"]);
    s.usage = usage;
    ResolveSecrets(binding);
    binding.leased = true;
    ++channels_in_use_[name];
    ++connections_in_use_[connection_name];
    return binding;
}
void Store::Release(const KafkaBindingSnapshotV1& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto c = channels_in_use_.find(snapshot.channel_name), n = connections_in_use_.find(snapshot.connection_name);
    if (snapshot.channel_revision != 0 && c != channels_in_use_.end() && c->second) --c->second;
    if (n != connections_in_use_.end() && n->second) --n->second;
}
void Store::ResolveSecrets(Binding& binding) {
    if (binding.connection.HasMember("credential_bindings"))
        for (auto i = binding.connection["credential_bindings"].MemberBegin();
             i != binding.connection["credential_bindings"].MemberEnd(); ++i) {
            auto reference = Text(i->value, "reference");
            if (!reference.empty())
                binding.secrets[std::string("ref:") + i->name.GetString()] = reference;
            else {
                Statement secret(db_, "SELECT cipher FROM secrets WHERE id=?");
                secret.TextBind(1, Text(i->value, "secret_id"));
                Require(secret.Row(), "SECRET_DECRYPTION_FAILED", 500);
                binding.secrets[i->name.GetString()] = Decrypt(secret.At(0));
            }
        }
}
Binding Store::AcquireConnection(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(db_, "PROVIDER_UNAVAILABLE", 503);
    Binding binding;
    binding.connection = Get("connections", name);
    binding.snapshot.connection_name = name;
    binding.snapshot.connection_revision = binding.connection["revision"].GetUint64();
    ResolveSecrets(binding);
    ++connections_in_use_[name];
    binding.leased = true;
    return binding;
}
Binding Store::Draft(Json connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    Require(db_, "PROVIDER_UNAVAILABLE", 503);
    Json previous;
    previous.SetObject();
    if (connection.HasMember("expected_revision")) {
        Require(connection["expected_revision"].IsUint64(), "EXPECTED_REVISION_REQUIRED");
        previous = Get("connections", Text(connection, "name"));
        Require(previous["revision"].GetUint64() == connection["expected_revision"].GetUint64(), "REVISION_CONFLICT",
                409);
        connection.RemoveMember("expected_revision");
    }
    Require(!connection.HasMember("revision"), "UNKNOWN_FIELD");
    Exec("SAVEPOINT draft");
    try {
        Credentials(connection, previous);
        ValidateConnection(connection);
        Binding binding;
        binding.snapshot.connection_name = Text(connection, "name");
        binding.connection = std::move(connection);
        ResolveSecrets(binding);
        Exec("ROLLBACK TO draft; RELEASE draft");
        return binding;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK TO draft; RELEASE draft", nullptr, nullptr, nullptr);
        throw;
    }
}
}  // namespace flowsql::channels::kafka
