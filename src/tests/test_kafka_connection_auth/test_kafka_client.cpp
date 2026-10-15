// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flowsql::channels::kafka;
int main() {
    Store store;
    std::array<unsigned char, 32> key{};
    store.Open(":memory:", key);
    store.Mutate("connections", "add",
                 Parse(R"({"name":"offline","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
    store.Mutate(
        "channels", "add",
        Parse(
            R"({"name":"events","connection_name":"offline","role":"both","topic":"events","consumer_group":"test","payload_kind":"structured"})"));
    Deployment deployment;
    auto binding = store.Acquire("events", "source");
    Client client(std::move(binding), deployment);
    void* capability = reinterpret_cast<void*>(1);
    assert(client.QueryInterface({}, &capability) == ENOTSUP && !capability);
    std::string output;
    auto start = std::chrono::steady_clock::now();
    std::thread waiter([&] { assert(client.Probe(30000, &output) == ECANCELED); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.Cancel();
    waiter.join();
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    assert(output.find("CANCELLED") != std::string::npos && output.find("unverified") != std::string::npos);
    store.Release(client.Snapshot());
    assert(Encode(Client::Capabilities()).find("oidc") != std::string::npos);
    auto temporary = std::filesystem::temp_directory_path() / ("flowsql-kafka-provider-" + std::to_string(getpid()));
    std::filesystem::create_directory(temporary);
    {
        std::ofstream master(temporary / "key", std::ios::binary);
        master.write(reinterpret_cast<char*>(key.data()), key.size());
    }
    Plugin provider;
    auto options = Parse("{}");
    auto& allocator = options.GetAllocator();
    String(options, "db_path", (temporary / "definitions.db").string(), allocator);
    String(options, "master_key_file", (temporary / "key").string(), allocator);
    String(options, "runtime_directory", (temporary / "runtime").string(), allocator);
    assert(provider.Option(Encode(options).c_str()) == 0 && provider.Load(nullptr) == 0);
    provider.Definitions().Mutate(
        "connections", "add",
        Parse(R"({"name":"offline","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
    provider.Definitions().Mutate(
        "channels", "add",
        Parse(
            R"({"name":"source","connection_name":"offline","role":"source","topic":"events","consumer_group":"test","payload_kind":"packet"})"));
    flowsql::IKafkaClientSessionV1* leased = nullptr;
    std::string error;
    assert(provider.OpenSession("source", "sink", &leased, &error) != 0 && leased == nullptr);
    assert(provider.OpenSession("source", "source", &leased, &error) == 0 && leased);
    std::atomic<bool> stopped{false};
    std::thread stop([&] {
        assert(provider.Stop() == 0);
        stopped = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!stopped);
    provider.ReleaseSession(leased);
    stop.join();
    assert(stopped && provider.Unload() == 0);
    std::filesystem::remove_all(temporary);
    if (const char* file = getenv("FLOWSQL_KAFKA_CONNECTION_JSON")) {
        std::ifstream input(file);
        std::string json((std::istreambuf_iterator<char>(input)), {});
        auto connection = Parse(json);
        store.Mutate("connections", "add", std::move(connection));
        store.Mutate(
            "channels", "add",
            Parse(
                R"({"name":"real","connection_name":"real","role":"source","topic":"events","consumer_group":"test","payload_kind":"structured"})"));
        if (const char* path = getenv("FLOWSQL_KAFKA_CA")) deployment.files["ca"] = path;
        if (const char* path = getenv("FLOWSQL_KAFKA_KEYTAB")) deployment.files["keytab"] = path;
        if (const char* path = getenv("FLOWSQL_KAFKA_CERT")) deployment.files["certificate"] = path;
        if (const char* path = getenv("FLOWSQL_KAFKA_KEY")) deployment.files["key"] = path;
        if (const char* path = getenv("FLOWSQL_KAFKA_OAUTH_CA")) deployment.files["oauth_ca"] = path;
        deployment.runtime_directory =
            getenv("FLOWSQL_KAFKA_RUNTIME") ? getenv("FLOWSQL_KAFKA_RUNTIME") : "/tmp/flowsql-kafka-test";
        auto frozen = store.Acquire("real", "source");
        if (const char* topic = getenv("FLOWSQL_KAFKA_TOPIC")) frozen.snapshot.topic = topic;
        std::unique_ptr<Client> second;
        if (const char* other = getenv("FLOWSQL_KAFKA_SECOND_KEYTAB")) {
            Binding parallel;
            parallel.snapshot = frozen.snapshot;
            parallel.snapshot.topic =
                getenv("FLOWSQL_KAFKA_SECOND_TOPIC") ? getenv("FLOWSQL_KAFKA_SECOND_TOPIC") : "eventsbob";
            parallel.connection = Parse(Encode(frozen.connection));
            String(parallel.connection["sasl"], "principal", "bob@FLOWSQL.TEST", parallel.connection.GetAllocator());
            auto private_deployment = deployment;
            private_deployment.files["keytab"] = other;
            second = std::make_unique<Client>(std::move(parallel), private_deployment);
        }
        std::string second_result;
        std::thread concurrent([&] {
            if (second) assert(second->Probe(10000, &second_result) == 0);
        });
        Client real(std::move(frozen), deployment);
        int result = real.Probe(10000, &output);
        std::cout << output << std::endl;
        concurrent.join();
        bool expected_failure = getenv("FLOWSQL_KAFKA_EXPECT_FAILURE") != nullptr;
        assert(expected_failure ? result != 0 : result == 0);
        if (const char* seconds = getenv("FLOWSQL_KAFKA_REPEAT_SECONDS")) {
            std::this_thread::sleep_for(std::chrono::seconds(std::stoi(seconds)));
            result = real.Probe(10000, &output);
            std::cout << output << std::endl;
            assert(getenv("FLOWSQL_KAFKA_REPEAT_EXPECT_FAILURE") ? result != 0 : result == 0);
            if (second) assert(second->Probe(10000, &second_result) == 0);
        }
        store.Release(real.Snapshot());
    }
    puts("PASS: metadata-only client, capabilities, isolated binding and bounded cancellation");
}
