// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_packet_processor.h"

#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

bool IsOfflineProgressDisposition(NpmCaptureProgressDisposition disposition) {
    return disposition == NpmCaptureProgressDisposition::kAdvanced ||
           disposition == NpmCaptureProgressDisposition::kUnchanged;
}

void AppendSessionEndEvents(std::vector<NpmSessionSnapshot>* snapshots,
                            int64_t observed_at,
                            std::vector<NpmSessionEndEvent>* events) {
    for (auto& snapshot : *snapshots) {
        events->push_back(NpmSessionEndEvent{std::move(snapshot), observed_at});
    }
}

int NotifySessionEndRange(const std::vector<NpmSessionSnapshot>& ended_sessions,
                          size_t begin,
                          size_t end,
                          const std::vector<INpmAnalysisModule*>& modules,
                          int64_t observed_at_ns,
                          INpmResultWriter& writer) {
    for (size_t index = begin; index < end; ++index) {
        const auto view = ended_sessions[index].View();
        for (auto* module : modules) {
            const int error = module->OnSessionEnd(
                view, ended_sessions[index].end_reason, observed_at_ns, writer);
            if (error != 0) return error;
        }
    }
    return 0;
}

NpmSessionTableError SampleTerminalProtocol(bool protocol_was_pending,
                                            const NpmPacketView& packet,
                                            packet::IPacketProtocolIdentifier& identifier,
                                            NpmSessionSnapshot* snapshot) {
    if (!protocol_was_pending || packet.payload.empty()) return NpmSessionTableError::kNone;
    if (packet.payload.data == nullptr || packet.layer == nullptr) {
        return NpmSessionTableError::kInvalidPacketView;
    }

    const auto identified = identifier.Identify(packet.packet, *packet.layer);
    if (identified.status == packet::ProtocolStatus::kIdentified && identified.id != 0) {
        snapshot->protocol_status = NpmProtocolStatus::kIdentified;
        snapshot->protocol_id = identified.id;
        if (identified.sub_id != 0) {
            snapshot->protocol_sub_id = identified.sub_id;
        } else {
            snapshot->protocol_sub_id.reset();
        }
    } else {
        snapshot->protocol_status = NpmProtocolStatus::kUnknown;
        snapshot->protocol_id.reset();
        snapshot->protocol_sub_id.reset();
    }
    return NpmSessionTableError::kNone;
}

}  // namespace

NpmPacketProcessStatus ProcessNpmPacket(
    const NpmObservationDomainMap& domain_map,
    const packet::PacketView& packet,
    const packet::PacketLayerInfo& layer,
    NpmSessionTable& sessions,
    packet::IPacketProtocolIdentifier& identifier,
    const std::vector<INpmAnalysisModule*>& modules,
    INpmResultWriter& writer,
    std::vector<NpmSessionSnapshot>* ended_sessions) {
    NpmPacketProcessStatus status;
    if (ended_sessions == nullptr) {
        status.error = NpmPacketProcessError::kNullOutput;
        return status;
    }
    for (auto* module : modules) {
        if (module == nullptr) {
            status.error = NpmPacketProcessError::kNullModule;
            return status;
        }
    }

    NpmSessionPacketBinding binding;
    status.binding_error = BuildNpmSessionPacketBinding(domain_map, packet, layer, &binding);
    if (status.binding_error != NpmSessionPacketError::kNone) {
        status.error = NpmPacketProcessError::kBindingError;
        return status;
    }

    NpmPacketView npm_packet;
    npm_packet.packet = packet;
    npm_packet.layer = &layer;
    npm_packet.payload = binding.payload;
    npm_packet.direction = binding.direction;
    npm_packet.transport = binding.transport;

    bool had_existing_session = false;
    uint64_t existing_session_id = 0;
    NpmProtocolStatus existing_protocol_status = NpmProtocolStatus::kPending;
    const bool may_close_session =
        binding.transport.tcp.valid && (binding.transport.tcp.rst || binding.transport.tcp.fin);
    if (may_close_session) {
        NpmSessionView existing;
        const auto find_error = sessions.Find(binding.key, &existing);
        if (find_error == NpmSessionTableError::kNone) {
            had_existing_session = true;
            existing_session_id = existing.session_id;
            existing_protocol_status = existing.protocol_status;
        } else if (find_error != NpmSessionTableError::kNotFound) {
            status.session_error = find_error;
            status.error = NpmPacketProcessError::kSessionError;
            return status;
        }
    }

    NpmSessionObserveResult observed;
    status.session_error = sessions.Observe(binding, packet.meta, &observed);
    if (status.session_error != NpmSessionTableError::kNone) {
        status.error = NpmPacketProcessError::kSessionError;
        return status;
    }

    if (observed.has_active_session) {
        status.module_error = NotifySessionEndRange(observed.ended_sessions,
                                                    0,
                                                    observed.ended_sessions.size(),
                                                    modules,
                                                    packet.meta.timestamp_ns,
                                                    writer);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }

        NpmSessionView sampled;
        status.session_error = sessions.SampleProtocol(
            binding.key, observed.active_session.session_id, npm_packet, identifier, &sampled);
        if (status.session_error != NpmSessionTableError::kNone) {
            status.error = NpmPacketProcessError::kSessionError;
            return status;
        }

        for (auto* module : modules) {
            status.module_error = module->OnPacket(npm_packet, sampled, writer);
            if (status.module_error != 0) {
                status.error = NpmPacketProcessError::kModuleError;
                return status;
            }
        }
    } else if (!observed.ended_sessions.empty()) {
        const size_t current_session_index = observed.ended_sessions.size() - 1;
        auto& current_snapshot = observed.ended_sessions[current_session_index];
        const bool current_session_started_pending =
            !had_existing_session || current_snapshot.session_id != existing_session_id ||
            existing_protocol_status == NpmProtocolStatus::kPending;
        status.module_error = NotifySessionEndRange(observed.ended_sessions,
                                                    0,
                                                    current_session_index,
                                                    modules,
                                                    packet.meta.timestamp_ns,
                                                    writer);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }

        status.session_error = SampleTerminalProtocol(
            current_session_started_pending, npm_packet, identifier, &current_snapshot);
        if (status.session_error != NpmSessionTableError::kNone) {
            status.error = NpmPacketProcessError::kSessionError;
            return status;
        }

        const auto current_session = observed.ended_sessions[current_session_index].View();
        for (auto* module : modules) {
            status.module_error = module->OnPacket(npm_packet, current_session, writer);
            if (status.module_error != 0) {
                status.error = NpmPacketProcessError::kModuleError;
                return status;
            }
        }

        status.module_error = NotifySessionEndRange(observed.ended_sessions,
                                                    current_session_index,
                                                    observed.ended_sessions.size(),
                                                    modules,
                                                    packet.meta.timestamp_ns,
                                                    writer);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }
    }

    *ended_sessions = std::move(observed.ended_sessions);
    return status;
}

NpmPacketBatchProcessStatus ProcessNpmOfflinePacketBatch(
    const NpmObservationDomainMap& domain_map,
    const NpmPacketBatchView& batch,
    NpmSessionTable& sessions,
    packet::IPacketProtocolIdentifier& identifier,
    const std::vector<INpmAnalysisModule*>& modules,
    INpmResultWriter& writer,
    std::vector<NpmSessionEndEvent>* ended_events) {
    NpmPacketBatchProcessStatus status;
    if (ended_events == nullptr) {
        status.error = NpmPacketBatchProcessError::kNullOutput;
        return status;
    }
    for (auto* module : modules) {
        if (module == nullptr) {
            status.error = NpmPacketBatchProcessError::kNullModule;
            return status;
        }
    }

    int64_t current_row = -1;
    try {
        std::vector<NpmSessionEndEvent> next_events;
        for (current_row = 0; current_row < batch.num_rows(); ++current_row) {
            packet::PacketView packet;
            packet::PacketLayerInfo layer;
            status.batch_error = batch.Get(current_row, &packet, &layer);
            if (status.batch_error != NpmPacketBatchError::kNone) {
                status.error = NpmPacketBatchProcessError::kBatchViewError;
                status.row = current_row;
                return status;
            }

            std::vector<NpmSessionSnapshot> observed_sessions;
            status.packet_status = ProcessNpmPacket(
                domain_map, packet, layer, sessions, identifier, modules, writer, &observed_sessions);
            if (status.packet_status.error != NpmPacketProcessError::kNone) {
                status.error = NpmPacketBatchProcessError::kPacketError;
                status.row = current_row;
                return status;
            }
            AppendSessionEndEvents(&observed_sessions, packet.meta.timestamp_ns, &next_events);

            NpmCaptureProgressUpdate update;
            update.capture_time_ns = packet.meta.timestamp_ns;
            update.packet_observed = true;
            auto progress = sessions.AdvanceCaptureProgress(update);
            status.progress_disposition = progress.disposition;
            if (!IsOfflineProgressDisposition(progress.disposition)) {
                status.error = NpmPacketBatchProcessError::kProgressDeferred;
                status.row = current_row;
                return status;
            }

            status.module_error = NotifyNpmSessionEnd(
                progress.ended_sessions, modules, packet.meta.timestamp_ns, writer);
            if (status.module_error != 0) {
                status.error = NpmPacketBatchProcessError::kModuleError;
                status.row = current_row;
                return status;
            }
            AppendSessionEndEvents(&progress.ended_sessions, packet.meta.timestamp_ns, &next_events);
        }

        *ended_events = std::move(next_events);
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmPacketBatchProcessError::kAllocationFailed;
        status.row = current_row;
        return status;
    }
}

}  // namespace flowsql::npm
