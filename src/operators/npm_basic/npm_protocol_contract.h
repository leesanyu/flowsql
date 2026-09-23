// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTRACT_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTRACT_H_

#include "npm_analysis_contract.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace flowsql::npm {

enum class NpmInputKindV1 : uint8_t { kTcpPacket = 1, kUdpDatagram = 2, kControlPacket = 4 };
constexpr uint8_t kNpmInputMaskV1 = 7;

enum class NpmRevisionSemanticsV1 : uint8_t { kCumulative = 0, kEvent = 1 };

struct NpmEntityDescriptorV1 {
    std::string entity_id;
    std::string module_id;
    uint32_t schema_version = 1;
    std::shared_ptr<arrow::Schema> schema;
    NpmRevisionSemanticsV1 revision_semantics = NpmRevisionSemanticsV1::kCumulative;
    std::string identity_column = "entity_instance_id";
    std::string revision_column = "revision";
    std::string observed_at_column = "observed_at";
    std::string is_final_column = "is_final";
};

/** Owned, immutable after task Open. No provider or configuration-plane handles are retained. */
struct NpmModulePlanV1 {
    std::string module_id;
    uint8_t input_mask = 0;
    bool requires_labeling = false;
    bool requires_tcp_stream = false;
    std::optional<std::vector<uint32_t>> primary_label_ids;
    std::vector<NpmEntityDescriptorV1> entities;
};

/** Borrowed only during OnInput; transport/session are null for a control packet. */
struct NpmInputEventV1 {
    NpmInputKindV1 kind = NpmInputKindV1::kTcpPacket;
    uint64_t observation_domain_id = 0;
    packet::PacketView packet;
    const packet::PacketLayerInfo* layer = nullptr;
    Span<const uint8_t> body;
    bool body_complete = true;
    const NpmPacketView* transport = nullptr;
    const NpmSessionView* session = nullptr;
};

struct NpmModuleTimeV1 {
    std::optional<int64_t> watermark_ns;
    int64_t observed_at_ns = 0;
};

struct NpmMaintenancePlanV1 {
    std::optional<int64_t> event_deadline_ns;
    std::optional<int64_t> snapshot_deadline_monotonic_ns;
};

/** Owned by the task. run_id identifies this Open, including across repeated executions. */
struct NpmResultContextV1 {
    std::string task_id;
    std::string run_id;
};

/** Capability facts resolved once by Open; false never causes implicit feature enablement. */
struct NpmModuleCapabilitiesV1 {
    bool labeling_enabled = false;
    bool labeling_available = false;
    bool tcp_stream_available = false;
    std::vector<uint32_t> available_label_ids;
};

enum class NpmProtocolContractErrorV1 : uint8_t {
    kNone = 0,
    kInvalidInput,
    kInvalidPlan,
    kUnavailableCapability,
    kInvalidLabelSelection,
    kInvalidEntity,
    kInvalidSchema,
    kWrongModule,
    kInvalidRows,
};

struct NpmProtocolContractStatusV1 {
    NpmProtocolContractErrorV1 error = NpmProtocolContractErrorV1::kNone;
    std::string field;
    int64_t row = -1;
};

/** Checks borrowed-view relationships only; the unique input binding owns wire/header validation. */
NpmProtocolContractStatusV1 ValidateNpmInputEventV1(const NpmInputEventV1& event);
NpmProtocolContractStatusV1 ValidateNpmEntityDescriptorV1(const NpmEntityDescriptorV1& entity);
NpmProtocolContractStatusV1 ValidateNpmModulePlanV1(const NpmModulePlanV1& plan,
                                                    const NpmModuleCapabilitiesV1& capabilities);
/** Stateless row validation. Module-owned lifecycle state enforces cross-emission revision/finality. */
NpmProtocolContractStatusV1 ValidateNpmEntityRowsV1(std::string_view emitting_module,
                                                    const NpmEntityDescriptorV1& entity,
                                                    const arrow::RecordBatch& rows);
NpmEntityDescriptorV1 NpmBasicEntityDescriptorV1(bool labeling_enabled = false);
NpmEntityDescriptorV1 NpmSessionEntityDescriptorV1(bool labeling_enabled = false);

interface INpmResultEmitterV1 {
    virtual ~INpmResultEmitterV1() = default;
    /** Synchronously consumes or copies the borrowed rows before returning; nonzero aborts the task. */
    virtual int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) = 0;
};

interface INpmProtocolModuleV1 {
    virtual ~INpmProtocolModuleV1() = default;
    virtual int OnInput(const NpmInputEventV1&, INpmResultEmitterV1&) = 0;
    virtual int OnSessionSnapshot(const NpmSessionView&, int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    virtual int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t observed_at_ns,
                             INpmResultEmitterV1&) = 0;
    virtual std::optional<int64_t> NextEventDeadlineNs() const = 0;
    virtual int OnTime(const NpmModuleTimeV1&, INpmResultEmitterV1&) = 0;
    virtual int Finish(int64_t observed_at_ns, INpmResultEmitterV1&) = 0;
    /** Idempotent, emits no results, and is called only after in-flight module callbacks finish. */
    virtual void Abort() noexcept = 0;
};

interface INpmResultConsumerV1 {
    virtual ~INpmResultConsumerV1() = default;
    virtual int Consume(const NpmResultContextV1&, const NpmEntityDescriptorV1&, const arrow::RecordBatch& rows) = 0;
    virtual int Finish() = 0;
    /** Concurrent, nonblocking cancellation signal; destruction waits for in-flight calls to exit. */
    virtual void Cancel() noexcept = 0;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTRACT_H_
