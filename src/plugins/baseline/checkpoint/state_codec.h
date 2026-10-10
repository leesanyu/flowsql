// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "state_fields.h"

namespace flowsql::baseline::checkpoint {
class Invalid : public std::runtime_error {
 public:
    using std::runtime_error::runtime_error;
};
inline void Require(bool condition, const char* message) {
    if (!condition) throw Invalid(message);
}
struct BoundedStream {
    using Ch = char;
    std::string content;
    size_t limit;
    explicit BoundedStream(size_t limit) : limit(limit) {}
    void Put(char c) {
        Require(content.size() < limit, "checkpoint byte limit");
        content.push_back(c);
    }
    void Flush() {}
};
using Writer = rapidjson::Writer<BoundedStream>;
inline std::string Hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(text.size() * 2);
    for (unsigned char c : text) {
        encoded += digits[c >> 4];
        encoded += digits[c & 15];
    }
    return encoded;
}
inline std::string Unhex(std::string_view text) {
    Require(text.size() % 2 == 0, "checkpoint binary string length");
    auto digit = [](char c) -> unsigned {
        Require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "checkpoint binary string digit");
        return c <= '9' ? c - '0' : c - 'a' + 10;
    };
    std::string decoded;
    decoded.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) decoded.push_back(char((digit(text[i]) << 4) | digit(text[i + 1])));
    return decoded;
}
template <class T>
struct Vector : std::false_type {};
template <class T, class A>
struct Vector<std::vector<T, A>> : std::true_type {};
template <class T>
struct Array : std::false_type {};
template <class T, size_t N>
struct Array<std::array<T, N>> : std::true_type {};
template <class T>
struct Map : std::false_type {};
template <class K, class V, class H, class E, class A>
struct Map<std::unordered_map<K, V, H, E, A>> : std::true_type {};
template <class K, class V, class C, class A>
struct Map<std::map<K, V, C, A>> : std::true_type {};
template <class T>
struct Optional : std::false_type {};
template <class T>
struct Optional<std::optional<T>> : std::true_type {};
template <class T>
struct Shared : std::false_type {};
template <class T>
struct Shared<std::shared_ptr<T>> : std::true_type {};
template <class T>
struct Unique : std::false_type {};
template <class T, class D>
struct Unique<std::unique_ptr<T, D>> : std::true_type {};
template <class T>
T DefaultValue() {
    if constexpr (std::is_same_v<T, RelationBasisRuntimeState>)
        return T(RelationBasisRuntimeConfig{});
    else if constexpr (std::is_same_v<T, RelationStreamBasisAccumulator>)
        return T(RelationStreamBasisConfig{});
    else
        return T{};
}
template <class T>
void Write(Writer& writer, const T& value);
template <class T>
void Read(const rapidjson::Value& json, T& value);
struct WriteFields {
    Writer& writer;
    template <class T>
    void operator()(const char* name, const T& value) {
        writer.Key(name);
        Write(writer, value);
    }
};
struct ReadFields {
    const rapidjson::Value& object;
    size_t count = 0;
    explicit ReadFields(const rapidjson::Value& object) : object(object) {
        Require(object.IsObject(), "checkpoint object required");
    }
    template <class T>
    void operator()(const char* name, T& value) {
        auto member = object.FindMember(name);
        Require(member != object.MemberEnd(), "checkpoint field missing");
        Read(member->value, value);
        ++count;
    }
    void Finish() const {
        Require(count == object.MemberCount(), "checkpoint fields mismatch");
        for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it)
            for (auto other = object.MemberBegin(); other != it; ++other)
                Require(it->name != other->name, "checkpoint duplicate field");
    }
};
template <class T>
void Write(Writer& w, const T& v) {
    if constexpr (std::is_same_v<T, bool>)
        w.Bool(v);
    else if constexpr (std::is_enum_v<T>)
        w.Int64(static_cast<int64_t>(v));
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        w.Int64(v);
    else if constexpr (std::is_integral_v<T>)
        w.Uint64(v);
    else if constexpr (std::is_floating_point_v<T>) {
        Require(std::isfinite(v), "nonfinite checkpoint state");
        w.Double(v);
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto hex = Hex(v);
        w.String(hex.data(), hex.size());
    } else if constexpr (Vector<T>::value || Array<T>::value) {
        w.StartArray();
        for (const auto& item : v) Write(w, item);
        w.EndArray();
    } else if constexpr (Map<T>::value) {
        std::vector<const typename T::value_type*> ordered;
        ordered.reserve(v.size());
        for (const auto& item : v) ordered.push_back(&item);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
        w.StartArray();
        for (const auto* item : ordered) {
            w.StartArray();
            Write(w, item->first);
            Write(w, item->second);
            w.EndArray();
        }
        w.EndArray();
    } else if constexpr (Optional<T>::value || Shared<T>::value || Unique<T>::value) {
        if (v)
            Write(w, *v);
        else
            w.Null();
    } else {
        w.StartObject();
        WriteFields fields{w};
        Fields<T>::Visit(fields, v);
        w.EndObject();
    }
}
template <class T>
void Read(const rapidjson::Value& j, T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        Require(j.IsBool(), "checkpoint bool required");
        v = j.GetBool();
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        U integer;
        Read(j, integer);
        v = static_cast<T>(integer);
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        Require(j.IsInt64() && j.GetInt64() >= std::numeric_limits<T>::min() &&
                    j.GetInt64() <= std::numeric_limits<T>::max(),
                "checkpoint signed integer range");
        v = static_cast<T>(j.GetInt64());
    } else if constexpr (std::is_integral_v<T>) {
        Require(j.IsUint64() && j.GetUint64() <= std::numeric_limits<T>::max(), "checkpoint unsigned integer range");
        v = static_cast<T>(j.GetUint64());
    } else if constexpr (std::is_floating_point_v<T>) {
        Require(j.IsNumber() && std::isfinite(j.GetDouble()), "checkpoint finite number required");
        v = j.GetDouble();
    } else if constexpr (std::is_same_v<T, std::string>) {
        Require(j.IsString(), "checkpoint binary string required");
        v = Unhex({j.GetString(), j.GetStringLength()});
    } else if constexpr (Vector<T>::value) {
        Require(j.IsArray(), "checkpoint array required");
        v.clear();
        v.reserve(j.Size());
        for (const auto& item : j.GetArray()) {
            auto decoded = DefaultValue<typename T::value_type>();
            Read(item, decoded);
            v.push_back(std::move(decoded));
        }
    } else if constexpr (Array<T>::value) {
        Require(j.IsArray() && j.Size() == v.size(), "checkpoint fixed array length");
        for (size_t i = 0; i < v.size(); ++i) Read(j[static_cast<rapidjson::SizeType>(i)], v[i]);
    } else if constexpr (Map<T>::value) {
        Require(j.IsArray(), "checkpoint map required");
        v.clear();
        for (const auto& item : j.GetArray()) {
            Require(item.IsArray() && item.Size() == 2, "checkpoint map entry");
            typename T::key_type key{};
            auto decoded = DefaultValue<typename T::mapped_type>();
            Read(item[0], key);
            Read(item[1], decoded);
            Require(v.emplace(std::move(key), std::move(decoded)).second, "checkpoint duplicate key");
        }
    } else if constexpr (Optional<T>::value) {
        if (j.IsNull())
            v.reset();
        else {
            auto decoded = DefaultValue<typename T::value_type>();
            Read(j, decoded);
            v = std::move(decoded);
        }
    } else if constexpr (Unique<T>::value) {
        if (j.IsNull())
            v.reset();
        else {
            auto decoded = std::make_unique<typename T::element_type>();
            Read(j, *decoded);
            v = std::move(decoded);
        }
    } else if constexpr (Shared<T>::value) {
        if (j.IsNull())
            v.reset();
        else {
            auto decoded = std::make_shared<typename T::element_type>();
            Read(j, *decoded);
            v = std::move(decoded);
        }
    } else {
        ReadFields fields{j};
        Fields<T>::Visit(fields, v);
        fields.Finish();
    }
}
template <class T>
std::string Encode(const T& value, size_t limit) {
    BoundedStream stream(limit);
    Writer writer(stream);
    Write(writer, value);
    return std::move(stream.content);
}
inline rapidjson::Document Parse(std::string_view content) {
    // Insitu-free parsing produces an entirely owned document; validate depth before recursion.
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char c : content) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            Require(++depth <= 64, "checkpoint nesting limit");
        } else if (c == '}' || c == ']')
            --depth;
    }
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag>(content.data(),
                                                                                          content.size());
    Require(!doc.HasParseError(), "checkpoint JSON parse failed");
    return doc;
}
}  // namespace flowsql::baseline::checkpoint
