// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/channel_dataset.hpp>
#include "kafka_plugin.h"
namespace flowsql::channels::kafka {
int Plugin::Describe(const ChannelDatasetRequestV1& request, ChannelDatasetSnapshotV1* out,
                     DataSemanticsErrorV1* error) {
    if (!out) return dataset::Fail(error, "INVALID_REQUEST", "output is required");
    *out = {};
    if (error) *error = {};
    try {
        auto locator = Parse(request.locator_json);
        if (Text(locator, "kind") != "kafka_source" || Text(locator, "name").empty() || !request.timeout_ms ||
            request.timeout_ms > 30000)
            return dataset::Fail(error, "INVALID_REQUEST", "a named Kafka source is required");
        auto snapshot = store_.DescribeDataset(locator);
        out->document_json = std::make_shared<const std::string>(Encode(snapshot));
        return 0;
    } catch (const Failure& e) {
        return dataset::Fail(error,
                             e.status == 404   ? "NOT_FOUND"
                             : e.status == 503 ? "SOURCE_UNAVAILABLE"
                                               : e.code,
                             "Kafka dataset metadata unavailable");
    } catch (const std::exception&) {
        return dataset::Fail(error, "INCOMPLETE_STRUCTURE", "invalid source schema");
    }
}
}  // namespace flowsql::channels::kafka
