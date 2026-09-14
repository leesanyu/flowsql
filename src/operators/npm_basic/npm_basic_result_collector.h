// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_

#include "npm_basic_result_encoder.h"
#include "npm_basic_result_projector.h"
#include "npm_packet_processor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace flowsql::npm {

enum class NpmBasicDrainError : uint8_t {
    kNone = 0,
    kNullOutput,
    kNullBudget,
    kProjectionError,
    kEncodeError,
    kAllocationFailed,
};

struct NpmBasicDrainStatus {
    NpmBasicDrainError error = NpmBasicDrainError::kNone;
    int64_t event_index = -1;
    NpmBasicProjectionError projection_error = NpmBasicProjectionError::kNone;
    NpmBasicEncodeError encode_error = NpmBasicEncodeError::kNone;
};

/** Task-private writer: copies call-borrowed module records, then drains them with owned end events. */
class NpmBasicResultCollector final : public INpmResultWriter {
 public:
    NpmBasicResultCollector() = default;
    NpmBasicResultCollector(const NpmBasicResultCollector&) = delete;
    NpmBasicResultCollector& operator=(const NpmBasicResultCollector&) = delete;
    NpmBasicResultCollector(NpmBasicResultCollector&&) = delete;
    NpmBasicResultCollector& operator=(NpmBasicResultCollector&&) = delete;

    int WriteBasic(const NpmBasicResult& result) override;

    /** Success consumes pending module records; failure leaves pending records and caller output unchanged. */
    NpmBasicDrainStatus Drain(const std::vector<NpmSessionEndEvent>& events,
                              NpmBasicResultProjector& projector,
                              const std::shared_ptr<INpmTaskBudget>& budget,
                              std::shared_ptr<arrow::RecordBatch>* output);

    size_t pending_results() const noexcept { return pending_.size(); }

 private:
    std::vector<NpmBasicResult> pending_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_
