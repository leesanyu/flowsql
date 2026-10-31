// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/imetadata_description.h>
#include <framework/interfaces/ioperator_contract_catalog.h>
#include "channel_dictionary.h"
#include "config_channel_provider.h"

namespace flowsql::channels::config {
class MetadataDescriptionService : public IMetadataDescriptionV1 {
 public:
    using Lookup =
        std::function<int(const std::string&, uint32_t, OperatorComponentContractV1*, DataSemanticsErrorV1*)>;
    MetadataDescriptionService(DataSemanticsRegistry& registry, ConfigChannelProvider& configs,
                               ChannelDictionaryService::Describe describe, Lookup lookup)
        : registry_(registry), configs_(configs), describe_(std::move(describe)), lookup_(std::move(lookup)) {}
    int Describe(const char*, std::string*) override;
    int SaveChannel(const char*, std::string*) override;
    int SaveOperator(const char*, std::string*) override;

 private:
    DataSemanticsRegistry& registry_;
    ConfigChannelProvider& configs_;
    ChannelDictionaryService::Describe describe_;
    Lookup lookup_;
};
}  // namespace flowsql::channels::config
