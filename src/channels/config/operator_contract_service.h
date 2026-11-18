// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include "data_semantics_registry.h"

namespace flowsql::channels::config {
class OperatorContractService : public IOperatorDataContract {
 public:
    explicit OperatorContractService(DataSemanticsRegistry& registry, IConfigChannelRegistry* configs = nullptr)
        : registry_(registry), configs_(configs) {}
    int Resolve(const char*, OperatorContractSnapshot*, DataSemanticsError*) override;
    int CheckAndDerive(const char*, std::string*, DataSemanticsError*) override;
    int RecordUse(const char*, OperatorContractSnapshot*, DataSemanticsError*) override;

 private:
    DataSemanticsRegistry& registry_;
    IConfigChannelRegistry* configs_;
};
}  // namespace flowsql::channels::config
