// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_RESULT_ENCODER_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_RESULT_ENCODER_H_

#include <operators/npm_basic/npm_analysis_contract.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace flowsql::npm {

enum class NpmSessionEncodeError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidResult,
    kArrowError,
    kAllocationFailed,
    kNullBudget,
    kInvalidBufferSize,
    kBudgetError,
};

/** Encodes valid Session results into the fixed NpmSessionResultSchema. Output is unchanged on error. */
NpmSessionEncodeError EncodeNpmSessionResults(const std::vector<NpmSessionResult>& results,
                                              std::shared_ptr<arrow::RecordBatch>* output, std::string* error = nullptr,
                                              bool labeling_enabled = false);

/** Encodes, reserves exact Arrow buffer bytes, and releases the reservation with the last output owner. */
NpmSessionEncodeError EncodeNpmSessionResultsWithBudget(const std::vector<NpmSessionResult>& results,
                                                        const std::shared_ptr<INpmTaskBudget>& budget,
                                                        std::shared_ptr<arrow::RecordBatch>* output,
                                                        std::string* error = nullptr, bool labeling_enabled = false);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_RESULT_ENCODER_H_
