#include "control_message.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <ctime>

#include "control_protocol.h"

namespace flowsql {
namespace bridge {

std::string ControlMessage::BuildCommand(const std::string& type, const std::string& payload_json) {
    rapidjson::Document doc;
    doc.SetObject();
    auto& allocator = doc.GetAllocator();

    // version
    doc.AddMember("version", rapidjson::StringRef(PROTOCOL_VERSION), allocator);

    // type
    rapidjson::Value type_val;
    type_val.SetString(type.c_str(), type.length(), allocator);
    doc.AddMember("type", type_val, allocator);

    // timestamp
    doc.AddMember("timestamp", static_cast<int64_t>(std::time(nullptr)), allocator);

    // payload
    rapidjson::Document payload_doc;
    payload_doc.Parse(payload_json.c_str());
    if (!payload_doc.HasParseError() && payload_doc.IsObject()) {
        rapidjson::Value payload_val(payload_doc, allocator);
        doc.AddMember("payload", payload_val, allocator);
    } else {
        // 如果解析失败，使用空对象
        rapidjson::Value empty_obj(rapidjson::kObjectType);
        doc.AddMember("payload", empty_obj, allocator);
    }

    // 序列化
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    return std::string(buffer.GetString()) + "\n";
}

std::string ControlMessage::BuildShutdown() {
    return BuildCommand(MessageType::SHUTDOWN);
}

std::string ControlMessage::BuildReloadOperators() {
    return BuildCommand(MessageType::RELOAD_OPERATORS);
}

int ControlMessage::ParseMessage(const std::string& json, std::string* type, std::string* payload) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());

    if (doc.HasParseError() || !doc.IsObject()) {
        printf("ControlMessage::ParseMessage: invalid JSON\n");
        return -1;
    }

    // 检查版本
    if (!doc.HasMember("version") || !doc["version"].IsString()) {
        printf("ControlMessage::ParseMessage: missing version\n");
        return -1;
    }

    // 提取 type
    if (!doc.HasMember("type") || !doc["type"].IsString()) {
        printf("ControlMessage::ParseMessage: missing type\n");
        return -1;
    }
    *type = doc["type"].GetString();

    // 提取 payload（序列化为 JSON 字符串）
    if (doc.HasMember("payload") && doc["payload"].IsObject()) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc["payload"].Accept(writer);
        *payload = buffer.GetString();
    } else {
        *payload = "{}";
    }

    return 0;
}

int ControlMessage::ParseOperatorList(const std::string& payload_json, std::vector<OperatorMeta>* operators) {
    rapidjson::Document doc;
    doc.Parse(payload_json.c_str());

    if (doc.HasParseError() || !doc.IsObject()) {
        return -1;
    }

    if (!doc.HasMember("operators") || !doc["operators"].IsArray()) {
        return -1;
    }

    operators->clear();
    for (auto& item : doc["operators"].GetArray()) {
        if (!item.IsObject()) continue;

        OperatorMeta meta;
        if (item.HasMember("catelog") && item["catelog"].IsString())
            meta.catelog = item["catelog"].GetString();
        if (item.HasMember("name") && item["name"].IsString())
            meta.name = item["name"].GetString();
        if (item.HasMember("description") && item["description"].IsString())
            meta.description = item["description"].GetString();
        if (item.HasMember("position") && item["position"].IsString()) {
            std::string pos = item["position"].GetString();
            meta.position = (pos == "STORAGE") ? OperatorPosition::STORAGE : OperatorPosition::DATA;
        }

        if (!meta.catelog.empty() && !meta.name.empty()) {
            operators->push_back(std::move(meta));
        }
    }

    return 0;
}

int ControlMessage::ParseOperatorMeta(const std::string& payload_json, OperatorMeta* meta) {
    rapidjson::Document doc;
    doc.Parse(payload_json.c_str());

    if (doc.HasParseError() || !doc.IsObject()) {
        return -1;
    }

    if (doc.HasMember("catelog") && doc["catelog"].IsString())
        meta->catelog = doc["catelog"].GetString();
    if (doc.HasMember("name") && doc["name"].IsString())
        meta->name = doc["name"].GetString();
    if (doc.HasMember("description") && doc["description"].IsString())
        meta->description = doc["description"].GetString();
    if (doc.HasMember("position") && doc["position"].IsString()) {
        std::string pos = doc["position"].GetString();
        meta->position = (pos == "STORAGE") ? OperatorPosition::STORAGE : OperatorPosition::DATA;
    }

    if (meta->catelog.empty() || meta->name.empty()) {
        return -1;
    }

    return 0;
}

}  // namespace bridge
}  // namespace flowsql
