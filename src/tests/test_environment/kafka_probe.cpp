// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_client.h>
#include <channels/kafka/kafka_store.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql::channels::kafka;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream input(argv[1]);
        auto profiles = Parse(std::string((std::istreambuf_iterator<char>(input)), {}));
        if (!profiles.IsObject() || !profiles.HasMember(argv[2])) return 2;
        const auto& profile = profiles[argv[2]];
        Store store;
        std::array<unsigned char, 32> key{};
        store.Open(":memory:", key);
        store.Mutate("connections", "add", Parse(Encode(profile["connection"])));
        auto channel = Parse(
            R"({"name":"environment_probe","connection_name":"real","role":"source","topic":"events","consumer_group":"environment_probe","payload_kind":"structured"})");
        String(channel, "topic", profile["topic"].GetString(), channel.GetAllocator());
        store.Mutate("channels", "add", std::move(channel));
        auto binding = store.Acquire("environment_probe", "source");
        Deployment deployment;
        for (const auto& item : profile["files"].GetObject())
            deployment.files[item.name.GetString()] = item.value.GetString();
        deployment.runtime_directory = profile["runtime"].GetString();
        if (binding.connection.HasMember("sasl_mechanism") &&
            std::string(binding.connection["sasl_mechanism"].GetString()) == "GSSAPI") {
            const auto config = std::filesystem::path(deployment.files["keytab"]).parent_path() / "krb5.conf";
            setenv("KRB5_CONFIG", config.c_str(), 1);
        }
        Client client(std::move(binding), deployment);
        std::string report;
        const int result = client.Probe(3000, &report);
        std::cout << report << '\n';
        return result ? 1 : 0;
    } catch (const Failure& error) {
        std::cout << "{\"status\":\"failed\",\"code\":\"" << error.code << "\",\"stage\":\"" << error.stage << "\"}\n";
        return 1;
    } catch (...) {
        std::cout << "{\"status\":\"failed\",\"error\":{\"code\":\"CONFIG_INVALID\"}}\n";
        return 1;
    }
}
