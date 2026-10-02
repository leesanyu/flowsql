// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_result_encoder.h"

#include "npm_tls_hello.h"

#include <arrow/api.h>
#include <arrow/util/utf8.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <new>
#include <string_view>
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

bool ValidRow(const NpmTlsHandshakeResultV1& row, const NpmTlsSessionIdentityV1& session) {
    const auto& handshake = row.handshake;
    if (!handshake.entity_instance_id || !session.session_id || handshake.session_id != session.session_id ||
        session.a_ip.empty() || session.b_ip.empty() || session.a_ip.size() > 45 || session.b_ip.size() > 45 ||
        !arrow::util::ValidateUTF8(session.a_ip) || !arrow::util::ValidateUTF8(session.b_ip) ||
        (handshake.client_direction != NpmPacketDirection::kAToB &&
         handshake.client_direction != NpmPacketDirection::kBToA) ||
        handshake.hello_retry_count > 1 || handshake.phase != NpmTlsHandshakePhaseV1::kClosed)
        return false;
    if (row.outcome == NpmTlsOutcomeV1::kFatalAlertObserved)
        return row.incomplete_reason == NpmTlsIncompleteReasonV1::kNone && row.alert_level == 2 &&
               row.alert_description.has_value();
    return row.incomplete_reason != NpmTlsIncompleteReasonV1::kNone && !row.alert_level && !row.alert_description;
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

int EmitNpmTlsHandshakeV1(const NpmTlsHandshakeResultV1& result, const NpmTlsSessionIdentityV1& session,
                          const std::shared_ptr<INpmTaskBudget>& budget, INpmResultEmitterV1& emitter,
                          arrow::MemoryPool* pool) {
    if (!budget || !ValidRow(result, session)) return EINVAL;
    const auto& handshake = result.handshake;
    const auto& client = handshake.first_client_hello;
    const auto& server = handshake.final_server_hello;
    BuildCharge charge(budget, 16 * 1024 + (client ? 2 * client->offered_alpn.size() * 255 : 0) +
                                   (server && server->selected_alpn ? 6 * server->selected_alpn->size() : 0));
    if (!charge.Reserve()) return ENOSPC;
    try {
        std::optional<std::string> alpn_json;
        if (client && !client->offered_alpn.empty()) {
            std::string json;
            const auto encoded = EncodeNpmTlsAlpnJsonV1(client->offered_alpn, &json);
            if (encoded == NpmTlsHelloErrorV1::kAllocationFailed) return ENOMEM;
            if (encoded != NpmTlsHelloErrorV1::kNone) return EINVAL;
            alpn_json = std::move(json);
        }
        const std::optional<bool> offered_tls13 =
            client ? std::optional<bool>(std::find(client->offered_versions.begin(), client->offered_versions.end(),
                                                   0x0304) != client->offered_versions.end())
                   : std::nullopt;
        const std::optional<uint16_t> selected_version =
            server ? std::optional<uint16_t>(server->selected_version.value_or(server->legacy_version)) : std::nullopt;
        const std::optional<uint16_t> cipher = server ? server->cipher_suite : std::nullopt;
        std::optional<std::string> selected_alpn;
        if (server && selected_version != 0x0304 && server->selected_alpn) {
            std::string json;
            const auto encoded = EncodeNpmTlsAlpnJsonV1({*server->selected_alpn}, &json);
            if (encoded == NpmTlsHelloErrorV1::kAllocationFailed) return ENOMEM;
            if (encoded != NpmTlsHelloErrorV1::kNone) return EINVAL;
            selected_alpn = json.substr(1, json.size() - 2);  // One JSON string, without the array brackets.
        }
        const std::optional<int64_t> client_at = client ? client->complete_at_ns : std::nullopt;
        const std::optional<int64_t> server_at = server ? server->complete_at_ns : std::nullopt;
        const std::optional<std::string_view> reason =
            result.incomplete_reason == NpmTlsIncompleteReasonV1::kNone
                ? std::nullopt
                : std::optional<std::string_view>(NpmTlsIncompleteReasonNameV1(result.incomplete_reason));
        if (!pool) pool = arrow::default_memory_pool();
        arrow::UInt64Builder entity_id(pool), revision(pool), session_id(pool), domain_id(pool);
        arrow::Int64Builder observed_at(pool), client_time(pool), server_time(pool), latency(pool);
        arrow::BooleanBuilder is_final(pool), offered13(pool);
        arrow::StringBuilder a_ip(pool), b_ip(pool), outcome(pool), incomplete_reason(pool), sni(pool), alpn(pool),
            selected_protocol(pool);
        arrow::UInt16Builder a_port(pool), b_port(pool), version(pool), cipher_suite(pool);
        arrow::UInt8Builder retry_count(pool), direction(pool), alert_level(pool), alert_description(pool);
        if (!Append(entity_id, handshake.entity_instance_id) || !Append(revision, uint64_t{1}) ||
            !Append(observed_at, result.observed_at_ns) || !Append(is_final, true) ||
            !Append(session_id, session.session_id) || !Append(domain_id, session.observation_domain_id) ||
            !Append(a_ip, session.a_ip) || !Append(b_ip, session.b_ip) || !Append(a_port, session.a_port) ||
            !Append(b_port, session.b_port) ||
            !Append(outcome, std::string_view(NpmTlsOutcomeNameV1(result.outcome))) ||
            !Append(retry_count, handshake.hello_retry_count) || !Append(incomplete_reason, reason) ||
            !Append(direction, static_cast<uint8_t>(handshake.client_direction)) || !Append(client_time, client_at) ||
            !Append(server_time, server_at) || !Append(latency, result.server_hello_latency_ns) ||
            !Append(sni, client ? client->sni : std::nullopt) || !Append(alpn, alpn_json) ||
            !Append(offered13, offered_tls13) || !Append(version, selected_version) || !Append(cipher_suite, cipher) ||
            !Append(selected_protocol, selected_alpn) || !Append(alert_level, result.alert_level) ||
            !Append(alert_description, result.alert_description))
            return ENOMEM;
        const std::array<arrow::ArrayBuilder*, 25> builders = {&entity_id,
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
                                                               &retry_count,
                                                               &incomplete_reason,
                                                               &direction,
                                                               &client_time,
                                                               &server_time,
                                                               &latency,
                                                               &sni,
                                                               &alpn,
                                                               &offered13,
                                                               &version,
                                                               &cipher_suite,
                                                               &selected_protocol,
                                                               &alert_level,
                                                               &alert_description};
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(builders.size());
        for (auto* builder : builders) {
            std::shared_ptr<arrow::Array> column;
            const auto status = builder->Finish(&column);
            if (!status.ok()) return status.IsOutOfMemory() ? ENOMEM : EIO;
            columns.push_back(std::move(column));
        }
        auto batch = arrow::RecordBatch::Make(NpmTlsHandshakeEntityDescriptorV1().schema, 1, std::move(columns));
        return emitter.Emit("tls_handshake", *batch);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

}  // namespace flowsql::npm
