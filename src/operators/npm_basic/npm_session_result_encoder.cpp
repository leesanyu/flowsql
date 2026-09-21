// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_session_result_encoder.h"

#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/utf8.h>

#include <new>
#include <string>
#include <utility>

namespace flowsql::npm {
namespace {

NpmSessionEncodeError Fail(NpmSessionEncodeError code,
                           const std::string& message,
                           std::string* error) {
    if (error != nullptr) *error = message;
    return code;
}

bool ValidResultUtf8(const NpmSessionResult& result, std::string* field) {
    if (!arrow::util::ValidateUTF8(result.a_ip)) {
        *field = "a_ip";
        return false;
    }
    if (!arrow::util::ValidateUTF8(result.b_ip)) {
        *field = "b_ip";
        return false;
    }
    if (result.protocol.has_value() && !arrow::util::ValidateUTF8(*result.protocol)) {
        *field = "protocol";
        return false;
    }
    return true;
}

const char* BudgetErrorName(NpmBudgetError error) {
    switch (error) {
        case NpmBudgetError::kNone:
            return "none";
        case NpmBudgetError::kNullUsage:
            return "null usage";
        case NpmBudgetError::kInvalidCategory:
            return "invalid category";
        case NpmBudgetError::kTrackedLimitExceeded:
            return "tracked limit exceeded";
        case NpmBudgetError::kPendingOutputLimitExceeded:
            return "pending output limit exceeded";
        case NpmBudgetError::kReleaseUnderflow:
            return "release underflow";
    }
    return "unknown budget error";
}

class PendingOutputOwner {
 public:
    PendingOutputOwner(std::shared_ptr<arrow::RecordBatch> batch,
                       std::shared_ptr<INpmTaskBudget> budget,
                       uint64_t bytes)
        : batch_(std::move(batch)), budget_(std::move(budget)), bytes_(bytes) {}

    ~PendingOutputOwner() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kPendingOutput, bytes_);
    }

    NpmBudgetError Reserve() {
        const auto result = budget_->Reserve(NpmBudgetCategory::kPendingOutput, bytes_);
        reserved_ = result == NpmBudgetError::kNone;
        return result;
    }

    arrow::RecordBatch* batch() const { return batch_.get(); }

 private:
    std::shared_ptr<arrow::RecordBatch> batch_;
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t bytes_ = 0;
    bool reserved_ = false;
};

}  // namespace

NpmSessionEncodeError EncodeNpmSessionResults(const std::vector<NpmSessionResult>& results,
                                              std::shared_ptr<arrow::RecordBatch>* output, std::string* error,
                                              bool labeling_enabled) {
    if (output == nullptr) {
        return Fail(NpmSessionEncodeError::kNullOutput, "output is null", error);
    }

    try {
        for (size_t index = 0; index < results.size(); ++index) {
            const auto validation = ValidateNpmSessionResult(results[index]);
            if (validation != NpmSessionResultError::kNone) {
                return Fail(NpmSessionEncodeError::kInvalidResult,
                            "result[" + std::to_string(index) + "] is invalid: " +
                                std::to_string(static_cast<uint8_t>(validation)),
                            error);
            }
            std::string invalid_utf8_field;
            if (!ValidResultUtf8(results[index], &invalid_utf8_field)) {
                return Fail(NpmSessionEncodeError::kInvalidResult,
                            "result[" + std::to_string(index) + "]." + invalid_utf8_field +
                                " is not valid UTF-8",
                            error);
            }
        }

        arrow::UInt64Builder session_id;
        arrow::UInt64Builder observation_domain_id;
        arrow::UInt64Builder revision;
        arrow::Int64Builder observed_at;
        arrow::BooleanBuilder is_final;
        arrow::UInt8Builder ip_family;
        arrow::UInt8Builder transport_protocol;
        arrow::StringBuilder a_ip;
        arrow::StringBuilder b_ip;
        arrow::UInt16Builder a_port;
        arrow::UInt16Builder b_port;
        arrow::Int64Builder first_ns;
        arrow::Int64Builder last_ns;
        arrow::Int64Builder duration_ns;
        arrow::UInt32Builder primary_label_id;
        arrow::StringBuilder protocol_status;
        arrow::UInt16Builder protocol_id;
        arrow::UInt16Builder protocol_sub_id;
        arrow::StringBuilder protocol;
        arrow::StringBuilder end_reason;
        arrow::UInt64Builder packets_ab;
        arrow::UInt64Builder packets_ba;
        arrow::UInt64Builder wire_bytes_ab;
        arrow::UInt64Builder wire_bytes_ba;
        arrow::UInt64Builder payload_bytes_ab;
        arrow::UInt64Builder payload_bytes_ba;
        arrow::StringBuilder rate_status;
        arrow::DoubleBuilder wire_bps_ab;
        arrow::DoubleBuilder wire_bps_ba;
        arrow::DoubleBuilder payload_bps_ab;
        arrow::DoubleBuilder payload_bps_ba;
        arrow::UInt64Builder tcp_unique_payload_bytes_ab;
        arrow::UInt64Builder tcp_unique_payload_bytes_ba;
        arrow::DoubleBuilder tcp_unique_payload_bps_ab;
        arrow::DoubleBuilder tcp_unique_payload_bps_ba;
        arrow::StringBuilder tcp_handshake_status;
        arrow::StringBuilder tcp_initiator;
        arrow::Int64Builder tcp_handshake_duration_ns;
        arrow::Int64Builder tcp_synack_rtt_ns;
        arrow::StringBuilder tcp_rtt_status;
        arrow::UInt64Builder tcp_rtt_samples;
        arrow::Int64Builder tcp_rtt_min_ns;
        arrow::Int64Builder tcp_rtt_mean_ns;
        arrow::Int64Builder tcp_rtt_max_ns;
        arrow::StringBuilder tcp_retransmission_status;
        arrow::UInt64Builder tcp_retrans_packets_ab;
        arrow::UInt64Builder tcp_retrans_packets_ba;
        arrow::UInt64Builder tcp_retrans_payload_bytes_ab;
        arrow::UInt64Builder tcp_retrans_payload_bytes_ba;
        arrow::UInt32Builder measurement_flags;

        const auto check = [&](const arrow::Status& status, const char* field) {
            if (status.ok()) return true;
            if (error != nullptr) *error = std::string(field) + ": " + status.ToString();
            return false;
        };
        const auto append_optional_uint16 = [&](const std::optional<uint16_t>& value,
                                                arrow::UInt16Builder* builder,
                                                const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };
        const auto append_optional_uint64 = [&](const std::optional<uint64_t>& value,
                                                arrow::UInt64Builder* builder,
                                                const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };
        const auto append_optional_int64 = [&](const std::optional<int64_t>& value,
                                               arrow::Int64Builder* builder,
                                               const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };
        const auto append_optional_double = [&](const std::optional<double>& value,
                                                arrow::DoubleBuilder* builder,
                                                const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };
        const auto append_optional_string = [&](const std::optional<std::string>& value,
                                                arrow::StringBuilder* builder,
                                                const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };

        for (const auto& result : results) {
            if (!check(session_id.Append(result.session_id), "session_id") ||
                !check(observation_domain_id.Append(result.observation_domain_id),
                       "observation_domain_id") ||
                !check(revision.Append(result.revision), "revision") ||
                !check(observed_at.Append(result.observed_at), "observed_at") ||
                !check(is_final.Append(result.is_final), "is_final") ||
                !check(ip_family.Append(result.ip_family), "ip_family") ||
                !check(transport_protocol.Append(result.transport_protocol),
                       "transport_protocol") ||
                !check(a_ip.Append(result.a_ip), "a_ip") ||
                !check(b_ip.Append(result.b_ip), "b_ip") ||
                !check(a_port.Append(result.a_port), "a_port") ||
                !check(b_port.Append(result.b_port), "b_port") ||
                !check(first_ns.Append(result.first_ns), "first_ns") ||
                !check(last_ns.Append(result.last_ns), "last_ns") ||
                !check(duration_ns.Append(result.duration_ns), "duration_ns") ||
                !check(protocol_status.Append(NpmProtocolStatusName(result.protocol_status)),
                       "protocol_status") ||
                !append_optional_uint16(result.protocol_id, &protocol_id, "protocol_id") ||
                !append_optional_uint16(
                    result.protocol_sub_id, &protocol_sub_id, "protocol_sub_id") ||
                !append_optional_string(result.protocol, &protocol, "protocol")) {
                return NpmSessionEncodeError::kArrowError;
            }
            if (labeling_enabled && !check(primary_label_id.Append(result.primary_label_id), "primary_label_id")) {
                return NpmSessionEncodeError::kArrowError;
            }
            if (result.end_reason.has_value()) {
                if (!check(end_reason.Append(NpmSessionEndReasonName(*result.end_reason)),
                           "end_reason")) {
                    return NpmSessionEncodeError::kArrowError;
                }
            } else if (!check(end_reason.AppendNull(), "end_reason")) {
                return NpmSessionEncodeError::kArrowError;
            }
            if (!check(packets_ab.Append(result.packets_ab), "packets_ab") ||
                !check(packets_ba.Append(result.packets_ba), "packets_ba") ||
                !check(wire_bytes_ab.Append(result.wire_bytes_ab), "wire_bytes_ab") ||
                !check(wire_bytes_ba.Append(result.wire_bytes_ba), "wire_bytes_ba") ||
                !check(payload_bytes_ab.Append(result.payload_bytes_ab), "payload_bytes_ab") ||
                !check(payload_bytes_ba.Append(result.payload_bytes_ba), "payload_bytes_ba") ||
                !check(rate_status.Append(NpmRateStatusName(result.rate_status)), "rate_status") ||
                !append_optional_double(result.wire_bps_ab, &wire_bps_ab, "wire_bps_ab") ||
                !append_optional_double(result.wire_bps_ba, &wire_bps_ba, "wire_bps_ba") ||
                !append_optional_double(
                    result.payload_bps_ab, &payload_bps_ab, "payload_bps_ab") ||
                !append_optional_double(
                    result.payload_bps_ba, &payload_bps_ba, "payload_bps_ba") ||
                !append_optional_uint64(result.tcp_unique_payload_bytes_ab,
                                        &tcp_unique_payload_bytes_ab,
                                        "tcp_unique_payload_bytes_ab") ||
                !append_optional_uint64(result.tcp_unique_payload_bytes_ba,
                                        &tcp_unique_payload_bytes_ba,
                                        "tcp_unique_payload_bytes_ba") ||
                !append_optional_double(result.tcp_unique_payload_bps_ab,
                                        &tcp_unique_payload_bps_ab,
                                        "tcp_unique_payload_bps_ab") ||
                !append_optional_double(result.tcp_unique_payload_bps_ba,
                                        &tcp_unique_payload_bps_ba,
                                        "tcp_unique_payload_bps_ba") ||
                !check(tcp_handshake_status.Append(
                           NpmTcpHandshakeStatusName(result.tcp_handshake_status)),
                       "tcp_handshake_status")) {
                return NpmSessionEncodeError::kArrowError;
            }
            if (result.tcp_initiator.has_value()) {
                if (!check(tcp_initiator.Append(NpmTcpInitiatorName(*result.tcp_initiator)),
                           "tcp_initiator")) {
                    return NpmSessionEncodeError::kArrowError;
                }
            } else if (!check(tcp_initiator.AppendNull(), "tcp_initiator")) {
                return NpmSessionEncodeError::kArrowError;
            }
            if (!append_optional_int64(result.tcp_handshake_duration_ns,
                                       &tcp_handshake_duration_ns,
                                       "tcp_handshake_duration_ns") ||
                !append_optional_int64(
                    result.tcp_synack_rtt_ns, &tcp_synack_rtt_ns, "tcp_synack_rtt_ns") ||
                !check(tcp_rtt_status.Append(NpmTcpRttStatusName(result.tcp_rtt_status)),
                       "tcp_rtt_status") ||
                !append_optional_uint64(
                    result.tcp_rtt_samples, &tcp_rtt_samples, "tcp_rtt_samples") ||
                !append_optional_int64(
                    result.tcp_rtt_min_ns, &tcp_rtt_min_ns, "tcp_rtt_min_ns") ||
                !append_optional_int64(
                    result.tcp_rtt_mean_ns, &tcp_rtt_mean_ns, "tcp_rtt_mean_ns") ||
                !append_optional_int64(
                    result.tcp_rtt_max_ns, &tcp_rtt_max_ns, "tcp_rtt_max_ns") ||
                !check(tcp_retransmission_status.Append(
                           NpmTcpRetransmissionStatusName(result.tcp_retransmission_status)),
                       "tcp_retransmission_status") ||
                !append_optional_uint64(result.tcp_retrans_packets_ab,
                                        &tcp_retrans_packets_ab,
                                        "tcp_retrans_packets_ab") ||
                !append_optional_uint64(result.tcp_retrans_packets_ba,
                                        &tcp_retrans_packets_ba,
                                        "tcp_retrans_packets_ba") ||
                !append_optional_uint64(result.tcp_retrans_payload_bytes_ab,
                                        &tcp_retrans_payload_bytes_ab,
                                        "tcp_retrans_payload_bytes_ab") ||
                !append_optional_uint64(result.tcp_retrans_payload_bytes_ba,
                                        &tcp_retrans_payload_bytes_ba,
                                        "tcp_retrans_payload_bytes_ba") ||
                !check(measurement_flags.Append(result.measurement_flags),
                       "measurement_flags")) {
                return NpmSessionEncodeError::kArrowError;
            }
        }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(labeling_enabled ? 50 : 49);
        const auto finish = [&](auto* builder, const char* field) {
            std::shared_ptr<arrow::Array> array;
            if (!check(builder->Finish(&array), field)) return false;
            arrays.push_back(std::move(array));
            return true;
        };
        if (!finish(&session_id, "session_id") || !finish(&observation_domain_id, "observation_domain_id") ||
            !finish(&revision, "revision") || !finish(&observed_at, "observed_at") || !finish(&is_final, "is_final") ||
            !finish(&ip_family, "ip_family") || !finish(&transport_protocol, "transport_protocol") ||
            !finish(&a_ip, "a_ip") || !finish(&b_ip, "b_ip") || !finish(&a_port, "a_port") ||
            !finish(&b_port, "b_port") || !finish(&first_ns, "first_ns") || !finish(&last_ns, "last_ns") ||
            !finish(&duration_ns, "duration_ns")) {
            return NpmSessionEncodeError::kArrowError;
        }
        if (labeling_enabled && !finish(&primary_label_id, "primary_label_id")) {
            return NpmSessionEncodeError::kArrowError;
        }
        if (!finish(&protocol_status, "protocol_status") || !finish(&protocol_id, "protocol_id") ||
            !finish(&protocol_sub_id, "protocol_sub_id") || !finish(&protocol, "protocol") ||
            !finish(&end_reason, "end_reason") || !finish(&packets_ab, "packets_ab") ||
            !finish(&packets_ba, "packets_ba") || !finish(&wire_bytes_ab, "wire_bytes_ab") ||
            !finish(&wire_bytes_ba, "wire_bytes_ba") || !finish(&payload_bytes_ab, "payload_bytes_ab") ||
            !finish(&payload_bytes_ba, "payload_bytes_ba") || !finish(&rate_status, "rate_status") ||
            !finish(&wire_bps_ab, "wire_bps_ab") || !finish(&wire_bps_ba, "wire_bps_ba") ||
            !finish(&payload_bps_ab, "payload_bps_ab") || !finish(&payload_bps_ba, "payload_bps_ba") ||
            !finish(&tcp_unique_payload_bytes_ab, "tcp_unique_payload_bytes_ab") ||
            !finish(&tcp_unique_payload_bytes_ba, "tcp_unique_payload_bytes_ba") ||
            !finish(&tcp_unique_payload_bps_ab, "tcp_unique_payload_bps_ab") ||
            !finish(&tcp_unique_payload_bps_ba, "tcp_unique_payload_bps_ba") ||
            !finish(&tcp_handshake_status, "tcp_handshake_status") || !finish(&tcp_initiator, "tcp_initiator") ||
            !finish(&tcp_handshake_duration_ns, "tcp_handshake_duration_ns") ||
            !finish(&tcp_synack_rtt_ns, "tcp_synack_rtt_ns") || !finish(&tcp_rtt_status, "tcp_rtt_status") ||
            !finish(&tcp_rtt_samples, "tcp_rtt_samples") || !finish(&tcp_rtt_min_ns, "tcp_rtt_min_ns") ||
            !finish(&tcp_rtt_mean_ns, "tcp_rtt_mean_ns") || !finish(&tcp_rtt_max_ns, "tcp_rtt_max_ns") ||
            !finish(&tcp_retransmission_status, "tcp_retransmission_status") ||
            !finish(&tcp_retrans_packets_ab, "tcp_retrans_packets_ab") ||
            !finish(&tcp_retrans_packets_ba, "tcp_retrans_packets_ba") ||
            !finish(&tcp_retrans_payload_bytes_ab, "tcp_retrans_payload_bytes_ab") ||
            !finish(&tcp_retrans_payload_bytes_ba, "tcp_retrans_payload_bytes_ba") ||
            !finish(&measurement_flags, "measurement_flags")) {
            return NpmSessionEncodeError::kArrowError;
        }

        auto batch = arrow::RecordBatch::Make(NpmSessionResultSchema(labeling_enabled),
                                              static_cast<int64_t>(results.size()), std::move(arrays));
        const auto validation = batch->ValidateFull();
        if (!validation.ok()) {
            return Fail(NpmSessionEncodeError::kArrowError,
                        std::string("encoded batch: ") + validation.ToString(),
                        error);
        }
        *output = std::move(batch);
        if (error != nullptr) error->clear();
        return NpmSessionEncodeError::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(NpmSessionEncodeError::kAllocationFailed,
                    "NPM Session result allocation failed",
                    error);
    }
}

NpmSessionEncodeError EncodeNpmSessionResultsWithBudget(const std::vector<NpmSessionResult>& results,
                                                        const std::shared_ptr<INpmTaskBudget>& budget,
                                                        std::shared_ptr<arrow::RecordBatch>* output, std::string* error,
                                                        bool labeling_enabled) {
    if (output == nullptr) {
        return Fail(NpmSessionEncodeError::kNullOutput, "output is null", error);
    }
    if (budget == nullptr) {
        return Fail(NpmSessionEncodeError::kNullBudget, "budget is null", error);
    }

    try {
        std::shared_ptr<arrow::RecordBatch> batch;
        const auto encode_result = EncodeNpmSessionResults(results, &batch, error, labeling_enabled);
        if (encode_result != NpmSessionEncodeError::kNone) return encode_result;

        const int64_t signed_bytes = arrow::util::TotalBufferSize(*batch);
        if (signed_bytes < 0) {
            return Fail(NpmSessionEncodeError::kInvalidBufferSize,
                        "encoded batch has a negative Arrow buffer size",
                        error);
        }

        const uint64_t bytes = static_cast<uint64_t>(signed_bytes);
        auto owner = std::make_shared<PendingOutputOwner>(std::move(batch), budget, bytes);
        const auto budget_result = owner->Reserve();
        if (budget_result != NpmBudgetError::kNone) {
            return Fail(NpmSessionEncodeError::kBudgetError,
                        std::string("pending output budget reservation failed: ") +
                            BudgetErrorName(budget_result),
                        error);
        }

        std::shared_ptr<arrow::RecordBatch> leased_batch(owner, owner->batch());
        *output = std::move(leased_batch);
        if (error != nullptr) error->clear();
        return NpmSessionEncodeError::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(NpmSessionEncodeError::kAllocationFailed,
                    "NPM Session pending output allocation failed",
                    error);
    }
}

}  // namespace flowsql::npm
