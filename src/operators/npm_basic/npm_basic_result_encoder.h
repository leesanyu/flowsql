// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_ENCODER_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_ENCODER_H_

#include "npm_analysis_contract.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace flowsql::npm {

enum class NpmBasicEncodeError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidResult,
    kArrowError,
    kAllocationFailed,
    kNullBudget,
    kInvalidBufferSize,
    kBudgetError,
};

/** Encodes valid active/final results into the fixed NpmBasicResultSchema. Output is unchanged on error. */
NpmBasicEncodeError EncodeNpmBasicResults(const std::vector<NpmBasicResult>& results,
                                          std::shared_ptr<arrow::RecordBatch>* output,
                                          std::string* error = nullptr);

/** Encodes, reserves exact Arrow buffer bytes, and releases the reservation with the last output owner. */
NpmBasicEncodeError EncodeNpmBasicResultsWithBudget(
    const std::vector<NpmBasicResult>& results,
    const std::shared_ptr<INpmTaskBudget>& budget,
    std::shared_ptr<arrow::RecordBatch>* output,
    std::string* error = nullptr);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_ENCODER_H_
