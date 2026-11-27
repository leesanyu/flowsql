// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/api.h>
#include <framework/interfaces/idatabase_exploration.h>
#include <rapidjson/document.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "numeric_display.h"
#include "workflow.h"

namespace flowsql::visualization {

struct DataFrameResult {
    std::vector<Field> fields;
    std::vector<std::vector<ExplorationValue>> rows;
    std::vector<NumericDisplay> displays;
    std::string scope = "complete";
};

// Browser-facing field identity shared by discovery and compute; unknown Arrow types stay unsupported.
std::vector<Field> DescribeArrowSchema(const arrow::Schema& schema);
std::string ArrowSchemaRevision(const arrow::Schema& schema);

// Compiles and runs the Graphic Walker workflow against one immutable snapshot batch.
// Throws QueryFailure with INVALID_WORKFLOW / UNSUPPORTED_OPERATION / UNSUPPORTED_TYPE / RESULT_LIMIT / TIMEOUT.
DataFrameResult ExecuteDataFrameWorkflow(const std::shared_ptr<arrow::RecordBatch>& batch,
                                         const std::vector<Field>& fields, const rapidjson::Value& payload,
                                         const std::string& detail_mode, uint32_t max_rows, uint64_t max_bytes,
                                         const std::chrono::steady_clock::time_point& deadline,
                                         const std::atomic_bool* cancelled);

}  // namespace flowsql::visualization
