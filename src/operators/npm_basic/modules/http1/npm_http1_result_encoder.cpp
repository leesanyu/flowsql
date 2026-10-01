// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_http1_result_encoder.h"

#include <arrow/api.h>
#include <arrow/util/utf8.h>

#include <array>
#include <cerrno>
#include <new>

namespace flowsql::npm {
namespace {

template <typename Builder, typename Value>
bool Append(Builder& builder, const Value& value) {
    return builder.Append(value).ok();
}

template <typename Builder, typename Value>
bool Append(Builder& builder, const std::optional<Value>& value) {
    return value ? builder.Append(*value).ok() : builder.AppendNull().ok();
}

std::optional<uint8_t> DirectionValue(std::optional<NpmPacketDirection> direction) {
    if (!direction) return std::nullopt;
    return static_cast<uint8_t>(*direction);
}

bool ValidDirection(std::optional<NpmPacketDirection> direction) {
    return !direction || *direction == NpmPacketDirection::kAToB || *direction == NpmPacketDirection::kBToA;
}

bool ValidText(const std::optional<std::string>* value, size_t max_bytes) {
    return !value || !*value || (value->value().size() <= max_bytes && arrow::util::ValidateUTF8(value->value()));
}

bool ValidRow(const NpmHttp1TransactionResultV1& row, const NpmHttp1SessionIdentityV1& session) {
    if (row.entity_instance_id == 0 || row.session_id == 0 || row.session_id != session.session_id ||
        session.a_ip.empty() || session.b_ip.empty() || session.a_ip.size() > 45 || session.b_ip.size() > 45 ||
        !arrow::util::ValidateUTF8(session.a_ip) || !arrow::util::ValidateUTF8(session.b_ip) ||
        !ValidDirection(row.request_direction) || !ValidDirection(row.response_direction) ||
        !ValidText(row.request ? &row.request->method : nullptr, 1024) ||
        !ValidText(row.request ? &row.request->target : nullptr, 4 * 1048576) ||
        !ValidText(row.request ? &row.request->host : nullptr, 4 * 1048576))
        return false;
    if (row.request &&
        (row.request->is_response || !row.request->method || !row.request->target || row.request->status_code))
        return false;
    if (row.response && (!row.response->is_response || !row.response->status_code || *row.response->status_code < 100 ||
                         *row.response->status_code > 599))
        return false;
    switch (row.outcome) {
        case NpmHttp1OutcomeV1::kMatched:
            return row.request && row.response && row.request_direction && row.response_direction &&
                   *row.request_direction != *row.response_direction &&
                   row.incomplete_reason == NpmHttp1IncompleteReasonV1::kNone;
        case NpmHttp1OutcomeV1::kRequestOnly:
            return row.request && !row.response && row.request_direction && !row.response_direction &&
                   row.incomplete_reason != NpmHttp1IncompleteReasonV1::kNone &&
                   row.incomplete_reason != NpmHttp1IncompleteReasonV1::kNoPendingRequest && !row.latency_ns;
        case NpmHttp1OutcomeV1::kResponseOnly:
            return !row.request && row.response && !row.request_direction && row.response_direction &&
                   row.incomplete_reason == NpmHttp1IncompleteReasonV1::kNoPendingRequest && !row.latency_ns;
    }
    return false;
}

class BuildCharge final {
 public:
    BuildCharge(const std::shared_ptr<INpmTaskBudget>& budget, uint64_t bytes) : budget_(budget), bytes_(bytes) {}
    bool Reserve() {
        reserved_ = budget_->Reserve(NpmBudgetCategory::kModuleState, bytes_) == NpmBudgetError::kNone;
        return reserved_;
    }
    ~BuildCharge() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kModuleState, bytes_);
    }

 private:
    const std::shared_ptr<INpmTaskBudget>& budget_;
    uint64_t bytes_;
    bool reserved_ = false;
};

}  // namespace

int EmitNpmHttp1TransactionV1(const NpmHttp1TransactionResultV1& result, const NpmHttp1SessionIdentityV1& session,
                              const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                              arrow::MemoryPool* pool) {
    if (!budget || !ValidRow(result, session)) return EINVAL;
    const uint64_t text_bytes = result.request ? result.request->method->size() + result.request->target->size() +
                                                     (result.request->host ? result.request->host->size() : 0)
                                               : 0;
    BuildCharge charge(budget, 16 * 1024 + 2 * text_bytes);
    if (!charge.Reserve()) return ENOSPC;
    try {
        if (!pool) pool = arrow::default_memory_pool();
        arrow::UInt64Builder entity_id(pool), revision(pool), session_id(pool), domain_id(pool);
        arrow::Int64Builder observed_at(pool), request_at(pool), response_at(pool), latency(pool);
        arrow::BooleanBuilder is_final(pool);
        arrow::StringBuilder a_ip(pool), b_ip(pool), outcome(pool), method(pool), target(pool), host(pool),
            reason(pool);
        arrow::UInt16Builder a_port(pool), b_port(pool), status_code(pool);
        arrow::UInt32Builder informational_count(pool);
        arrow::UInt8Builder request_direction(pool), response_direction(pool);
        const std::optional<std::string> method_value = result.request ? result.request->method : std::nullopt;
        const std::optional<std::string> target_value = result.request ? result.request->target : std::nullopt;
        const std::optional<std::string> host_value = result.request ? result.request->host : std::nullopt;
        const std::optional<uint16_t> status_value = result.response ? result.response->status_code : std::nullopt;
        const std::optional<int64_t> request_time = result.request ? result.request->complete_at_ns : std::nullopt;
        const std::optional<int64_t> response_time = result.response ? result.response->complete_at_ns : std::nullopt;
        const char* outcome_name = NpmHttp1OutcomeNameV1(result.outcome);
        const char* reason_name = NpmHttp1IncompleteReasonNameV1(result.incomplete_reason);
        if (!outcome_name || (result.incomplete_reason != NpmHttp1IncompleteReasonV1::kNone && !reason_name))
            return EINVAL;
        const std::optional<std::string_view> reason_value =
            reason_name ? std::optional<std::string_view>(reason_name) : std::nullopt;
        if (!Append(entity_id, result.entity_instance_id) || !Append(revision, uint64_t{1}) ||
            !Append(observed_at, result.observed_at_ns) || !Append(is_final, true) ||
            !Append(session_id, session.session_id) || !Append(domain_id, session.observation_domain_id) ||
            !Append(a_ip, session.a_ip) || !Append(b_ip, session.b_ip) || !Append(a_port, session.a_port) ||
            !Append(b_port, session.b_port) || !Append(outcome, std::string_view(outcome_name)) ||
            !Append(informational_count, result.informational_count) ||
            !Append(request_direction, DirectionValue(result.request_direction)) ||
            !Append(response_direction, DirectionValue(result.response_direction)) || !Append(method, method_value) ||
            !Append(target, target_value) || !Append(host, host_value) || !Append(status_code, status_value) ||
            !Append(request_at, request_time) || !Append(response_at, response_time) ||
            !Append(latency, result.latency_ns) || !Append(reason, reason_value))
            return ENOMEM;
        const std::array<arrow::ArrayBuilder*, 22> builders = {&entity_id,
                                                               &revision,
                                                               &observed_at,
                                                               &is_final,
                                                               &session_id,
                                                               &domain_id,
                                                               &a_ip,
                                                               &b_ip,
                                                               &a_port,
                                                               &b_port,
                                                               &outcome,
                                                               &informational_count,
                                                               &request_direction,
                                                               &response_direction,
                                                               &method,
                                                               &target,
                                                               &host,
                                                               &status_code,
                                                               &request_at,
                                                               &response_at,
                                                               &latency,
                                                               &reason};
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(builders.size());
        for (auto* builder : builders) {
            std::shared_ptr<arrow::Array> column;
            const auto status = builder->Finish(&column);
            if (!status.ok()) return status.IsOutOfMemory() ? ENOMEM : EIO;
            columns.push_back(std::move(column));
        }
        auto batch = arrow::RecordBatch::Make(NpmHttp1TransactionEntityDescriptorV1().schema, 1, std::move(columns));
        return emitter.Emit("http1_transaction", *batch);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

}  // namespace flowsql::npm
