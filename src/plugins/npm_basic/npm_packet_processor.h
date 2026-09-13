// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_PROCESSOR_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_PROCESSOR_H_

#include "npm_packet_batch_view.h"
#include "npm_session_table.h"

#include <cstdint>
#include <vector>

namespace flowsql::npm {

enum class NpmPacketProcessError : uint8_t {
    kNone = 0,
    kNullOutput,
    kNullModule,
    kBindingError,
    kSessionError,
    kModuleError,
};

struct NpmPacketProcessStatus {
    NpmPacketProcessError error = NpmPacketProcessError::kNone;
    NpmSessionPacketError binding_error = NpmSessionPacketError::kNone;
    NpmSessionTableError session_error = NpmSessionTableError::kNone;
    int module_error = 0;
};

/** Processes one decoded packet. Ended-session output is replaced only after every callback succeeds. */
NpmPacketProcessStatus ProcessNpmPacket(
    const NpmObservationDomainMap& domain_map,
    const packet::PacketView& packet,
    const packet::PacketLayerInfo& layer,
    NpmSessionTable& sessions,
    packet::IPacketProtocolIdentifier& identifier,
    const std::vector<INpmAnalysisModule*>& modules,
    INpmResultWriter& writer,
    std::vector<NpmSessionSnapshot>* ended_sessions);

struct NpmSessionEndEvent {
    NpmSessionSnapshot snapshot;
    int64_t observed_at = 0;
};

enum class NpmPacketBatchProcessError : uint8_t {
    kNone = 0,
    kNullOutput,
    kNullModule,
    kBatchViewError,
    kPacketError,
    kProgressDeferred,
    kModuleError,
    kAllocationFailed,
};

struct NpmPacketBatchProcessStatus {
    NpmPacketBatchProcessError error = NpmPacketBatchProcessError::kNone;
    int64_t row = -1;
    NpmPacketBatchError batch_error = NpmPacketBatchError::kNone;
    NpmPacketProcessStatus packet_status;
    NpmCaptureProgressDisposition progress_disposition = NpmCaptureProgressDisposition::kUnchanged;
    int module_error = 0;
};

/** Processes a validated packet batch and advances offline event time after each successful row. */
NpmPacketBatchProcessStatus ProcessNpmOfflinePacketBatch(
    const NpmObservationDomainMap& domain_map,
    const NpmPacketBatchView& batch,
    NpmSessionTable& sessions,
    packet::IPacketProtocolIdentifier& identifier,
    const std::vector<INpmAnalysisModule*>& modules,
    INpmResultWriter& writer,
    std::vector<NpmSessionEndEvent>* ended_events);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_PACKET_PROCESSOR_H_
