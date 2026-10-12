// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_collector.h"
#include <arrow/api.h>
#include "npm_family_address_builder.h"

#include <cerrno>
#include <chrono>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

int64_t ResultGeneratedAtNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

NpmBasicResultCollector::NpmBasicResultCollector() : NpmBasicResultCollector(NpmBasicFeatureConfig{}) {}

NpmBasicResultCollector::NpmBasicResultCollector(NpmBasicFeatureConfig features) : features_(features) {}

template <typename Address>
int NpmBasicResultCollector::WriteBasicImpl(const NpmBasicFamilyResult<Address>& result,
                                            std::vector<NpmBasicFamilyResult<Address>>& pending) {
    if (!features_.basic_enabled) return ENOTSUP;
    if (ValidateNpmBasicResult(result) != NpmBasicResultError::kNone) return EINVAL;
    const std::string entity =
        NpmAddressArrowBuilder<Address>::family == packet::AddressFamily::kIPv4 ? "basic_ipv4" : "basic_ipv6";
    if (router_) {
        std::shared_ptr<arrow::RecordBatch> rows;
        std::string error;
        if (EncodeNpmBasicResults<Address>({result}, &rows, &error, features_.labeling_enabled, router_->pool()) !=
            NpmBasicEncodeError::kNone)
            return router_->Reject(ENOMEM, entity + ": " + error);
        return router_->Emit("basic", entity, *rows);
    }
    if (features_.observing_entity != entity) return 0;
    try {
        pending.push_back(result);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}
int NpmBasicResultCollector::WriteBasic(const NpmBasicResult4& result) {
    return WriteBasicImpl(result, pending_basic4_);
}
int NpmBasicResultCollector::WriteBasic(const NpmBasicResult6& result) {
    return WriteBasicImpl(result, pending_basic6_);
}
template <typename Address>
int NpmBasicResultCollector::WriteSessionImpl(const NpmSessionFamilyResult<Address>& result,
                                              std::vector<NpmSessionFamilyResult<Address>>& pending) {
    if (!features_.session_enabled) return ENOTSUP;
    if (ValidateNpmSessionResult(result) != NpmSessionResultError::kNone) return EINVAL;
    const std::string entity =
        NpmAddressArrowBuilder<Address>::family == packet::AddressFamily::kIPv4 ? "session_ipv4" : "session_ipv6";
    if (router_) {
        std::shared_ptr<arrow::RecordBatch> rows;
        std::string error;
        if (EncodeNpmSessionResults<Address>({result}, &rows, &error, features_.labeling_enabled, router_->pool()) !=
            NpmSessionEncodeError::kNone)
            return router_->Reject(ENOMEM, entity + ": " + error);
        return router_->Emit("session", entity, *rows);
    }
    if (features_.observing_entity != entity) return 0;
    try {
        pending.push_back(result);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}
int NpmBasicResultCollector::WriteSession(const NpmSessionResult4& result) {
    return WriteSessionImpl(result, pending_session4_);
}
int NpmBasicResultCollector::WriteSession(const NpmSessionResult6& result) {
    return WriteSessionImpl(result, pending_session6_);
}

NpmBasicDrainStatus NpmBasicResultCollector::Drain(const std::vector<NpmSessionEndEvent>& events,
                                                   NpmBasicResultProjector& projector,
                                                   const std::shared_ptr<INpmTaskBudget>& budget,
                                                   std::shared_ptr<arrow::RecordBatch>* output) {
    NpmBasicDrainStatus status;
    if (output == nullptr) {
        status.error = NpmBasicDrainError::kNullOutput;
        return status;
    }
    if (budget == nullptr) {
        status.error = NpmBasicDrainError::kNullBudget;
        return status;
    }

    if (router_) {
        status = RouteEnds(events, projector);
        if (status.error != NpmBasicDrainError::kNone) return status;
        status.router_error = router_->Drain(output);
        if (status.router_error) status.error = NpmBasicDrainError::kRouterError;
        return status;
    }
    const auto basic4_size = pending_basic4_.size(), basic6_size = pending_basic6_.size();
    const auto session4_size = pending_session4_.size(), session6_size = pending_session6_.size();
    const auto rollback = [&] {
        pending_basic4_.resize(basic4_size);
        pending_basic6_.resize(basic6_size);
        pending_session4_.resize(session4_size);
        pending_session6_.resize(session6_size);
    };
    status = RouteEnds(events, projector);
    if (status.error != NpmBasicDrainError::kNone) {
        rollback();
        return status;
    }
    const auto& entity = features_.observing_entity;
    if (entity == "basic_ipv4")
        status.encode_error =
            EncodeNpmBasicResultsWithBudget(pending_basic4_, budget, output, nullptr, features_.labeling_enabled);
    else if (entity == "basic_ipv6")
        status.encode_error =
            EncodeNpmBasicResultsWithBudget(pending_basic6_, budget, output, nullptr, features_.labeling_enabled);
    else if (entity == "session_ipv4")
        status.session_encode_error =
            EncodeNpmSessionResultsWithBudget(pending_session4_, budget, output, nullptr, features_.labeling_enabled);
    else if (entity == "session_ipv6")
        status.session_encode_error =
            EncodeNpmSessionResultsWithBudget(pending_session6_, budget, output, nullptr, features_.labeling_enabled);
    else {
        auto empty =
            arrow::RecordBatch::MakeEmpty(features_.protocol_schema ? features_.protocol_schema : arrow::schema({}));
        if (!empty.ok()) {
            status.error = NpmBasicDrainError::kEncodeError;
            rollback();
            return status;
        }
        *output = *empty;
    }
    if (status.encode_error != NpmBasicEncodeError::kNone ||
        status.session_encode_error != NpmSessionEncodeError::kNone) {
        status.error = NpmBasicDrainError::kEncodeError;
        rollback();
        return status;
    }
    pending_basic4_.clear();
    pending_basic6_.clear();
    pending_session4_.clear();
    pending_session6_.clear();
    return status;
}

NpmBasicDrainStatus NpmBasicResultCollector::RouteEnds(const std::vector<NpmSessionEndEvent>& events,
                                                       NpmBasicResultProjector& projector) {
    NpmBasicDrainStatus status;
    if (!features_.basic_enabled || periodic_basic_) return status;
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        status.projection_error = projector.ProjectAndWrite(event.snapshot.View(), ResultGeneratedAtNs(),
                                                            event.snapshot.end_reason, *this, &status.router_error);
        if (status.projection_error != NpmBasicProjectionError::kNone)
            status.error = NpmBasicDrainError::kProjectionError;
        else if (status.router_error)
            status.error = NpmBasicDrainError::kRouterError;
        if (status.error != NpmBasicDrainError::kNone) {
            status.event_index = index;
            return status;
        }
    }
    return status;
}

size_t NpmBasicResultCollector::pending_results() const noexcept {
    if (router_) return router_->pending_results();
    return pending_basic4_.size() + pending_basic6_.size() + pending_session4_.size() + pending_session6_.size();
}

}  // namespace flowsql::npm
