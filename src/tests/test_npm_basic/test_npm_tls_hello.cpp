// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_hello.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npm = flowsql::npm;

namespace {

void U16(std::string* bytes, uint16_t value) {
    bytes->push_back(static_cast<char>(value >> 8));
    bytes->push_back(static_cast<char>(value));
}

std::string Ext(uint16_t type, std::string value) {
    std::string result;
    U16(&result, type);
    U16(&result, static_cast<uint16_t>(value.size()));
    return result + value;
}

std::string Sni(std::string name) {
    std::string entry(1, 0);
    U16(&entry, static_cast<uint16_t>(name.size()));
    entry += name;
    std::string value;
    U16(&value, static_cast<uint16_t>(entry.size()));
    return Ext(0, value + entry);
}

std::string Alpn(std::vector<std::string> protocols) {
    std::string list;
    for (const auto& protocol : protocols) {
        list.push_back(static_cast<char>(protocol.size()));
        list += protocol;
    }
    std::string value;
    U16(&value, static_cast<uint16_t>(list.size()));
    return Ext(16, value + list);
}

std::string Versions(std::vector<uint16_t> versions) {
    std::string value(1, static_cast<char>(versions.size() * 2));
    for (auto version : versions) U16(&value, version);
    return Ext(43, value);
}

std::string Client(std::string extensions = {}) {
    std::string body;
    U16(&body, 0x0303);
    body += std::string(32, 'c');
    body.push_back(0);
    U16(&body, 2);
    U16(&body, 0x1301);
    body.push_back(1);
    body.push_back(0);
    if (!extensions.empty()) {
        U16(&body, static_cast<uint16_t>(extensions.size()));
        body += extensions;
    }
    return body;
}

std::string Server(std::string extensions = {}, bool retry = false) {
    std::string body;
    U16(&body, 0x0303);
    if (retry) {
        const uint8_t random[] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
                                  0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
                                  0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};
        body.append(reinterpret_cast<const char*>(random), sizeof(random));
    } else {
        body += std::string(32, 's');
    }
    body.push_back(0);
    U16(&body, 0x1301);
    body.push_back(0);
    if (!extensions.empty()) {
        U16(&body, static_cast<uint16_t>(extensions.size()));
        body += extensions;
    }
    return body;
}

void TestClientAndServer() {
    using Error = npm::NpmTlsHelloErrorV1;
    npm::NpmTlsHelloFactsV1 facts;
    auto client =
        Client(Ext(65000, "opaque") + Sni("example.com") + Alpn({"h2", "http/1.1"}) + Versions({0x0304, 0x0303}));
    assert(npm::ParseNpmTlsHelloV1(1, client, 123, &facts) == Error::kNone);
    client.assign(client.size(), '\0');
    assert(facts.legacy_version == 0x0303 && facts.complete_at_ns == 123);
    assert(facts.sni == "example.com" && facts.offered_alpn == std::vector<std::string>({"h2", "http/1.1"}));
    assert(facts.offered_versions == std::vector<uint16_t>({0x0304, 0x0303}));
    std::string json;
    assert(npm::EncodeNpmTlsAlpnJsonV1(facts.offered_alpn, &json) == Error::kNone);
    assert(json == R"(["h2","http/1.1"])");

    assert(npm::ParseNpmTlsHelloV1(1, Client(), std::nullopt, &facts) == Error::kNone);
    assert(!facts.sni && facts.offered_alpn.empty() && facts.offered_versions.empty() && !facts.complete_at_ns);
    auto server = Server(Ext(43, std::string("\x03\x04", 2)), true);
    assert(npm::ParseNpmTlsHelloV1(2, server, 234, &facts) == Error::kNone);
    server.clear();
    assert(facts.hello_retry_request && facts.selected_version == 0x0304 && facts.cipher_suite == 0x1301);
    assert(facts.complete_at_ns == 234 && !facts.selected_alpn);
    assert(npm::ParseNpmTlsHelloV1(2, Server(Alpn({"h2"})), 345, &facts) == Error::kNone);
    assert(!facts.hello_retry_request && facts.selected_alpn == "h2" && !facts.selected_version);
}

void TestErrorsAndBounds() {
    using Error = npm::NpmTlsHelloErrorV1;
    npm::NpmTlsHelloFactsV1 facts;
    facts.sni = "sentinel";
    const auto reject = [&](uint8_t type, const std::string& body, Error error) {
        assert(npm::ParseNpmTlsHelloV1(type, body, 456, &facts) == error);
        assert(facts.sni == "sentinel");
    };
    reject(3, Client(), Error::kMalformed);
    reject(1, Client().substr(0, 32), Error::kMalformed);
    reject(1, Client(Sni("bad..name")), Error::kMalformed);
    reject(1, Client(Sni("bad_name")), Error::kMalformed);
    reject(1, Client(Sni(std::string(256, 'a'))), Error::kLimit);
    reject(1, Client(Sni("a.example") + Sni("b.example")), Error::kMalformed);
    reject(1, Client(Alpn({"h2", "h2"})), Error::kMalformed);
    reject(1, Client(Alpn({"h2"}) + Alpn({"h3"})), Error::kMalformed);
    reject(1, Client(Versions({0x0304}) + Versions({0x0303})), Error::kMalformed);
    reject(1, Client(Ext(43, std::string("\x03\x03\x04", 3))), Error::kMalformed);
    reject(1, Client(Ext(65000, "opaque").substr(0, 5)), Error::kMalformed);
    reject(2, Server(Alpn({"h2", "h3"})), Error::kMalformed);
    reject(2, Server(Ext(0, "invalid")), Error::kMalformed);
    reject(2, Server(Ext(43, std::string("\x03", 1))), Error::kMalformed);
    reject(1, Client(Alpn({std::string(100, static_cast<char>(0x80))})), Error::kLimit);
    assert(npm::ParseNpmTlsHelloV1(1, Client(), 0, nullptr) == Error::kInvalidOutput);

    std::string encoded = "sentinel";
    assert(npm::EncodeNpmTlsAlpnJsonV1({"x\"\\", std::string("\x00\xff", 2)}, &encoded) == Error::kNone);
    assert(encoded == "[\"x\\\"\\\\\",\"\\u0000\\u00ff\"]");
    assert(npm::EncodeNpmTlsAlpnJsonV1({std::string(100, static_cast<char>(0x80))}, &encoded) == Error::kLimit);
    assert(encoded == "[\"x\\\"\\\\\",\"\\u0000\\u00ff\"]");
}

}  // namespace

int main() {
    TestClientAndServer();
    TestErrorsAndBounds();
}
