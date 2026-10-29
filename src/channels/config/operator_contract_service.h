// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include "data_semantics_registry.h"

namespace flowsql::channels::config {
class OperatorContractService : public IOperatorDataContractV1 {
 public:
    explicit OperatorContractService(DataSemanticsRegistry& registry, IConfigChannelRegistryV1* configs = nullptr)
        : registry_(registry), configs_(configs) {}
    int Resolve(const char*, OperatorContractSnapshotV1*, DataSemanticsErrorV1*) override;
    int CheckAndDerive(const char*, std::string*, DataSemanticsErrorV1*) override;
    int RecordUse(const char*, OperatorContractSnapshotV1*, DataSemanticsErrorV1*) override;

 private:
    DataSemanticsRegistry& registry_;
    IConfigChannelRegistryV1* configs_;
};
}  // namespace flowsql::channels::config
