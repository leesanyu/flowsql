// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "config_content_validator.h"

#include <common/json_depth.hpp>

#include <rapidjson/document.h>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/parser.h>

#include <boost/property_tree/detail/rapidxml.hpp>

#include <cerrno>
#include <charconv>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace flowsql::channels::config {
namespace {

constexpr size_t kMaxDepth = 64;

int Invalid(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return EINVAL;
}

bool ValidateJsonNode(const rapidjson::Value& value, size_t depth, std::string* error) {
    if (value.IsObject() || value.IsArray()) {
        if (depth > kMaxDepth) {
            if (error) *error = "JSON nesting depth exceeds 64";
            return false;
        }
    }
    if (value.IsObject()) {
        std::unordered_set<std::string> keys;
        for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
            const std::string key(member->name.GetString(), member->name.GetStringLength());
            if (!keys.insert(key).second) {
                if (error) *error = "JSON object contains duplicate key: " + key;
                return false;
            }
            if (!ValidateJsonNode(member->value, depth + 1, error)) return false;
        }
    } else if (value.IsArray()) {
        for (const auto& item : value.GetArray()) {
            if (!ValidateJsonNode(item, depth + 1, error)) return false;
        }
    }
    return true;
}

int ValidateJson(const std::string& content, std::string* error) {
    if (!JsonNestingWithin(content, kMaxDepth)) return Invalid(error, "JSON nesting depth exceeds 64");
    rapidjson::Document document;
    document.Parse(content.data(), content.size());
    if (document.HasParseError()) return Invalid(error, "invalid JSON syntax");
    return ValidateJsonNode(document, 1, error) ? 0 : EINVAL;
}

class SafeYamlHandler final : public YAML::EventHandler {
 public:
    struct DepthExceeded {};

    void OnDocumentStart(const YAML::Mark&) override {}
    void OnDocumentEnd() override {}

    void OnNull(const YAML::Mark&, YAML::anchor_t anchor) override {
        Scalar("tag:yaml.org,2002:null", "null", anchor);
    }

    void OnAlias(const YAML::Mark&, YAML::anchor_t) override { Fail("YAML alias is not allowed"); }

    void OnScalar(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                  const std::string& value) override {
        Scalar(tag, value, anchor);
    }

    void OnSequenceStart(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                         YAML::EmitterStyle::value) override {
        StartContainer(false, tag, anchor);
    }

    void OnSequenceEnd() override { EndContainer(); }

    void OnMapStart(const YAML::Mark&, const std::string& tag, YAML::anchor_t anchor,
                    YAML::EmitterStyle::value) override {
        StartContainer(true, tag, anchor);
    }

    void OnMapEnd() override { EndContainer(); }
    void OnAnchor(const YAML::Mark&, const std::string&) override { Fail("YAML anchor is not allowed"); }

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

 private:
    enum class Role { kRoot, kKey, kValue };

    struct Frame {
        bool map = false;
        bool expects_key = false;
        Role parent_role = Role::kRoot;
        std::unordered_set<std::string> keys;
    };

    static bool SafeTag(const std::string& tag) {
        static const std::unordered_set<std::string> safe = {
            "", "?", "!", "tag:yaml.org,2002:null", "tag:yaml.org,2002:bool",
            "tag:yaml.org,2002:int", "tag:yaml.org,2002:float", "tag:yaml.org,2002:str",
            "tag:yaml.org,2002:seq", "tag:yaml.org,2002:map", "tag:yaml.org,2002:timestamp",
            "tag:yaml.org,2002:binary", "tag:yaml.org,2002:set", "tag:yaml.org,2002:omap",
            "tag:yaml.org,2002:pairs"};
        return safe.find(tag) != safe.end();
    }

    void Fail(const std::string& message) {
        if (error_.empty()) error_ = message;
    }

    Role BeginNode(bool scalar_key, const std::string& key) {
        if (frames_.empty()) return Role::kRoot;
        Frame& parent = frames_.back();
        if (!parent.map) return Role::kValue;
        if (!parent.expects_key) return Role::kValue;
        if (!scalar_key) {
            Fail("YAML complex mapping key is not allowed");
            parent.expects_key = false;
            return Role::kKey;
        }
        if (!parent.keys.insert(key).second) Fail("YAML mapping contains duplicate key: " + key);
        parent.expects_key = false;
        return Role::kKey;
    }

    void CompleteNode(Role role) {
        if (role == Role::kValue && !frames_.empty() && frames_.back().map) {
            frames_.back().expects_key = true;
        }
    }

    void CheckProperties(const std::string& tag, YAML::anchor_t anchor) {
        if (!SafeTag(tag)) Fail("YAML custom tag is not allowed");
        if (anchor != YAML::NullAnchor) Fail("YAML anchor is not allowed");
    }

    void Scalar(const std::string& tag, const std::string& value, YAML::anchor_t anchor) {
        CheckProperties(tag, anchor);
        const Role role = BeginNode(true, value);
        CompleteNode(role);
    }

    void StartContainer(bool map, const std::string& tag, YAML::anchor_t anchor) {
        CheckProperties(tag, anchor);
        const Role role = BeginNode(false, "");
        if (frames_.size() + 1 > kMaxDepth) throw DepthExceeded{};
        frames_.push_back({map, map, role, {}});
    }

    void EndContainer() {
        if (frames_.empty()) {
            Fail("invalid YAML container structure");
            return;
        }
        const Role role = frames_.back().parent_role;
        frames_.pop_back();
        CompleteNode(role);
    }

    std::vector<Frame> frames_;
    std::string error_;
};

int ValidateYaml(const std::string& content, std::string* error) {
    try {
        std::istringstream input(content);
        YAML::Parser parser(input);
        SafeYamlHandler handler;
        if (!parser.HandleNextDocument(handler)) return Invalid(error, "empty YAML document");
        if (parser.HandleNextDocument(handler)) return Invalid(error, "multiple YAML documents are not allowed");
        return handler.ok() ? 0 : Invalid(error, handler.error());
    } catch (const SafeYamlHandler::DepthExceeded&) {
        return Invalid(error, "YAML nesting depth exceeds 64");
    } catch (const YAML::Exception&) {
        return Invalid(error, "invalid YAML syntax");
    }
}

std::string LowerAscii(const std::string& value) {
    std::string lower = value;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower;
}

bool XmlWhitespace(const char* value, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (!std::isspace(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool ValidXmlReference(const std::string& reference) {
    if (reference == "amp" || reference == "lt" || reference == "gt" ||
        reference == "apos" || reference == "quot") return true;
    if (reference.empty() || reference[0] != '#') return false;
    const bool hex = reference.size() > 1 && reference[1] == 'x';
    const size_t offset = hex ? 2 : 1;
    if (offset == reference.size()) return false;
    uint32_t code = 0;
    const auto parsed = std::from_chars(reference.data() + offset, reference.data() + reference.size(),
                                        code, hex ? 16 : 10);
    if (parsed.ec != std::errc{} || parsed.ptr != reference.data() + reference.size()) return false;
    return code == 9 || code == 10 || code == 13 || (code >= 0x20 && code <= 0xd7ff) ||
           (code >= 0xe000 && code <= 0xfffd) || (code >= 0x10000 && code <= 0x10ffff);
}

bool ValidateXmlReferences(const std::string& content) {
    for (size_t offset = 0; offset < content.size(); ++offset) {
        if (content.compare(offset, 4, "<!--") == 0 || content.compare(offset, 9, "<![CDATA[") == 0) {
            const bool comment = content.compare(offset, 4, "<!--") == 0;
            const size_t end = content.find(comment ? "-->" : "]]>", offset + (comment ? 4 : 9));
            if (end == std::string::npos) return false;
            offset = end + 2;
        } else if (content[offset] == '&') {
            const size_t end = content.find(';', offset + 1);
            if (end == std::string::npos || !ValidXmlReference(content.substr(offset + 1, end - offset - 1))) {
                return false;
            }
            offset = end;
        }
    }
    return true;
}

bool XmlNestingWithin(const std::string& content, size_t max_depth) {
    size_t depth = 0;
    for (size_t offset = 0; offset < content.size();) {
        offset = content.find('<', offset);
        if (offset == std::string::npos) return true;
        if (content.compare(offset, 4, "<!--") == 0 || content.compare(offset, 9, "<![CDATA[") == 0) {
            const bool comment = content.compare(offset, 4, "<!--") == 0;
            const size_t end = content.find(comment ? "-->" : "]]>", offset + (comment ? 4 : 9));
            if (end == std::string::npos) return true;
            offset = end + 3;
            continue;
        }
        if (content.compare(offset, 2, "<?") == 0) {
            const size_t end = content.find("?>", offset + 2);
            if (end == std::string::npos) return true;
            offset = end + 2;
            continue;
        }
        if (content.compare(offset, 2, "<!") == 0) return true;
        const bool closing = offset + 1 < content.size() && content[offset + 1] == '/';
        char quote = '\0';
        size_t end = offset + 1;
        for (; end < content.size(); ++end) {
            const char ch = content[end];
            if (quote) {
                if (ch == quote) quote = '\0';
            } else if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (ch == '>') {
                break;
            }
        }
        if (end == content.size()) return true;
        size_t tail = end;
        while (tail > offset && std::isspace(static_cast<unsigned char>(content[tail - 1]))) --tail;
        const bool empty = tail > offset && content[tail - 1] == '/';
        if (closing) {
            if (depth > 0) --depth;
        } else if (!empty && ++depth > max_depth) {
            return false;
        }
        offset = end + 1;
    }
    return true;
}

using XmlNode = boost::property_tree::detail::rapidxml::xml_node<char>;
bool ValidateXmlNode(XmlNode* node, size_t depth, std::string* error) {
    using namespace boost::property_tree::detail::rapidxml;
    if (node->type() == node_doctype) {
        if (error) *error = "XML DTD is not allowed";
        return false;
    }
    if (node->type() == node_pi) {
        if (error) *error = "XML processing instruction is not allowed";
        return false;
    }
    if (node->type() != node_element) return true;
    if (depth > kMaxDepth) {
        if (error) *error = "XML nesting depth exceeds 64";
        return false;
    }
    for (auto* attribute = node->first_attribute(); attribute; attribute = attribute->next_attribute()) {
        const std::string value(attribute->value(), attribute->value_size());
        if (value == "http://www.w3.org/2001/XInclude") {
            if (error) *error = "XML XInclude is not allowed";
            return false;
        }
    }
    for (auto* child = node->first_node(); child; child = child->next_sibling()) {
        if (!ValidateXmlNode(child, depth + 1, error)) return false;
    }
    return true;
}

int ValidateXml(const std::string& content, std::string* error) {
    using namespace boost::property_tree::detail::rapidxml;
    if (content.find('\0') != std::string::npos || !ValidateXmlReferences(content)) {
        return Invalid(error, "invalid XML character or entity reference");
    }
    if (!XmlNestingWithin(content, kMaxDepth)) return Invalid(error, "XML nesting depth exceeds 64");
    const std::string lower = LowerAscii(content);
    if (lower.find("<!doctype") != std::string::npos || lower.find("<!entity") != std::string::npos) {
        return Invalid(error, "XML DTD and entities are not allowed");
    }
    std::vector<char> buffer(content.begin(), content.end());
    buffer.push_back('\0');
    xml_document<char> document;
    try {
        document.parse<parse_full>(buffer.data());
    } catch (const parse_error&) {
        return Invalid(error, "invalid XML syntax");
    }
    size_t roots = 0;
    for (auto* node = document.first_node(); node; node = node->next_sibling()) {
        if (node->type() == node_element) {
            ++roots;
            if (!ValidateXmlNode(node, 1, error)) return EINVAL;
        } else if (node->type() == node_data && !XmlWhitespace(node->value(), node->value_size())) {
            return Invalid(error, "XML text outside the root element is not allowed");
        } else if (node->type() == node_doctype || node->type() == node_pi) {
            return Invalid(error, "XML external declaration or processing instruction is not allowed");
        }
    }
    return roots == 1 ? 0 : Invalid(error, "XML must contain exactly one root element");
}

}  // namespace

int ValidateConfigContent(const std::string& format, const std::string& content, std::string* error) {
    if (error) error->clear();
    if (format == "json") return ValidateJson(content, error);
    if (format == "yaml") return ValidateYaml(content, error);
    if (format == "xml") return ValidateXml(content, error);
    return Invalid(error, "unsupported configuration format");
}

}  // namespace flowsql::channels::config
