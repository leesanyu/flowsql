// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/irouter_handle.h>
#include <common/channel_dataset.hpp>
#include <common/iquerier.hpp>

namespace flowsql::contracts {
// Invoke the existing in-process control capability, outside any caller-owned lock.
// Cross-process callers continue to use the same HTTP JSON protocol.
inline int Control(IQuerier* querier, const std::string& operation, const rapidjson::Value& request,
                   rapidjson::Document* response, DataSemanticsErrorV1* error) {
    if (error) *error = {};
    if (!response) return dataset::Fail(error, "INVALID_REQUEST", "control output required");
    response->SetNull();
    fnRouterHandler handler;
    const auto path = "/semantics/v1/" + operation;
    if (querier)
        querier->Traverse(IID_ROUTER_HANDLE, [&](void* ptr) {
            static_cast<IRouterHandle*>(ptr)->EnumRoutes([&](const RouteItem& route) {
                if (route.uri == path && route.method == "POST") handler = route.handler;
            });
            return handler ? -1 : 0;
        });
    if (!handler) return dataset::Fail(error, "UNAVAILABLE", "semantics control provider unavailable");
    std::string text;
    const int rc = handler(path, dataset::Encode(request), text);
    if (text.size() > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64))
        return dataset::Fail(error, "LIMIT_EXCEEDED", "semantics response exceeds control limits");
    response->Parse(text.c_str());
    if (response->HasParseError() || !response->IsObject()) {
        response->SetNull();
        return dataset::Fail(error, "UNAVAILABLE", "invalid semantics control response");
    }
    if (rc || response->HasMember("error")) {
        if (response->HasMember("error")) {
            const auto& e = (*response)["error"];
            dataset::Fail(error, dataset::Get(e, "code", "UNAVAILABLE"), dataset::Get(e, "message"),
                          dataset::Get(e, "field_path"));
        } else
            dataset::Fail(error, "UNAVAILABLE", "semantics control failed");
        response->SetNull();
        return -1;
    }
    if (!response->HasMember("data") || !(*response)["data"].IsObject()) {
        response->SetNull();
        return dataset::Fail(error, "UNAVAILABLE", "missing semantics response data");
    }
    return 0;
}
}  // namespace flowsql::contracts
