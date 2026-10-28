// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/ichannel_dataset_descriptor.h>
#include <functional>
#include "data_semantics_registry.h"

namespace flowsql::channels::config {
// The callback resolves a local IID or an HTTP metadata route. Never called under the registry write lock.
class ChannelDictionaryService {
 public:
    using Describe =
        std::function<int(const ChannelDatasetRequestV1&, ChannelDatasetSnapshotV1*, DataSemanticsErrorV1*)>;
    ChannelDictionaryService(DataSemanticsRegistry& registry, Describe describe)
        : registry_(registry), describe_(std::move(describe)) {}
    int32_t Control(const std::string& operation, const std::string& request, std::string& response);

 private:
    DataSemanticsRegistry& registry_;
    Describe describe_;
};
}  // namespace flowsql::channels::config
