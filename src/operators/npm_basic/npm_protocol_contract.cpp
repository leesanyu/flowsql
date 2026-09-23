// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_protocol_contract.h"

#include <arrow/api.h>

#include <algorithm>
#include <array>
#include <utility>

namespace flowsql::npm {
namespace {

using Error = NpmProtocolContractErrorV1;

NpmProtocolContractStatusV1 Fail(Error error, std::string field, int64_t row = -1) {
    return {error, std::move(field), row};
}

bool Contains(Span<const uint8_t> bytes, Span<const uint8_t> view) {
    if (view.size == 0 && view.data == nullptr) return true;
    if (bytes.data == nullptr || view.data == nullptr) return false;
    const auto begin = reinterpret_cast<uintptr_t>(bytes.data);
    const auto position = reinterpret_cast<uintptr_t>(view.data);
    return position >= begin && position - begin <= bytes.size && view.size <= bytes.size - (position - begin);
}

bool SameSpan(Span<const uint8_t> a, Span<const uint8_t> b) { return a.data == b.data && a.size == b.size; }

std::array<std::string_view, 4> RequiredColumns(const NpmEntityDescriptorV1& entity) {
    return {entity.identity_column, entity.revision_column, entity.observed_at_column, entity.is_final_column};
}

}  // namespace

NpmProtocolContractStatusV1 ValidateNpmInputEventV1(const NpmInputEventV1& event) {
    if (event.layer == nullptr) return Fail(Error::kInvalidInput, "layer");
    if ((event.packet.bytes.size != 0 && event.packet.bytes.data == nullptr) ||
        event.packet.meta.captured_len != event.packet.bytes.size) {
        return Fail(Error::kInvalidInput, "packet");
    }
    if (!Contains(event.packet.bytes, event.body)) return Fail(Error::kInvalidInput, "body");
    if (event.kind == NpmInputKindV1::kControlPacket) {
        if (event.transport != nullptr || event.session != nullptr) return Fail(Error::kInvalidInput, "session");
        if (event.layer->transport_protocol != 1 && event.layer->transport_protocol != 58) {
            return Fail(Error::kInvalidInput, "kind");
        }
        return {};
    }
    const bool tcp = event.kind == NpmInputKindV1::kTcpPacket;
    if (!tcp && event.kind != NpmInputKindV1::kUdpDatagram) return Fail(Error::kInvalidInput, "kind");
    if (event.transport == nullptr || event.session == nullptr || event.session->key == nullptr ||
        event.session->session_id == 0) {
        return Fail(Error::kInvalidInput, "session");
    }
    const auto& transport = *event.transport;
    const auto& facts = transport.transport;
    const uint8_t protocol = tcp ? 6 : 17;
    if (event.session->key->transport_protocol != protocol || event.layer->transport_protocol != protocol ||
        facts.tcp.valid != tcp) {
        return Fail(Error::kInvalidInput, "kind");
    }
    if (event.session->key->observation_domain_id != event.observation_domain_id) {
        return Fail(Error::kInvalidInput, "observation_domain_id");
    }
    if (transport.direction != NpmPacketDirection::kAToB && transport.direction != NpmPacketDirection::kBToA) {
        return Fail(Error::kInvalidInput, "direction");
    }
    const auto& meta = event.packet.meta;
    const auto& bound = transport.packet.meta;
    if (transport.layer != event.layer || !SameSpan(event.packet.bytes, transport.packet.bytes) ||
        meta.timestamp_ns != bound.timestamp_ns || meta.source_id != bound.source_id ||
        meta.sequence != bound.sequence || meta.captured_len != bound.captured_len || meta.wire_len != bound.wire_len ||
        meta.link_type != bound.link_type) {
        return Fail(Error::kInvalidInput, "transport");
    }
    if (!SameSpan(event.body, transport.payload) || facts.payload_captured_bytes != event.body.size ||
        facts.payload_captured_bytes > facts.payload_wire_bytes || event.body_complete != facts.payload_complete ||
        facts.payload_complete != (facts.payload_captured_bytes == facts.payload_wire_bytes)) {
        return Fail(Error::kInvalidInput, "body");
    }
    return {};
}

NpmProtocolContractStatusV1 ValidateNpmEntityDescriptorV1(const NpmEntityDescriptorV1& entity) {
    if (entity.entity_id.empty()) return Fail(Error::kInvalidEntity, "entity_id");
    if (entity.module_id.empty()) return Fail(Error::kInvalidEntity, "module_id");
    if (entity.schema_version == 0) return Fail(Error::kInvalidEntity, "schema_version");
    if (entity.revision_semantics != NpmRevisionSemanticsV1::kCumulative &&
        entity.revision_semantics != NpmRevisionSemanticsV1::kEvent) {
        return Fail(Error::kInvalidEntity, "revision_semantics");
    }
    if (!entity.schema) return Fail(Error::kInvalidSchema, "schema");
    const auto columns = RequiredColumns(entity);
    const std::array<arrow::Type::type, 4> types = {arrow::Type::UINT64, arrow::Type::UINT64, arrow::Type::INT64,
                                                    arrow::Type::BOOL};
    for (size_t index = 0; index < columns.size(); ++index) {
        const std::string name(columns[index]);
        if (name.empty() ||
            std::find(columns.begin(), columns.begin() + index, columns[index]) != columns.begin() + index) {
            return Fail(Error::kInvalidSchema, "column_mapping");
        }
        const auto matches = entity.schema->GetAllFieldIndices(name);
        if (matches.size() != 1) return Fail(Error::kInvalidSchema, name);
        const auto& field = entity.schema->field(matches.front());
        if (field->nullable() || field->type()->id() != types[index]) return Fail(Error::kInvalidSchema, name);
    }
    return {};
}

NpmProtocolContractStatusV1 ValidateNpmModulePlanV1(const NpmModulePlanV1& plan,
                                                    const NpmModuleCapabilitiesV1& capabilities) {
    if (plan.module_id.empty()) return Fail(Error::kInvalidPlan, "module_id");
    if (plan.input_mask == 0 || (plan.input_mask & ~kNpmInputMaskV1) != 0) {
        return Fail(Error::kInvalidPlan, "input_mask");
    }
    if (plan.primary_label_ids && !plan.requires_labeling) return Fail(Error::kInvalidPlan, "requires_labeling");
    if ((plan.input_mask & static_cast<uint8_t>(NpmInputKindV1::kControlPacket)) &&
        (plan.requires_labeling || plan.primary_label_ids)) {
        return Fail(Error::kInvalidPlan, "input_mask");
    }
    if (plan.requires_tcp_stream && (!(plan.input_mask & static_cast<uint8_t>(NpmInputKindV1::kTcpPacket)) ||
                                     !plan.requires_labeling || !plan.primary_label_ids)) {
        return Fail(Error::kInvalidPlan, "requires_tcp_stream");
    }
    if (plan.requires_labeling && (!capabilities.labeling_enabled || !capabilities.labeling_available)) {
        return Fail(Error::kUnavailableCapability, "labeling");
    }
    if (plan.primary_label_ids) {
        const auto& labels = *plan.primary_label_ids;
        if (labels.empty()) return Fail(Error::kInvalidLabelSelection, "primary_label_ids");
        auto sorted = labels;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.front() == 0 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return Fail(Error::kInvalidLabelSelection, "primary_label_ids");
        }
        auto available = capabilities.available_label_ids;
        std::sort(available.begin(), available.end());
        for (uint32_t label : sorted) {
            if (!std::binary_search(available.begin(), available.end(), label)) {
                return Fail(Error::kInvalidLabelSelection, "primary_label_ids");
            }
        }
    }
    if (plan.requires_tcp_stream && !capabilities.tcp_stream_available) {
        return Fail(Error::kUnavailableCapability, "tcp_stream");
    }
    std::vector<std::string_view> entity_ids;
    for (const auto& entity : plan.entities) {
        const auto status = ValidateNpmEntityDescriptorV1(entity);
        if (status.error != Error::kNone) return status;
        if (entity.module_id != plan.module_id) return Fail(Error::kWrongModule, entity.entity_id);
        entity_ids.push_back(entity.entity_id);
    }
    std::sort(entity_ids.begin(), entity_ids.end());
    if (std::adjacent_find(entity_ids.begin(), entity_ids.end()) != entity_ids.end()) {
        return Fail(Error::kInvalidEntity, "entity_id");
    }
    return {};
}

NpmProtocolContractStatusV1 ValidateNpmEntityRowsV1(std::string_view emitting_module,
                                                    const NpmEntityDescriptorV1& entity,
                                                    const arrow::RecordBatch& rows) {
    const auto status = ValidateNpmEntityDescriptorV1(entity);
    if (status.error != Error::kNone) return status;
    if (emitting_module != entity.module_id) return Fail(Error::kWrongModule, "module_id");
    if (!rows.schema() || !rows.schema()->Equals(*entity.schema, true)) return Fail(Error::kInvalidSchema, "schema");
    if (!rows.ValidateFull().ok()) return Fail(Error::kInvalidRows, "rows");
    // ValidateFull validates arrays, but does not enforce the schema's field nullability.
    for (int index = 0; index < rows.num_columns(); ++index) {
        if (!entity.schema->field(index)->nullable() && rows.column(index)->null_count() != 0) {
            return Fail(Error::kInvalidRows, entity.schema->field(index)->name());
        }
    }
    const auto& ids = static_cast<const arrow::UInt64Array&>(*rows.GetColumnByName(entity.identity_column));
    const auto& revisions = static_cast<const arrow::UInt64Array&>(*rows.GetColumnByName(entity.revision_column));
    const auto& finals = static_cast<const arrow::BooleanArray&>(*rows.GetColumnByName(entity.is_final_column));
    for (int64_t row = 0; row < rows.num_rows(); ++row) {
        if (ids.Value(row) == 0) return Fail(Error::kInvalidRows, entity.identity_column, row);
        if (revisions.Value(row) == 0 ||
            (entity.revision_semantics == NpmRevisionSemanticsV1::kEvent && revisions.Value(row) != 1)) {
            return Fail(Error::kInvalidRows, entity.revision_column, row);
        }
        if (entity.revision_semantics == NpmRevisionSemanticsV1::kEvent && !finals.Value(row)) {
            return Fail(Error::kInvalidRows, entity.is_final_column, row);
        }
    }
    return {};
}

NpmEntityDescriptorV1 NpmBasicEntityDescriptorV1(bool labeling_enabled) {
    NpmEntityDescriptorV1 entity;
    entity.entity_id = "basic";
    entity.module_id = "basic";
    entity.schema = NpmBasicResultSchema(labeling_enabled);
    entity.identity_column = "session_id";
    return entity;
}

NpmEntityDescriptorV1 NpmSessionEntityDescriptorV1(bool labeling_enabled) {
    NpmEntityDescriptorV1 entity;
    entity.entity_id = "session";
    entity.module_id = "session";
    entity.schema = NpmSessionResultSchema(labeling_enabled);
    entity.identity_column = "session_id";
    return entity;
}

}  // namespace flowsql::npm
