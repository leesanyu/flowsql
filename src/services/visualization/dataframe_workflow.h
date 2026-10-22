// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/api.h>
#include <framework/interfaces/idatabase_exploration.h>
#include <rapidjson/document.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "numeric_display.h"
#include "workflow.h"

namespace flowsql::visualization {

struct DataFrameResultV1 {
    std::vector<FieldV1> fields;
    std::vector<std::vector<ExplorationValueV1>> rows;
    std::vector<NumericDisplayV1> displays;
    std::string scope = "complete";
};

// Browser-facing field identity shared by discovery and compute; unknown Arrow types stay unsupported.
std::vector<FieldV1> DescribeArrowSchema(const arrow::Schema& schema);
std::string ArrowSchemaRevision(const arrow::Schema& schema);

// Compiles and runs the Graphic Walker workflow against one immutable snapshot batch.
// Throws QueryFailure with INVALID_WORKFLOW / UNSUPPORTED_OPERATION / UNSUPPORTED_TYPE / RESULT_LIMIT / TIMEOUT.
DataFrameResultV1 ExecuteDataFrameWorkflow(const std::shared_ptr<arrow::RecordBatch>& batch,
                                           const std::vector<FieldV1>& fields, const rapidjson::Value& payload,
                                           const std::string& detail_mode, uint32_t max_rows, uint64_t max_bytes,
                                           const std::chrono::steady_clock::time_point& deadline);

}  // namespace flowsql::visualization
