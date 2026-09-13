// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_encoder.h"

#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <arrow/util/utf8.h>

#include <new>
#include <string>
#include <utility>

namespace flowsql::npm {
namespace {

NpmBasicEncodeError Fail(NpmBasicEncodeError code, const std::string& message, std::string* error) {
    if (error != nullptr) *error = message;
    return code;
}

bool ValidResultUtf8(const NpmBasicResult& result, std::string* field) {
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
        if (reserved_) {
            budget_->Release(NpmBudgetCategory::kPendingOutput, bytes_);
        }
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

NpmBasicEncodeError EncodeNpmBasicResults(const std::vector<NpmBasicResult>& results,
                                          std::shared_ptr<arrow::RecordBatch>* output,
                                          std::string* error) {
    if (output == nullptr) return Fail(NpmBasicEncodeError::kNullOutput, "output is null", error);

    try {
        for (size_t index = 0; index < results.size(); ++index) {
            const auto validation = ValidateNpmBasicResult(results[index]);
            if (validation != NpmBasicResultError::kNone) {
                return Fail(NpmBasicEncodeError::kInvalidResult,
                            "result[" + std::to_string(index) + "] is invalid: " +
                                std::to_string(static_cast<uint8_t>(validation)),
                            error);
            }
            std::string invalid_utf8_field;
            if (!ValidResultUtf8(results[index], &invalid_utf8_field)) {
                return Fail(NpmBasicEncodeError::kInvalidResult,
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
        arrow::UInt64Builder packets_ab;
        arrow::UInt64Builder packets_ba;
        arrow::UInt64Builder wire_bytes_ab;
        arrow::UInt64Builder wire_bytes_ba;
        arrow::StringBuilder protocol_status;
        arrow::UInt16Builder protocol_id;
        arrow::UInt16Builder protocol_sub_id;
        arrow::StringBuilder protocol;
        arrow::StringBuilder end_reason;

        auto check = [&](const arrow::Status& status, const char* field) {
            if (status.ok()) return true;
            if (error != nullptr) *error = std::string(field) + ": " + status.ToString();
            return false;
        };
        auto append_optional_uint16 = [&](const std::optional<uint16_t>& value,
                                          arrow::UInt16Builder* builder,
                                          const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };
        auto append_optional_string = [&](const std::optional<std::string>& value,
                                          arrow::StringBuilder* builder,
                                          const char* field) {
            return value.has_value() ? check(builder->Append(*value), field)
                                     : check(builder->AppendNull(), field);
        };

        for (const auto& result : results) {
            const char* status_name = NpmProtocolStatusName(result.protocol_status);
            if (!check(session_id.Append(result.session_id), "session_id") ||
                !check(observation_domain_id.Append(result.observation_domain_id), "observation_domain_id") ||
                !check(revision.Append(result.revision), "revision") ||
                !check(observed_at.Append(result.observed_at), "observed_at") ||
                !check(is_final.Append(result.is_final), "is_final") ||
                !check(ip_family.Append(result.ip_family), "ip_family") ||
                !check(transport_protocol.Append(result.transport_protocol), "transport_protocol") ||
                !check(a_ip.Append(result.a_ip), "a_ip") || !check(b_ip.Append(result.b_ip), "b_ip") ||
                !check(a_port.Append(result.a_port), "a_port") ||
                !check(b_port.Append(result.b_port), "b_port") ||
                !check(first_ns.Append(result.first_ns), "first_ns") ||
                !check(last_ns.Append(result.last_ns), "last_ns") ||
                !check(packets_ab.Append(result.packets_ab), "packets_ab") ||
                !check(packets_ba.Append(result.packets_ba), "packets_ba") ||
                !check(wire_bytes_ab.Append(result.wire_bytes_ab), "wire_bytes_ab") ||
                !check(wire_bytes_ba.Append(result.wire_bytes_ba), "wire_bytes_ba") ||
                !check(protocol_status.Append(status_name), "protocol_status") ||
                !append_optional_uint16(result.protocol_id, &protocol_id, "protocol_id") ||
                !append_optional_uint16(result.protocol_sub_id, &protocol_sub_id, "protocol_sub_id") ||
                !append_optional_string(result.protocol, &protocol, "protocol")) {
                return NpmBasicEncodeError::kArrowError;
            }
            if (result.end_reason.has_value()) {
                if (!check(end_reason.Append(NpmSessionEndReasonName(*result.end_reason)), "end_reason")) {
                    return NpmBasicEncodeError::kArrowError;
                }
            } else if (!check(end_reason.AppendNull(), "end_reason")) {
                return NpmBasicEncodeError::kArrowError;
            }
        }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(22);
        auto finish = [&](auto* builder, const char* field) {
            std::shared_ptr<arrow::Array> array;
            if (!check(builder->Finish(&array), field)) return false;
            arrays.push_back(std::move(array));
            return true;
        };
        if (!finish(&session_id, "session_id") ||
            !finish(&observation_domain_id, "observation_domain_id") || !finish(&revision, "revision") ||
            !finish(&observed_at, "observed_at") || !finish(&is_final, "is_final") ||
            !finish(&ip_family, "ip_family") || !finish(&transport_protocol, "transport_protocol") ||
            !finish(&a_ip, "a_ip") || !finish(&b_ip, "b_ip") || !finish(&a_port, "a_port") ||
            !finish(&b_port, "b_port") || !finish(&first_ns, "first_ns") ||
            !finish(&last_ns, "last_ns") || !finish(&packets_ab, "packets_ab") ||
            !finish(&packets_ba, "packets_ba") || !finish(&wire_bytes_ab, "wire_bytes_ab") ||
            !finish(&wire_bytes_ba, "wire_bytes_ba") || !finish(&protocol_status, "protocol_status") ||
            !finish(&protocol_id, "protocol_id") || !finish(&protocol_sub_id, "protocol_sub_id") ||
            !finish(&protocol, "protocol") || !finish(&end_reason, "end_reason")) {
            return NpmBasicEncodeError::kArrowError;
        }

        auto batch = arrow::RecordBatch::Make(
            NpmBasicResultSchema(), static_cast<int64_t>(results.size()), std::move(arrays));
        const auto validation = batch->ValidateFull();
        if (!validation.ok()) {
            return Fail(NpmBasicEncodeError::kArrowError,
                        std::string("encoded batch: ") + validation.ToString(),
                        error);
        }
        *output = std::move(batch);
        if (error != nullptr) error->clear();
        return NpmBasicEncodeError::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(NpmBasicEncodeError::kAllocationFailed, "NPM basic result allocation failed", error);
    }
}

NpmBasicEncodeError EncodeNpmBasicResultsWithBudget(
    const std::vector<NpmBasicResult>& results,
    const std::shared_ptr<INpmTaskBudget>& budget,
    std::shared_ptr<arrow::RecordBatch>* output,
    std::string* error) {
    if (output == nullptr) return Fail(NpmBasicEncodeError::kNullOutput, "output is null", error);
    if (budget == nullptr) return Fail(NpmBasicEncodeError::kNullBudget, "budget is null", error);

    try {
        std::shared_ptr<arrow::RecordBatch> batch;
        const auto encode_result = EncodeNpmBasicResults(results, &batch, error);
        if (encode_result != NpmBasicEncodeError::kNone) return encode_result;

        const int64_t signed_bytes = arrow::util::TotalBufferSize(*batch);
        if (signed_bytes < 0) {
            return Fail(NpmBasicEncodeError::kInvalidBufferSize,
                        "encoded batch has a negative Arrow buffer size",
                        error);
        }

        const uint64_t bytes = static_cast<uint64_t>(signed_bytes);
        auto owner = std::make_shared<PendingOutputOwner>(std::move(batch), budget, bytes);
        const auto budget_result = owner->Reserve();
        if (budget_result != NpmBudgetError::kNone) {
            return Fail(NpmBasicEncodeError::kBudgetError,
                        std::string("pending output budget reservation failed: ") +
                            BudgetErrorName(budget_result),
                        error);
        }

        std::shared_ptr<arrow::RecordBatch> leased_batch(owner, owner->batch());
        *output = std::move(leased_batch);
        if (error != nullptr) error->clear();
        return NpmBasicEncodeError::kNone;
    } catch (const std::bad_alloc&) {
        return Fail(NpmBasicEncodeError::kAllocationFailed,
                    "NPM basic pending output allocation failed",
                    error);
    }
}

}  // namespace flowsql::npm
