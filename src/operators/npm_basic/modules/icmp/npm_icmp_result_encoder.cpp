// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_result_encoder.h"

#include <arrow/api.h>
#include <arrow/util/utf8.h>

#include <array>
#include <cerrno>
#include <new>
#include <vector>

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

class BuildCharge final {
 public:
    explicit BuildCharge(const std::shared_ptr<INpmTaskBudget>& budget) : budget_(budget) {}
    bool Reserve() {
        reserved_ = budget_->Reserve(NpmBudgetCategory::kModuleState, 8192) == NpmBudgetError::kNone;
        return reserved_;
    }
    ~BuildCharge() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kModuleState, 8192);
    }

 private:
    const std::shared_ptr<INpmTaskBudget>& budget_;
    bool reserved_ = false;
};

bool ValidRow(const NpmIcmpEventV1& event) {
    if (!event.entity_instance_id || (event.ip_family != 4 && event.ip_family != 6) || event.src_ip.empty() ||
        event.dst_ip.empty() || event.src_ip.size() > 45 || event.dst_ip.size() > 45 ||
        !arrow::util::ValidateUTF8(event.src_ip) || !arrow::util::ValidateUTF8(event.dst_ip))
        return false;
    const bool matched = event.outcome == "echo_matched";
    const bool request_only = event.outcome == "echo_request_only";
    const bool reply_only = event.outcome == "echo_reply_only";
    const bool error = event.outcome == "icmp_error";
    if (!matched && !request_only && !reply_only && !error) return false;
    if (matched && (event.incomplete_reason || !event.request_at_ns || !event.reply_at_ns || !event.echo_id ||
                    !event.echo_sequence || !event.echo_retries))
        return false;
    if ((request_only || reply_only) && !event.incomplete_reason) return false;
    if (error && (event.incomplete_reason || !event.quote_status || event.echo_id || event.echo_sequence ||
                  event.echo_retries || event.request_at_ns || event.reply_at_ns || event.latency_ns))
        return false;
    if (!error && (event.quote_status || event.quoted_ip_family || event.quoted_protocol || event.quoted_src_ip ||
                   event.quoted_dst_ip || event.quoted_src_port || event.quoted_dst_port ||
                   event.active_quoted_session_id || event.next_hop_mtu))
        return false;
    if (event.quoted_src_ip.has_value() != event.quoted_dst_ip.has_value() ||
        event.quoted_src_port.has_value() != event.quoted_dst_port.has_value())
        return false;
    return true;
}

}  // namespace

int EmitNpmIcmpEventV1(const NpmIcmpEventV1& event, const std::shared_ptr<INpmTaskBudget>& budget,
                       INpmResultEmitterV1& emitter, arrow::MemoryPool* pool) {
    if (!budget || !ValidRow(event)) return EINVAL;
    BuildCharge charge(budget);
    if (!charge.Reserve()) return ENOSPC;
    if (!pool) pool = arrow::default_memory_pool();
    try {
        arrow::UInt64Builder id(pool), revision(pool), domain(pool), session(pool);
        arrow::Int64Builder observed(pool), request_at(pool), reply_at(pool), latency(pool);
        arrow::BooleanBuilder is_final(pool), truncated(pool);
        arrow::UInt8Builder family(pool), type(pool), code(pool), quote_family(pool), quote_protocol(pool);
        arrow::UInt16Builder echo_id(pool), echo_sequence(pool), quote_src_port(pool), quote_dst_port(pool);
        arrow::UInt32Builder retries(pool), mtu(pool);
        arrow::StringBuilder src(pool), dst(pool), outcome(pool), reason(pool), quote_status(pool), quote_src(pool),
            quote_dst(pool);
        if (!Append(id, event.entity_instance_id) || !Append(revision, uint64_t{1}) ||
            !Append(observed, event.observed_at) || !Append(is_final, true) ||
            !Append(domain, event.observation_domain_id) || !Append(family, event.ip_family) ||
            !Append(src, event.src_ip) || !Append(dst, event.dst_ip) || !Append(outcome, event.outcome) ||
            !Append(type, event.icmp_type) || !Append(code, event.icmp_code) ||
            !Append(truncated, event.outer_truncated) || !Append(reason, event.incomplete_reason) ||
            !Append(echo_id, event.echo_id) || !Append(echo_sequence, event.echo_sequence) ||
            !Append(retries, event.echo_retries) || !Append(request_at, event.request_at_ns) ||
            !Append(reply_at, event.reply_at_ns) || !Append(latency, event.latency_ns) ||
            !Append(quote_status, event.quote_status) || !Append(quote_family, event.quoted_ip_family) ||
            !Append(quote_protocol, event.quoted_protocol) || !Append(quote_src, event.quoted_src_ip) ||
            !Append(quote_dst, event.quoted_dst_ip) || !Append(quote_src_port, event.quoted_src_port) ||
            !Append(quote_dst_port, event.quoted_dst_port) || !Append(session, event.active_quoted_session_id) ||
            !Append(mtu, event.next_hop_mtu))
            return ENOMEM;
        const std::array<arrow::ArrayBuilder*, 28> builders = {&id,
                                                               &revision,
                                                               &observed,
                                                               &is_final,
                                                               &domain,
                                                               &family,
                                                               &src,
                                                               &dst,
                                                               &outcome,
                                                               &type,
                                                               &code,
                                                               &truncated,
                                                               &reason,
                                                               &echo_id,
                                                               &echo_sequence,
                                                               &retries,
                                                               &request_at,
                                                               &reply_at,
                                                               &latency,
                                                               &quote_status,
                                                               &quote_family,
                                                               &quote_protocol,
                                                               &quote_src,
                                                               &quote_dst,
                                                               &quote_src_port,
                                                               &quote_dst_port,
                                                               &session,
                                                               &mtu};
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(builders.size());
        for (auto* builder : builders) {
            std::shared_ptr<arrow::Array> column;
            const auto status = builder->Finish(&column);
            if (!status.ok()) return status.IsOutOfMemory() ? ENOMEM : EIO;
            columns.push_back(std::move(column));
        }
        auto rows = arrow::RecordBatch::Make(NpmIcmpEventEntityDescriptorV1().schema, 1, std::move(columns));
        return emitter.Emit("icmp_event", *rows);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

}  // namespace flowsql::npm
