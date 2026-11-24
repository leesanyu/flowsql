// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/imetadata_description.h>
#include <common/iquerier.hpp>
#include <common/semantic_context.hpp>
#include <functional>

namespace flowsql::semantics {
// Owned capture; no per-row metadata reads. Read is an in-process IID or configured HTTP control call.
class Capture {
 public:
    using Read = std::function<int(const std::string&, std::string*)>;
    Capture(dataset::Json target, Read read) : target_(std::move(target)), read_(std::move(read)) {}
    void Begin();
    void Finish(bool same_object = true);
    dataset::Json Context(const dataset::Value& subject, const std::vector<std::string>& names,
                          const std::vector<SemanticProjection>& projections, const std::string& scope) const;

 private:
    bool ReadDescription(dataset::Json* output);
    dataset::Json target_, observed_;
    Read read_;
    DataSemanticsError error_;
    bool changed_ = false;
};
inline Capture::Read LocalDescription(IQuerier* querier) {
    auto* service = querier ? static_cast<IMetadataDescription*>(querier->First(IID_METADATA_DESCRIPTION)) : nullptr;
    return [service](const std::string& request, std::string* response) {
        if (!service) {
            *response = R"({"error":{"code":"SOURCE_UNAVAILABLE","message":"description service unavailable"}})";
            return -1;
        }
        return service->Describe(request.c_str(), response);
    };
}
inline bool Capture::ReadDescription(dataset::Json* output) {
    using namespace dataset;
    Json request(rapidjson::kObjectType), response;
    request.AddMember("target", Copy(target_, request.GetAllocator()), request.GetAllocator());
    std::string text;
    const int rc = read_(Encode(request), &text);
    if (text.size() > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64)) {
        error_ = {"LIMIT_EXCEEDED", "description exceeds result budget", ""};
        return false;
    }
    response.Parse(text.c_str());
    if (!response.IsObject() || rc || !response.HasMember("data")) {
        error_ = {"SOURCE_UNAVAILABLE", "invalid description response", ""};
        if (response.IsObject() && response.HasMember("error"))
            error_ = {Get(response["error"], "code", "SOURCE_UNAVAILABLE"), Get(response["error"], "message"), ""};
        return false;
    }
    output->CopyFrom(response["data"], output->GetAllocator());
    return true;
}
inline void Capture::Begin() {
    if (target_.IsObject()) ReadDescription(&observed_);
}
inline void Capture::Finish(bool same_object) {
    using namespace dataset;
    if (!observed_.IsObject()) return;
    Json after;
    if (!ReadDescription(&after)) return;
    changed_ = !same_object || observed_["basis"]["object_key"] != after["basis"]["object_key"];
    if (!changed_ && Get(observed_["structure"], "state") == "pending") {
        observed_ = std::move(after);
        return;
    }
    changed_ = changed_ || observed_["basis"]["source_revision"] != after["basis"]["source_revision"] ||
               observed_["basis"]["schema_fingerprint"] != after["basis"]["schema_fingerprint"];
}
inline dataset::Json Capture::Context(const dataset::Value& subject, const std::vector<std::string>& names,
                                      const std::vector<SemanticProjection>& projections,
                                      const std::string& scope) const {
    auto context =
        ResultContext(subject, observed_, names, projections, scope, changed_, error_.code.empty() ? nullptr : &error_);
    if (dataset::Encode(context).size() > 12 * 1024 * 1024) {
        const DataSemanticsError limit{"LIMIT_EXCEEDED", "description exceeds result budget", ""};
        return ResultContext(subject, dataset::Value(), names, {}, scope, false, &limit);
    }
    return context;
}
}  // namespace flowsql::semantics
