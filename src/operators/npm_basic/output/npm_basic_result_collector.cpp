// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_collector.h"
#include <arrow/api.h>
#include <arrow/util/utf8.h>
#include <stdexcept>
#include <type_traits>

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

struct NpmBasicResultCollector::TypedStorage {
    uint64_t charged_bytes = sizeof(TypedStorage);
    std::vector<NpmBasicResult4> basic4;
    std::vector<NpmBasicResult6> basic6;
    std::vector<NpmSessionResult4> session4;
    std::vector<NpmSessionResult6> session6;
};

NpmBasicResultCollector::NpmBasicResultCollector(NpmBasicFeatureConfig features, std::shared_ptr<INpmTaskBudget> budget,
                                                 std::shared_ptr<NpmResultRouter> router)
    : features_(std::move(features)), router_(std::move(router)) {
    if (!budget) throw std::invalid_argument("NPM collector requires a task budget");
    if (router_) {
        if (router_->Budget() != budget) throw std::invalid_argument("NPM collector/router budget mismatch");
        return;
    }
    std::vector<NpmEntityDescriptorV1> entities;
    for (auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
        if (features_.basic_enabled) entities.push_back(NpmBasicEntityDescriptorV1(family));
        if (features_.session_enabled) entities.push_back(NpmSessionEntityDescriptorV1(family));
    }
    router_ = std::make_shared<NpmResultRouter>(std::move(entities), features_.observing_entity,
                                                MakeNpmResultContext("collector"), std::move(budget), nullptr,
                                                [](const std::function<int()>& callback) { return callback(); });
}

NpmBasicResultCollector::~NpmBasicResultCollector() { DiscardAccepted(); }

bool NpmBasicResultCollector::RequiresEncoding(std::string_view entity) const {
    for (const auto& entry : router_->DeliveryPlan().entities)
        if (entry.descriptor.entity_id == entity) return entry.RequiresEncoding();
    return false;
}

int NpmBasicResultCollector::Fail(int code, std::string message) {
    const int error = router_->Reject(code, std::move(message));
    DiscardAccepted();
    router_->Discard();
    return error;
}

template <typename Result>
int NpmBasicResultCollector::Accept(const Result& result, std::vector<Result> TypedStorage::*member,
                                    std::string_view entity) {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!RequiresEncoding(entity)) return 0;
    const auto& plan = router_->DeliveryPlan();
    const uint64_t string_bytes = result.protocol ? std::max(result.protocol->size(), std::string().capacity()) + 1 : 0;
    if (typed_) {
        const auto& rows = typed_.get()->*member;
        const size_t capacity =
            rows.size() == rows.capacity() ? std::max<size_t>(1, rows.capacity() * 2) : rows.capacity();
        const uint64_t next_bytes =
            typed_->charged_bytes + (capacity - rows.capacity()) * sizeof(Result) + string_bytes;
        if (rows.size() >= plan.entity_row_limit || next_bytes > plan.typed_storage_soft_limit) {
            if (const int rc = FlushAccepted()) return rc;
        }
    }
    const auto& budget = router_->Budget();
    try {
        if (!typed_) {
            if (budget->Reserve(NpmBudgetCategory::kPendingOutput, sizeof(TypedStorage)) != NpmBudgetError::kNone)
                return Fail(ENOMEM, "NPM typed owner exceeds pending output budget");
            try {
                typed_ = std::make_unique<TypedStorage>();
            } catch (...) {
                budget->Release(NpmBudgetCategory::kPendingOutput, sizeof(TypedStorage));
                throw;
            }
        }
        auto& rows = typed_.get()->*member;
        if (rows.size() == rows.capacity()) {
            const size_t old_capacity = rows.capacity();
            const size_t capacity = std::min(plan.entity_row_limit, std::max<size_t>(1, old_capacity * 2));
            const uint64_t bytes = capacity * sizeof(Result);
            // Vector growth temporarily owns both arrays. Charge the full replacement before allocating.
            if (budget->Reserve(NpmBudgetCategory::kPendingOutput, bytes) != NpmBudgetError::kNone)
                return Fail(ENOMEM, "NPM typed array exceeds pending output budget");
            try {
                rows.reserve(capacity);
            } catch (...) {
                budget->Release(NpmBudgetCategory::kPendingOutput, bytes);
                throw;
            }
            const uint64_t actual = rows.capacity() * sizeof(Result);
            if (actual > bytes &&
                budget->Reserve(NpmBudgetCategory::kPendingOutput, actual - bytes) != NpmBudgetError::kNone) {
                // The standard library may reserve more than requested. Free that array before refunding.
                std::vector<Result>().swap(rows);
                budget->Release(NpmBudgetCategory::kPendingOutput, bytes);
                return Fail(ENOMEM, "NPM typed capacity exceeds pending output budget");
            }
            budget->Release(NpmBudgetCategory::kPendingOutput, old_capacity * sizeof(Result));
            typed_->charged_bytes += actual - old_capacity * sizeof(Result);
        }
        if (budget->Reserve(NpmBudgetCategory::kPendingOutput, string_bytes) != NpmBudgetError::kNone)
            return Fail(ENOMEM, "NPM typed string exceeds pending output budget");
        try {
            rows.push_back(result);
        } catch (...) {
            budget->Release(NpmBudgetCategory::kPendingOutput, string_bytes);
            throw;
        }
        const uint64_t actual = rows.back().protocol ? rows.back().protocol->capacity() + 1 : 0;
        if (actual > string_bytes &&
            budget->Reserve(NpmBudgetCategory::kPendingOutput, actual - string_bytes) != NpmBudgetError::kNone) {
            rows.pop_back();
            budget->Release(NpmBudgetCategory::kPendingOutput, string_bytes);
            return Fail(ENOMEM, "NPM typed string capacity exceeds pending output budget");
        }
        if (actual < string_bytes) budget->Release(NpmBudgetCategory::kPendingOutput, string_bytes - actual);
        typed_->charged_bytes += actual;
        // An oversize single row is owned only for this immediate encode/route operation.
        return typed_->charged_bytes > plan.typed_storage_soft_limit ? FlushAccepted() : 0;
    } catch (const std::bad_alloc&) {
        return Fail(ENOMEM, "NPM typed result allocation failed");
    }
}

int NpmBasicResultCollector::WriteBasic(const NpmBasicResult4& result) {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!features_.basic_enabled) return ENOTSUP;
    if (ValidateNpmBasicResult(result) != NpmBasicResultError::kNone ||
        (result.protocol && !arrow::util::ValidateUTF8(*result.protocol)))
        return EINVAL;
    return Accept(result, &TypedStorage::basic4, "basic_ipv4");
}
int NpmBasicResultCollector::WriteBasic(const NpmBasicResult6& result) {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!features_.basic_enabled) return ENOTSUP;
    if (ValidateNpmBasicResult(result) != NpmBasicResultError::kNone ||
        (result.protocol && !arrow::util::ValidateUTF8(*result.protocol)))
        return EINVAL;
    return Accept(result, &TypedStorage::basic6, "basic_ipv6");
}
int NpmBasicResultCollector::WriteSession(const NpmSessionResult4& result) {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!features_.session_enabled) return ENOTSUP;
    if (ValidateNpmSessionResult(result) != NpmSessionResultError::kNone ||
        (result.protocol && !arrow::util::ValidateUTF8(*result.protocol)))
        return EINVAL;
    return Accept(result, &TypedStorage::session4, "session_ipv4");
}
int NpmBasicResultCollector::WriteSession(const NpmSessionResult6& result) {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!features_.session_enabled) return ENOTSUP;
    if (ValidateNpmSessionResult(result) != NpmSessionResultError::kNone ||
        (result.protocol && !arrow::util::ValidateUTF8(*result.protocol)))
        return EINVAL;
    return Accept(result, &TypedStorage::session6, "session_ipv6");
}

int NpmBasicResultCollector::FlushAccepted() {
    if (const int rc = router_->Status()) return Fail(rc, "NPM result delivery is terminal");
    if (!typed_) return 0;
    try {
        const auto flush = [&](auto& rows, std::string_view module, std::string_view entity, auto encode) {
            if (rows.empty()) return 0;
            std::shared_ptr<arrow::RecordBatch> batch;
            std::string error;
            const auto encoded = encode(rows, &batch, &error, features_.labeling_enabled, router_->pool());
            if (encoded != decltype(encoded)::kNone) return Fail(ENOMEM, std::string(entity) + ": " + error);
            const int rc = router_->Emit(module, entity, *batch);
            if (rc) return Fail(rc, "NPM accepted result delivery failed");
            using Result = typename std::decay_t<decltype(rows)>::value_type;
            uint64_t bytes = rows.capacity() * sizeof(Result);
            for (const auto& row : rows)
                if (row.protocol) bytes += row.protocol->capacity() + 1;
            std::vector<Result>().swap(rows);
            typed_->charged_bytes -= bytes;
            router_->Budget()->Release(NpmBudgetCategory::kPendingOutput, bytes);
            return 0;
        };
        int rc = flush(typed_->basic4, "basic", "basic_ipv4", EncodeNpmBasicResults<network::IPv4Address>);
        if (!rc) rc = flush(typed_->basic6, "basic", "basic_ipv6", EncodeNpmBasicResults<network::IPv6Address>);
        if (!rc) rc = flush(typed_->session4, "session", "session_ipv4", EncodeNpmSessionResults<network::IPv4Address>);
        if (!rc) rc = flush(typed_->session6, "session", "session_ipv6", EncodeNpmSessionResults<network::IPv6Address>);
        if (rc) return rc;
        DiscardAccepted();
        return 0;
    } catch (const std::bad_alloc&) {
        return Fail(ENOMEM, "NPM batch result delivery allocation failed");
    }
}

void NpmBasicResultCollector::Abort() noexcept {
    router_->Cancel();
    DiscardAccepted();
    router_->Discard();
}

void NpmBasicResultCollector::DiscardAccepted() noexcept {
    if (!typed_) return;
    const uint64_t bytes = typed_->charged_bytes;
    typed_.reset();
    router_->Budget()->Release(NpmBudgetCategory::kPendingOutput, bytes);
}
uint64_t NpmBasicResultCollector::typed_storage_bytes() const noexcept { return typed_ ? typed_->charged_bytes : 0; }
size_t NpmBasicResultCollector::pending_typed_rows() const noexcept {
    return typed_ ? typed_->basic4.size() + typed_->basic6.size() + typed_->session4.size() + typed_->session6.size()
                  : 0;
}
int NpmBasicResultCollector::Finish() {
    if (finished_) return 0;
    if (const int rc = FlushAccepted()) return rc;
    const int rc = router_->Finish();
    if (rc) return Fail(rc, "NPM result Finish failed");
    finished_ = true;
    return 0;
}

NpmBasicDrainStatus NpmBasicResultCollector::Drain(const NpmSessionSnapshotBatch& events,
                                                   NpmBasicResultProjector& projector,
                                                   std::shared_ptr<arrow::RecordBatch>* output) {
    NpmBasicDrainStatus status;
    if (!output) {
        status.error = NpmBasicDrainError::kNullOutput;
        return status;
    }
    status.router_error = router_->Status();
    if (status.router_error) {
        status.error = NpmBasicDrainError::kRouterError;
        DiscardAccepted();
        router_->Discard();
        return status;
    }
    try {
        status = RouteEnds(events, projector);
        if (status.error != NpmBasicDrainError::kNone) return status;
        status.router_error = FlushAccepted();
        if (!status.router_error) status.router_error = router_->Drain(output);
        if (status.router_error) {
            status.error = NpmBasicDrainError::kRouterError;
            Fail(status.router_error, "NPM result drain failed");
        }
    } catch (const std::bad_alloc&) {
        status.error = NpmBasicDrainError::kRouterError;
        status.router_error = Fail(ENOMEM, "NPM accepted result drain allocation failed");
    }
    return status;
}

NpmBasicDrainStatus NpmBasicResultCollector::RouteEnds(const NpmSessionSnapshotBatch& events,
                                                       NpmBasicResultProjector& projector) {
    NpmBasicDrainStatus status;
    if (!features_.basic_enabled || periodic_basic_) return status;
    for (size_t index = 0; index < events.size(); ++index) {
        status.projection_error = projector.ProjectAndWrite(
            events.View(index), ResultGeneratedAtNs(), events.Row(index).facts.end_reason, *this, &status.router_error);
        if (status.projection_error != NpmBasicProjectionError::kNone)
            status.error = NpmBasicDrainError::kProjectionError;
        else if (status.router_error)
            status.error = NpmBasicDrainError::kRouterError;
        if (status.error != NpmBasicDrainError::kNone) {
            status.event_index = index;
            Fail(status.router_error ? status.router_error : EINVAL, "NPM final result projection failed");
            return status;
        }
    }
    return status;
}

size_t NpmBasicResultCollector::pending_results() const noexcept {
    return pending_typed_rows() + router_->pending_results();
}

}  // namespace flowsql::npm
