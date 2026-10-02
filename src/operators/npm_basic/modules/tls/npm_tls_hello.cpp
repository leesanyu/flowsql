// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_hello.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr size_t kMaxSniBytes = 255;
constexpr size_t kMaxAlpnJsonBytes = 512;
constexpr std::array<uint8_t, 32> kHelloRetryRequestRandom = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

class Reader {
 public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    bool Take(size_t count, std::string_view* value) {
        if (count > bytes_.size()) return false;
        *value = bytes_.substr(0, count);
        bytes_.remove_prefix(count);
        return true;
    }
    bool U8(uint8_t* value) {
        std::string_view bytes;
        if (!Take(1, &bytes)) return false;
        *value = static_cast<uint8_t>(bytes[0]);
        return true;
    }
    bool U16(uint16_t* value) {
        std::string_view bytes;
        if (!Take(2, &bytes)) return false;
        *value = (static_cast<uint8_t>(bytes[0]) << 8) | static_cast<uint8_t>(bytes[1]);
        return true;
    }
    bool Empty() const { return bytes_.empty(); }
    size_t Remaining() const { return bytes_.size(); }

 private:
    std::string_view bytes_;
};

bool ValidSni(std::string_view name) {
    if (name.empty() || name.size() > kMaxSniBytes || name.front() == '.' || name.back() == '.') return false;
    size_t label_length = 0;
    char previous = 0;
    for (unsigned char byte : name) {
        if (byte == '.') {
            if (label_length == 0 || label_length > 63 || previous == '-') return false;
            label_length = 0;
        } else {
            const bool alnum =
                (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
            if (!alnum && byte != '-') return false;
            if (label_length == 0 && byte == '-') return false;
            ++label_length;
        }
        previous = static_cast<char>(byte);
    }
    return label_length > 0 && label_length <= 63 && previous != '-';
}

NpmTlsHelloErrorV1 ParseSni(std::string_view bytes, NpmTlsHelloFactsV1* facts) {
    Reader reader(bytes);
    uint16_t length = 0;
    if (!reader.U16(&length) || length != reader.Remaining() || length == 0) return NpmTlsHelloErrorV1::kMalformed;
    uint8_t type = 0;
    uint16_t name_length = 0;
    std::string_view name;
    if (!reader.U8(&type) || type != 0 || !reader.U16(&name_length) || !reader.Take(name_length, &name) ||
        !reader.Empty()) {
        return NpmTlsHelloErrorV1::kMalformed;
    }
    if (name_length > kMaxSniBytes) return NpmTlsHelloErrorV1::kLimit;
    if (!ValidSni(name)) return NpmTlsHelloErrorV1::kMalformed;
    facts->sni = std::string(name);
    return NpmTlsHelloErrorV1::kNone;
}

NpmTlsHelloErrorV1 ParseAlpn(std::string_view bytes, bool server, NpmTlsHelloFactsV1* facts) {
    Reader reader(bytes);
    uint16_t length = 0;
    if (!reader.U16(&length) || length != reader.Remaining() || length == 0) return NpmTlsHelloErrorV1::kMalformed;
    while (!reader.Empty()) {
        uint8_t protocol_length = 0;
        std::string_view protocol;
        if (!reader.U8(&protocol_length) || protocol_length == 0 || !reader.Take(protocol_length, &protocol)) {
            return NpmTlsHelloErrorV1::kMalformed;
        }
        if (server) {
            if (facts->selected_alpn) return NpmTlsHelloErrorV1::kMalformed;
            facts->selected_alpn = std::string(protocol);
        } else {
            if (std::find(facts->offered_alpn.begin(), facts->offered_alpn.end(), protocol) !=
                facts->offered_alpn.end()) {
                return NpmTlsHelloErrorV1::kMalformed;
            }
            facts->offered_alpn.emplace_back(protocol);
        }
    }
    std::string encoded;
    const auto error = EncodeNpmTlsAlpnJsonV1(
        server ? std::vector<std::string>{*facts->selected_alpn} : facts->offered_alpn, &encoded);
    return error;
}

NpmTlsHelloErrorV1 ParseVersions(std::string_view bytes, bool server, NpmTlsHelloFactsV1* facts) {
    Reader reader(bytes);
    if (server) {
        uint16_t version = 0;
        if (!reader.U16(&version) || !reader.Empty()) return NpmTlsHelloErrorV1::kMalformed;
        facts->selected_version = version;
    } else {
        uint8_t length = 0;
        if (!reader.U8(&length) || length != reader.Remaining() || length == 0 || (length & 1) != 0) {
            return NpmTlsHelloErrorV1::kMalformed;
        }
        while (!reader.Empty()) {
            uint16_t version = 0;
            if (!reader.U16(&version)) return NpmTlsHelloErrorV1::kMalformed;
            facts->offered_versions.push_back(version);
        }
    }
    return NpmTlsHelloErrorV1::kNone;
}

NpmTlsHelloErrorV1 ParseExtensions(Reader* hello, bool server, NpmTlsHelloFactsV1* facts) {
    if (hello->Empty()) return NpmTlsHelloErrorV1::kNone;
    uint16_t length = 0;
    std::string_view bytes;
    if (!hello->U16(&length) || length != hello->Remaining() || !hello->Take(length, &bytes)) {
        return NpmTlsHelloErrorV1::kMalformed;
    }
    Reader extensions(bytes);
    bool seen_sni = false;
    bool seen_alpn = false;
    bool seen_versions = false;
    while (!extensions.Empty()) {
        uint16_t type = 0;
        uint16_t extension_length = 0;
        std::string_view value;
        if (!extensions.U16(&type) || !extensions.U16(&extension_length) ||
            !extensions.Take(extension_length, &value)) {
            return NpmTlsHelloErrorV1::kMalformed;
        }
        NpmTlsHelloErrorV1 error = NpmTlsHelloErrorV1::kNone;
        if (type == 0) {
            if (seen_sni) return NpmTlsHelloErrorV1::kMalformed;
            seen_sni = true;
            if (server)
                error = value.empty() ? NpmTlsHelloErrorV1::kNone : NpmTlsHelloErrorV1::kMalformed;
            else
                error = ParseSni(value, facts);
        } else if (type == 16) {
            if (seen_alpn) return NpmTlsHelloErrorV1::kMalformed;
            seen_alpn = true;
            error = ParseAlpn(value, server, facts);
        } else if (type == 43) {
            if (seen_versions) return NpmTlsHelloErrorV1::kMalformed;
            seen_versions = true;
            error = ParseVersions(value, server, facts);
        }
        if (error != NpmTlsHelloErrorV1::kNone) return error;
    }
    return NpmTlsHelloErrorV1::kNone;
}

NpmTlsHelloErrorV1 ParseClient(std::string_view body, NpmTlsHelloFactsV1* facts) {
    Reader reader(body);
    std::string_view random;
    uint8_t session_length = 0;
    std::string_view ignored;
    uint16_t cipher_length = 0;
    uint8_t compression_length = 0;
    if (!reader.U16(&facts->legacy_version) || !reader.Take(32, &random) || !reader.U8(&session_length) ||
        session_length > 32 || !reader.Take(session_length, &ignored) || !reader.U16(&cipher_length) ||
        cipher_length < 2 || (cipher_length & 1) != 0 || !reader.Take(cipher_length, &ignored) ||
        !reader.U8(&compression_length) || compression_length == 0 || !reader.Take(compression_length, &ignored)) {
        return NpmTlsHelloErrorV1::kMalformed;
    }
    if (ignored.find('\0') == std::string_view::npos) return NpmTlsHelloErrorV1::kMalformed;
    return ParseExtensions(&reader, false, facts);
}

NpmTlsHelloErrorV1 ParseServer(std::string_view body, NpmTlsHelloFactsV1* facts) {
    Reader reader(body);
    std::string_view random;
    uint8_t session_length = 0;
    std::string_view ignored;
    uint16_t cipher = 0;
    uint8_t compression = 0;
    if (!reader.U16(&facts->legacy_version) || !reader.Take(32, &random) || !reader.U8(&session_length) ||
        session_length > 32 || !reader.Take(session_length, &ignored) || !reader.U16(&cipher) ||
        !reader.U8(&compression) || compression != 0) {
        return NpmTlsHelloErrorV1::kMalformed;
    }
    facts->cipher_suite = cipher;
    facts->hello_retry_request =
        std::equal(random.begin(), random.end(), kHelloRetryRequestRandom.begin(),
                   [](char byte, uint8_t expected) { return static_cast<uint8_t>(byte) == expected; });
    return ParseExtensions(&reader, true, facts);
}

}  // namespace

NpmTlsHelloErrorV1 EncodeNpmTlsAlpnJsonV1(const std::vector<std::string>& protocols, std::string* output) {
    if (output == nullptr) return NpmTlsHelloErrorV1::kInvalidOutput;
    try {
        std::string next = "[";
        constexpr char kHex[] = "0123456789abcdef";
        for (const auto& protocol : protocols) {
            if (protocol.empty() || protocol.size() > 255) return NpmTlsHelloErrorV1::kMalformed;
            if (next.size() > 1) next += ',';
            next += '"';
            for (unsigned char byte : protocol) {
                if (byte == '"' || byte == '\\') {
                    next += '\\';
                    next += static_cast<char>(byte);
                } else if (byte < 0x20 || byte > 0x7e) {
                    next += "\\u00";
                    next += kHex[byte >> 4];
                    next += kHex[byte & 0x0f];
                } else {
                    next += static_cast<char>(byte);
                }
                if (next.size() + 2 > kMaxAlpnJsonBytes) return NpmTlsHelloErrorV1::kLimit;
            }
            next += '"';
        }
        next += ']';
        if (next.size() > kMaxAlpnJsonBytes) return NpmTlsHelloErrorV1::kLimit;
        *output = std::move(next);
        return NpmTlsHelloErrorV1::kNone;
    } catch (const std::bad_alloc&) {
        return NpmTlsHelloErrorV1::kAllocationFailed;
    }
}

NpmTlsHelloErrorV1 ParseNpmTlsHelloV1(uint8_t message_type, std::string_view body,
                                      std::optional<int64_t> complete_at_ns, NpmTlsHelloFactsV1* output) {
    if (output == nullptr) return NpmTlsHelloErrorV1::kInvalidOutput;
    if (message_type != 1 && message_type != 2) return NpmTlsHelloErrorV1::kMalformed;
    try {
        NpmTlsHelloFactsV1 next;
        next.complete_at_ns = complete_at_ns;
        const auto error = message_type == 1 ? ParseClient(body, &next) : ParseServer(body, &next);
        if (error == NpmTlsHelloErrorV1::kNone) *output = std::move(next);
        return error;
    } catch (const std::bad_alloc&) {
        return NpmTlsHelloErrorV1::kAllocationFailed;
    }
}

}  // namespace flowsql::npm
