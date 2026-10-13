// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_

#include "npm_basic_result_encoder.h"
#include "npm_result_router.h"
#include "npm_session_result_encoder.h"

#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/core/npm_packet_processor.h>
#include <operators/npm_basic/modules/basic/npm_basic_result_projector.h>
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
    kRouterError,
};

struct NpmBasicDrainStatus {
    NpmBasicDrainError error = NpmBasicDrainError::kNone;
    int64_t event_index = -1;
    int router_error = 0;
    NpmBasicProjectionError projection_error = NpmBasicProjectionError::kNone;
    NpmBasicEncodeError encode_error = NpmBasicEncodeError::kNone;
    NpmSessionEncodeError session_encode_error = NpmSessionEncodeError::kNone;
};

/** Owns accepted typed rows until bounded, synchronous delivery through the task's router. */
class NpmBasicResultCollector final : public INpmResultWriter, public INpmAcceptedResultDelivery {
 public:
    NpmBasicResultCollector(NpmBasicFeatureConfig features, std::shared_ptr<INpmTaskBudget> budget,
                            std::shared_ptr<NpmResultRouter> router = {});
    ~NpmBasicResultCollector() override;
    NpmBasicResultCollector(const NpmBasicResultCollector&) = delete;
    NpmBasicResultCollector& operator=(const NpmBasicResultCollector&) = delete;
    NpmBasicResultCollector(NpmBasicResultCollector&&) = delete;
    NpmBasicResultCollector& operator=(NpmBasicResultCollector&&) = delete;

    NpmBasicDrainStatus RouteEnds(const NpmSessionSnapshotBatch& events, NpmBasicResultProjector& projector);
    void UsePeriodicBasic() { periodic_basic_ = true; }
    int Finish();
    int FlushAccepted() override;
    size_t pending_typed_rows() const noexcept override;
    void DiscardAccepted() noexcept override;
    void Abort() noexcept;
    uint64_t typed_storage_bytes() const noexcept;
    int WriteBasic(const NpmBasicResult4& result) override;
    int WriteBasic(const NpmBasicResult6& result) override;
    int WriteSession(const NpmSessionResult4& result) override;
    int WriteSession(const NpmSessionResult6& result) override;

    /** Delivery failure is terminal; caller output is unchanged and accepted storage is discarded. */
    NpmBasicDrainStatus Drain(const NpmSessionSnapshotBatch& events, NpmBasicResultProjector& projector,
                              std::shared_ptr<arrow::RecordBatch>* output);

    size_t pending_results() const noexcept;

 private:
    struct TypedStorage;
    template <typename Result>
    int Accept(const Result&, std::vector<Result> TypedStorage::*, std::string_view entity);
    int Fail(int code, std::string message);
    bool RequiresEncoding(std::string_view entity) const;
    NpmBasicFeatureConfig features_;
    bool periodic_basic_ = false;
    bool finished_ = false;
    std::shared_ptr<NpmResultRouter> router_;
    std::unique_ptr<TypedStorage> typed_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_RESULT_COLLECTOR_H_
