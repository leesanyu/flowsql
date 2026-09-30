// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_result_encoder.h"

#include <arrow/api.h>
#include <arrow/util/utf8.h>

#include <array>
#include <cerrno>
#include <new>

namespace flowsql::npm {
namespace {

constexpr uint64_t kSingleRowBuildCharge = 16 * 1024;

class BuildCharge final {
 public:
    explicit BuildCharge(const std::shared_ptr<INpmTaskBudget>& budget) : budget_(budget) {}
    ~BuildCharge() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kModuleState, kSingleRowBuildCharge);
    }
    bool Reserve() {
        reserved_ = budget_->Reserve(NpmBudgetCategory::kModuleState, kSingleRowBuildCharge) == NpmBudgetError::kNone;
        return reserved_;
    }

 private:
    const std::shared_ptr<INpmTaskBudget>& budget_;
    bool reserved_ = false;
};

template <typename Builder, typename Value>
bool Append(Builder& builder, const Value& value) {
    return builder.Append(value).ok();
}

template <typename Builder, typename Value>
bool Append(Builder& builder, const std::optional<Value>& value) {
    return value ? builder.Append(*value).ok() : builder.AppendNull().ok();
}

std::optional<uint8_t> DirectionValue(std::optional<NpmPacketDirection> direction) {
    return direction ? std::optional<uint8_t>(static_cast<uint8_t>(*direction)) : std::nullopt;
}

}  // namespace

int EmitNpmDnsTransactionV1(const NpmDnsUdpResultV1& result, const NpmDnsSessionIdentityV1& session,
                            const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                            arrow::MemoryPool* pool) {
    if (!budget || result.entity_instance_id == 0 || session.session_id == 0 ||
        result.session_id != session.session_id ||
        (session.transport_protocol != 6 && session.transport_protocol != 17) || session.a_ip.empty() ||
        session.b_ip.empty() || session.a_ip.size() > 45 || session.b_ip.size() > 45 || result.outcome.empty() ||
        result.outcome.size() > 32 || (result.qname && result.qname->size() > 1024) ||
        (result.incomplete_reason && result.incomplete_reason->size() > 64) ||
        !arrow::util::ValidateUTF8(session.a_ip) || !arrow::util::ValidateUTF8(session.b_ip) ||
        !arrow::util::ValidateUTF8(result.outcome) || (result.qname && !arrow::util::ValidateUTF8(*result.qname)) ||
        (result.incomplete_reason && !arrow::util::ValidateUTF8(*result.incomplete_reason)))
        return EINVAL;
    BuildCharge charge(budget);
    if (!charge.Reserve()) return ENOSPC;
    try {
        if (!pool) pool = arrow::default_memory_pool();
        arrow::UInt64Builder entity_id(pool), revision(pool), session_id(pool), observation_domain_id(pool);
        arrow::Int64Builder observed_at(pool), query_at(pool), response_at(pool), latency(pool);
        arrow::BooleanBuilder is_final(pool), response_tc(pool);
        arrow::UInt8Builder transport_protocol(pool), query_direction(pool), response_direction(pool);
        arrow::StringBuilder a_ip(pool), b_ip(pool), outcome(pool), qname(pool), incomplete_reason(pool);
        arrow::UInt16Builder a_port(pool), b_port(pool), dns_id(pool), qtype(pool), qclass(pool), response_rcode(pool);
        arrow::UInt32Builder query_retries(pool);

        if (!Append(entity_id, result.entity_instance_id) || !Append(revision, uint64_t{1}) ||
            !Append(observed_at, result.observed_at_ns) || !Append(is_final, true) ||
            !Append(session_id, session.session_id) || !Append(observation_domain_id, session.observation_domain_id) ||
            !Append(transport_protocol, session.transport_protocol) || !Append(a_ip, session.a_ip) ||
            !Append(b_ip, session.b_ip) || !Append(a_port, session.a_port) || !Append(b_port, session.b_port) ||
            !Append(dns_id, result.dns_id) || !Append(outcome, result.outcome) ||
            !Append(query_retries, result.query_retries) ||
            !Append(query_direction, DirectionValue(result.query_direction)) ||
            !Append(response_direction, DirectionValue(result.response_direction)) || !Append(qname, result.qname) ||
            !Append(qtype, result.qtype) || !Append(qclass, result.qclass) || !Append(query_at, result.query_at_ns) ||
            !Append(response_at, result.response_at_ns) || !Append(latency, result.latency_ns) ||
            !Append(response_rcode, result.response_rcode) || !Append(response_tc, result.response_tc) ||
            !Append(incomplete_reason, result.incomplete_reason))
            return ENOMEM;

        const std::array<arrow::ArrayBuilder*, 25> builders = {&entity_id,
                                                               &revision,
                                                               &observed_at,
                                                               &is_final,
                                                               &session_id,
                                                               &observation_domain_id,
                                                               &transport_protocol,
                                                               &a_ip,
                                                               &b_ip,
                                                               &a_port,
                                                               &b_port,
                                                               &dns_id,
                                                               &outcome,
                                                               &query_retries,
                                                               &query_direction,
                                                               &response_direction,
                                                               &qname,
                                                               &qtype,
                                                               &qclass,
                                                               &query_at,
                                                               &response_at,
                                                               &latency,
                                                               &response_rcode,
                                                               &response_tc,
                                                               &incomplete_reason};
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(builders.size());
        for (auto* builder : builders) {
            std::shared_ptr<arrow::Array> column;
            const auto status = builder->Finish(&column);
            if (!status.ok()) return status.IsOutOfMemory() ? ENOMEM : EIO;
            columns.push_back(std::move(column));
        }
        const auto schema = NpmDnsTransactionEntityDescriptorV1().schema;
        auto batch = arrow::RecordBatch::Make(schema, 1, std::move(columns));
        return emitter.Emit("dns_transaction", *batch);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

}  // namespace flowsql::npm
