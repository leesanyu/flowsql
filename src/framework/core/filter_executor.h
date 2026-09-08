// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_FILTER_EXECUTOR_H_
#define _FLOWSQL_FRAMEWORK_CORE_FILTER_EXECUTOR_H_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>

#include "filter_binding.h"

namespace flowsql {

enum class FilterEvalError : int32_t {
    kNone = 0,
    kInvalidArgument,
    kInvalidBoundExpression,
    kSchemaMismatch,
    kArrowError,
    kUnsupportedFunction,
};

/** Evaluate a bound predicate into one nullable boolean value per input row. */
FilterEvalError EvaluateFilterMask(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::shared_ptr<arrow::BooleanArray>* output,
    std::string* error);

/** Keep only rows whose evaluated predicate is true; false and null are dropped. */
FilterEvalError FilterRecordBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::shared_ptr<arrow::RecordBatch>* output,
    std::string* error);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FILTER_EXECUTOR_H_
