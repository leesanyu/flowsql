// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/iplugin.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <framework/interfaces/iblock_transform_execution_policy.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/iblock_transform_result_output.h>
#include <framework/interfaces/iblock_transform_source_config.h>
#include "evaluation.h"
namespace flowsql::baseliner {
class BaselinerOperator final : public IPlugin,
                                public IBlockTransformOperatorV2,
                                public IBlockTransformExecutionPolicyProviderV1,
                                public IBlockTransformSourceConfigProviderV1,
                                public IBlockTransformDataFrameInputProviderV1,
                                public IBlockTransformResultOutputProviderV1 {
 public:
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;
    std::string Category() const override { return "explore"; }
    std::string Name() const override { return "baseliner"; }
    std::string Description() const override { return "Configured baseline evaluation and forecast"; }
    bool SupportsDataFrameInput() const override { return true; }
    bool SupportsResultOutput() const override { return true; }
    int CreateTask(const BlockTransformTaskConfigV2& config, IBlockTransformTaskV2** task) override;
    void ReleaseTask(IBlockTransformTaskV2* task) override;
    int RequiresAsyncExecution(const char* with_json, const char* exact_source, bool* output) const override;
    int NormalizeSourceConfig(const char* with_json, const char* exact_source, std::string* output,
                              std::string* error) const override;

 private:
    IQuerier* querier_ = nullptr;
    bool started_ = false;
};
}  // namespace flowsql::baseliner
