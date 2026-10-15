// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_store.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
using namespace flowsql::channels::kafka;
static void Reject(const std::function<void()>& call, int status) {
    try {
        call();
        assert(false);
    } catch (const Failure& failure) {
        assert(failure.status == status);
    }
}
int main() {
    auto path = std::filesystem::temp_directory_path() / ("flowsql-kafka-store-" + std::to_string(getpid()) + ".db");
    std::array<unsigned char, 32> key{};
    key.fill(0x35);
    Store store;
    store.Open(path.string(), key);
    auto connection = Parse(
        R"({"name":"cluster","bootstrap_servers":["localhost:9092"],"security_protocol":"SASL_SSL","sasl_mechanism":"SCRAM-SHA-256","sasl":{"username":"tester"},"credential_bindings":{"password":{"operation":"replace","value":"sensitive-test-password"}}})");
    auto saved = store.Mutate("connections", "add", Parse(Encode(connection)));
    assert(saved["revision"].GetUint64() == 1);
    assert(Encode(saved).find("sensitive-test-password") == std::string::npos);
    store.Mutate(
        "channels", "add",
        Parse(
            R"({"name":"events","connection_name":"cluster","role":"both","topic":"events","consumer_group":"analysis","payload_kind":"structured","payload_options":{}})"));
    Reject([&] { store.Mutate("connections", "remove", Parse(R"({"name":"cluster","expected_revision":1})")); }, 409);
    auto source = store.Acquire("events", "source"), sink = store.Acquire("events", "sink");
    assert(source.secrets.at("password") == "sensitive-test-password");
    assert(source.snapshot.usage == "source" && sink.snapshot.usage == "sink");
    Reject([&] { store.Mutate("channels", "remove", Parse(R"({"name":"events","expected_revision":1})")); }, 409);
    store.Release(source.snapshot);
    store.Release(sink.snapshot);
    auto source_only = Parse(
        R"({"name":"source_only","connection_name":"cluster","role":"source","topic":"events","consumer_group":"other","payload_kind":"packet"})");
    store.Mutate("channels", "add", std::move(source_only));
    Reject([&] { store.Acquire("source_only", "sink"); }, 400);
    Reject(
        [&] {
            store.Mutate(
                "channels", "add",
                Parse(
                    R"({"name":"bad","connection_name":"missing","role":"sink","topic":"events","payload_kind":"structured"})"));
        },
        404);
    Reject(
        [&] {
            store.Mutate(
                "channels", "add",
                Parse(
                    R"({"name":"bad","connection_name":"cluster","role":"sink","topic":"events","consumer_group":"bad","payload_kind":"structured"})"));
        },
        400);
    Reject([&] { Parse(R"({"name":"x","name":"y"})"); }, 400);
    Reject(
        [&] {
            store.Mutate(
                "connections", "add",
                Parse(
                    R"({"name":"wrong","bootstrap_servers":["localhost:9092"],"security_protocol":"PLAINTEXT","sasl":{"username":"x"}})"));
        },
        400);
    auto request = Parse(Encode(connection));
    request.RemoveMember("credential_bindings");
    request.AddMember("expected_revision", 1, request.GetAllocator());
    std::atomic<int> successes{0}, conflicts{0};
    auto mutate = [&] {
        try {
            store.Mutate("connections", "modify", Parse(Encode(request)));
            ++successes;
        } catch (const Failure& e) {
            assert(e.status == 409);
            ++conflicts;
        }
    };
    std::thread a(mutate), b(mutate);
    a.join();
    b.join();
    assert(successes == 1 && conflicts == 1);
    auto row = store.Query("connections", "cluster");
    assert(row["items"][0]["revision"].GetUint64() == 2);
    store.Close();
    Store recovered;
    recovered.Open(path.string(), key);
    auto binding = recovered.Acquire("events", "source");
    assert(binding.snapshot.connection_revision == 2 && binding.secrets.at("password") == "sensitive-test-password");
    recovered.Release(binding.snapshot);
    recovered.Close();
    std::ifstream bytes(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(bytes)), {});
    assert(content.find("sensitive-test-password") == std::string::npos);
    Store wrong;
    auto wrong_key = key;
    wrong_key[0] ^= 1;
    Reject([&] { wrong.Open(path.string(), wrong_key); }, 500);
    std::filesystem::remove(path);
    puts("PASS: atomic revisions, roles, leases, reference protection, encrypted persistence and recovery");
}
