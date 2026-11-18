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

enum class NpmInputKind : uint8_t { kTcpPacket = 1, kUdpDatagram = 2, kControlPacket = 4 };
constexpr uint8_t kNpmInputMask = 7;

enum class NpmRevisionSemantics : uint8_t { kCumulative = 0, kEvent = 1 };

struct NpmEntityDescriptor {
    std::string entity_id;
    std::string module_id;
    uint32_t schema_version = 1;
    std::shared_ptr<arrow::Schema> schema;
    NpmRevisionSemantics revision_semantics = NpmRevisionSemantics::kCumulative;
    std::string identity_column = "entity_instance_id";
    std::string revision_column = "revision";
    std::string observed_at_column = "observed_at";
    std::string is_final_column = "is_final";
};

/** Owned, immutable after task Open. No provider or configuration-plane handles are retained. */
struct NpmModulePlan {
    std::string module_id;
    uint8_t input_mask = 0;
    bool requires_labeling = false;
    bool requires_tcp_stream = false;
    std::optional<std::vector<uint32_t>> primary_label_ids;
    std::vector<NpmEntityDescriptor> entities;
};

/** Borrowed only during OnInput; transport/session are null for a control packet. */
struct NpmInputEvent {
    NpmInputKind kind = NpmInputKind::kTcpPacket;
    uint64_t observation_domain_id = 0;
    packet::PacketView packet;
    const packet::PacketFamilyLayerView* layer = nullptr;
    Span<const uint8_t> body;
    bool body_complete = true;
    const NpmPacketView* transport = nullptr;
    const NpmSessionView* session = nullptr;
};

struct NpmModuleTime {
    std::optional<int64_t> watermark_ns;
    int64_t observed_at_ns = 0;
};

struct NpmMaintenancePlan {
    std::optional<int64_t> event_deadline_ns;
    std::optional<int64_t> snapshot_deadline_monotonic_ns;
};

/** Owned by the task. run_id identifies this Open, including across repeated executions. */
struct NpmResultContext {
    std::string task_id;
    std::string run_id;
};

/** Capability facts resolved once by Open; false never causes implicit feature enablement. */
struct NpmModuleCapabilities {
    bool labeling_enabled = false;
    bool labeling_available = false;
    bool tcp_stream_available = false;
    std::vector<uint32_t> available_label_ids;
};

enum class NpmProtocolContractError : uint8_t {
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

struct NpmProtocolContractStatus {
    NpmProtocolContractError error = NpmProtocolContractError::kNone;
    std::string field;
    int64_t row = -1;
};

/** Checks borrowed-view relationships only; the unique input binding owns wire/header validation. */
NpmProtocolContractStatus ValidateNpmInputEvent(const NpmInputEvent& event);
NpmProtocolContractStatus ValidateNpmEntityDescriptor(const NpmEntityDescriptor& entity);
NpmProtocolContractStatus ValidateNpmModulePlan(const NpmModulePlan& plan, const NpmModuleCapabilities& capabilities);
/** Stateless row validation. Module-owned lifecycle state enforces cross-emission revision/finality. */
NpmProtocolContractStatus ValidateNpmEntityRows(std::string_view emitting_module, const NpmEntityDescriptor& entity,
                                                const arrow::RecordBatch& rows);
NpmEntityDescriptor NpmBasicEntityDescriptor(packet::AddressFamily family);
NpmEntityDescriptor NpmSessionEntityDescriptor(packet::AddressFamily family);

interface INpmResultEmitter {
    virtual ~INpmResultEmitter() = default;
    /** Synchronously consumes or copies the borrowed rows before returning; nonzero aborts the task. */
    virtual int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) = 0;
};

interface INpmProtocolModule {
    virtual ~INpmProtocolModule() = default;
    virtual int OnInput(const NpmInputEvent&, INpmResultEmitter&) = 0;
    virtual int OnSessionSnapshot(const NpmSessionView&, int64_t observed_at_ns, INpmResultEmitter&) = 0;
    virtual int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t observed_at_ns,
                             INpmResultEmitter&) = 0;
    virtual std::optional<int64_t> NextEventDeadlineNs() const = 0;
    virtual int OnTime(const NpmModuleTime&, INpmResultEmitter&) = 0;
    virtual int Finish(int64_t observed_at_ns, INpmResultEmitter&) = 0;
    /** Idempotent, emits no results, and is called only after in-flight module callbacks finish. */
    virtual void Abort() noexcept = 0;
};

interface INpmResultConsumer {
    virtual ~INpmResultConsumer() = default;
    virtual int Consume(const NpmResultContext&, const NpmEntityDescriptor&, const arrow::RecordBatch& rows) = 0;
    virtual int Finish() = 0;
    /** Concurrent, nonblocking cancellation signal; destruction waits for in-flight calls to exit. */
    virtual void Cancel() noexcept = 0;
};

/** Borrowed only for the FailRun call. A failure must not hide the original task error. */
struct NpmResultFailure {
    int32_t code = 0;
    const char* stage = nullptr;
    const char* message = nullptr;
};

/** Managed run lifecycle and summary, in addition to the existing synchronous consumer contract. */
interface INpmManagedResultConsumer : public INpmResultConsumer {
    virtual int FailRun(const NpmResultFailure& failure) = 0;
    virtual std::string ResultJson() const = 0;
};

/** Task-private factory; Create validates all enabled entities before runtime publication. */
interface INpmResultConsumerFactory {
    virtual ~INpmResultConsumerFactory() = default;
    virtual int Create(const NpmResultContext& context, const std::vector<NpmEntityDescriptor>& entities,
                       std::shared_ptr<INpmTaskBudget> budget,
                       std::unique_ptr<INpmManagedResultConsumer>* consumer) = 0;
    virtual std::string LastError() const = 0;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTRACT_H_
