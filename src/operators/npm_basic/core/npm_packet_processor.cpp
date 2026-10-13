// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_packet_processor.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

class PacketScratchBudget {
 public:
    explicit PacketScratchBudget(INpmTaskBudget& budget) : budget_(budget) {}
    ~PacketScratchBudget() {
        if (bytes_) budget_.Release(NpmBudgetCategory::kInputBatch, bytes_);
    }
    bool Reserve(uint64_t bytes) {
        if (budget_.Reserve(NpmBudgetCategory::kInputBatch, bytes) != NpmBudgetError::kNone) return false;
        bytes_ += bytes;
        return true;
    }

 private:
    INpmTaskBudget& budget_;
    uint64_t bytes_ = 0;
};
template <typename Visitor>
NpmSessionPacketError WithPacketBinding(const NpmObservationDomainLookup& domains, const packet::PacketView& packet,
                                        const packet::PacketFamilyLayerView& layer, Visitor&& visitor) {
    const auto bind = [&](auto key) {
        NpmPacketBindingView binding;
        const auto error = BuildNpmSessionPacketBinding(domains, packet, layer, &key, &binding);
        if (error == NpmSessionPacketError::kNone) visitor(binding);
        return error;
    };
    return layer.Get4() ? bind(NpmSessionKey4{}) : bind(NpmSessionKey6{});
}

bool IsOfflineProgressDisposition(NpmCaptureProgressDisposition disposition) {
    return disposition == NpmCaptureProgressDisposition::kAdvanced ||
           disposition == NpmCaptureProgressDisposition::kUnchanged;
}

int NotifySessionEndRange(const NpmSessionSnapshotBatch& ended_sessions, size_t begin, size_t end,
                          const std::vector<INpmAnalysisModule*>& modules, int64_t observed_at_ns,
                          INpmResultWriter& writer, NpmTcpStreamProvider* streams) {
    for (size_t index = begin; index < end; ++index) {
        if (streams && streams->OnSessionEnd(ended_sessions.View(index), ended_sessions.Row(index).facts.end_reason,
                                             observed_at_ns) != 0)
            return EIO;
        const auto view = ended_sessions.View(index);
        for (auto* module : modules) {
            const int error =
                module->OnSessionEnd(view, ended_sessions.Row(index).facts.end_reason, observed_at_ns, writer);
            if (error != 0) return error;
        }
    }
    return 0;
}

NpmSessionTableError SampleTerminalProtocol(bool protocol_was_pending, const NpmPacketView& packet,
                                            packet::IPacketFamilyProtocolIdentifier& identifier,
                                            NpmSessionSnapshotFacts* snapshot) {
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

NpmPacketProcessStatus ProcessNpmPacket(const NpmObservationDomainLookup& domain_map, const packet::PacketView& packet,
                                        const packet::PacketFamilyLayerView& layer, NpmSessionTable& sessions,
                                        packet::IPacketFamilyProtocolIdentifier& identifier,
                                        const std::vector<INpmAnalysisModule*>& modules, INpmResultWriter& writer,
                                        NpmSessionSnapshotBatch* ended_sessions) {
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

    NpmSessionSnapshotBatch next_ends;
    const auto error = WithPacketBinding(domain_map, packet, layer, [&](const NpmPacketBindingView& binding) {
        status = ProcessNpmBoundPacket(packet, layer, binding, 0, sessions, identifier, modules, writer, &next_ends);
    });
    if (error != NpmSessionPacketError::kNone) {
        status.binding_error = error;
        status.error = NpmPacketProcessError::kBindingError;
    }
    if (status.error == NpmPacketProcessError::kNone) *ended_sessions = std::move(next_ends);
    return status;
}

NpmPacketProcessStatus ProcessNpmBoundPacket(const packet::PacketView& packet,
                                             const packet::PacketFamilyLayerView& layer,
                                             const NpmPacketBindingView& binding, uint32_t new_session_primary_label_id,
                                             NpmSessionTable& sessions,
                                             packet::IPacketFamilyProtocolIdentifier& identifier,
                                             const std::vector<INpmAnalysisModule*>& modules, INpmResultWriter& writer,
                                             NpmSessionSnapshotBatch* ended_sessions, NpmTcpStreamProvider* streams) {
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
    status.session_error =
        sessions.Observe(binding, packet.meta, &observed, new_session_primary_label_id, ended_sessions);
    if (status.session_error != NpmSessionTableError::kNone) {
        status.error = NpmPacketProcessError::kSessionError;
        return status;
    }

    if (observed.has_active_session) {
        status.module_error = NotifySessionEndRange(*ended_sessions, observed.ended_begin, observed.ended_end, modules,
                                                    packet.meta.timestamp_ns, writer, streams);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }

        NpmSessionView sampled;
        status.session_error = sessions.SampleProtocol(std::move(observed.access), npm_packet, identifier, &sampled);
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
        if (streams && (status.module_error = streams->OnPacket(npm_packet, sampled)) != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }
    } else if (observed.ended_end != observed.ended_begin) {
        const size_t current_session_index = observed.ended_end - 1;
        auto& current_snapshot = ended_sessions->MutableFacts(current_session_index);
        const bool current_session_started_pending = !had_existing_session ||
                                                     current_snapshot.session_id != existing_session_id ||
                                                     existing_protocol_status == NpmProtocolStatus::kPending;
        status.module_error = NotifySessionEndRange(*ended_sessions, observed.ended_begin, current_session_index,
                                                    modules, packet.meta.timestamp_ns, writer, streams);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }

        status.session_error =
            SampleTerminalProtocol(current_session_started_pending, npm_packet, identifier, &current_snapshot);
        if (status.session_error != NpmSessionTableError::kNone) {
            status.error = NpmPacketProcessError::kSessionError;
            return status;
        }

        const auto current_session = ended_sessions->View(current_session_index);
        for (auto* module : modules) {
            status.module_error = module->OnPacket(npm_packet, current_session, writer);
            if (status.module_error != 0) {
                status.error = NpmPacketProcessError::kModuleError;
                return status;
            }
        }
        if (streams && (status.module_error = streams->OnPacket(npm_packet, current_session)) != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }

        status.module_error = NotifySessionEndRange(*ended_sessions, current_session_index, observed.ended_end, modules,
                                                    packet.meta.timestamp_ns, writer, streams);
        if (status.module_error != 0) {
            status.error = NpmPacketProcessError::kModuleError;
            return status;
        }
    }

    return status;
}

NpmPacketBatchProcessStatus ProcessNpmOfflinePacketBatch(
    const NpmObservationDomainLookup& domain_map, const NpmPacketBatchView& batch, NpmSessionTable& sessions,
    packet::IPacketFamilyProtocolIdentifier& identifier, const std::vector<INpmAnalysisModule*>& modules,
    INpmResultWriter& writer, NpmSessionSnapshotBatch* ended_events, const IFlowLabelMatcherV1* matcher,
    const std::vector<NpmProtocolModuleAdapter*>& protocol_modules, NpmTcpStreamProvider* streams,
    bool advance_offline_progress) {
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

    const bool control_enabled = std::any_of(protocol_modules.begin(), protocol_modules.end(),
                                             [](const auto* module) { return module->AcceptsControl(); });
    const auto is_control = [&](const packet::PacketFamilyLayerView& layer) {
        return control_enabled && !layer.ports_valid;
    };
    const auto dispatch_control = [&](const packet::PacketView& packet, packet::PacketFamilyLayerView layer) {
        NpmPacketProcessStatus result;
        NpmInputEventV1 event;
        result.binding_error = BuildNpmControlInput(domain_map, packet, layer, &event);
        if (result.binding_error != NpmSessionPacketError::kNone) {
            result.error = NpmPacketProcessError::kBindingError;
            return result;
        }
        if (sessions.IsLatePacket(packet.meta.timestamp_ns)) {
            result.error = NpmPacketProcessError::kSessionError;
            result.session_error = NpmSessionTableError::kLatePacket;
            return result;
        }
        for (auto* module : protocol_modules) {
            result.module_error = module->OnControl(event);
            if (result.module_error != 0) {
                result.error = NpmPacketProcessError::kModuleError;
                return result;
            }
        }
        return result;
    };
    int64_t current_row = -1;
    const auto advance_before_packet = [&](const packet::PacketView& packet, NpmSessionSnapshotBatch* events) {
        if (!advance_offline_progress) return true;

        NpmCaptureProgressUpdate update;
        update.capture_time_ns = packet.meta.timestamp_ns;
        update.packet_observed = true;
        auto progress = sessions.AdvanceCaptureProgress(update, events);
        status.progress_disposition = progress.disposition;
        if (!IsOfflineProgressDisposition(progress.disposition)) {
            status.error = NpmPacketBatchProcessError::kProgressDeferred;
            status.row = current_row;
            return false;
        }

        status.module_error = NotifySessionEndRange(*events, progress.ended_begin, progress.ended_end, modules,
                                                    packet.meta.timestamp_ns, writer, streams);
        if (status.module_error != 0) {
            status.error = NpmPacketBatchProcessError::kModuleError;
            status.row = current_row;
            return false;
        }
        if (progress.disposition == NpmCaptureProgressDisposition::kAdvanced) {
            if (streams && (status.module_error = streams->AdvanceWatermark(progress.watermark_ns)) != 0) {
                status.error = NpmPacketBatchProcessError::kModuleError;
                status.row = current_row;
                return false;
            }
            for (auto* module : modules) {
                status.module_error = module->OnTime(progress.watermark_ns, packet.meta.timestamp_ns, writer);
                if (status.module_error != 0) {
                    status.error = NpmPacketBatchProcessError::kModuleError;
                    status.row = current_row;
                    return false;
                }
            }
            const NpmModuleTimeV1 time{{progress.watermark_ns}, packet.meta.timestamp_ns};
            for (auto* module : protocol_modules) {
                status.module_error = module->OnTime(time);
                if (status.module_error != 0) {
                    status.error = NpmPacketBatchProcessError::kModuleError;
                    status.row = current_row;
                    return false;
                }
            }
        }

        return true;
    };
    const auto drain_packet_ends = [&](int64_t observed_at) {
        if (!advance_offline_progress) return true;
        // A FIN/RST or tuple reuse may close a state at an already committed watermark.
        for (auto* module : modules) {
            status.module_error = module->OnTime(sessions.WatermarkNs(), observed_at, writer);
            if (status.module_error != 0) {
                status.error = NpmPacketBatchProcessError::kModuleError;
                status.row = current_row;
                return false;
            }
        }
        return true;
    };
    try {
        if (matcher == nullptr) {
            NpmSessionSnapshotBatch next_events;
            for (current_row = 0; current_row < batch.num_rows(); ++current_row) {
                packet::PacketView packet;
                packet::PacketFamilyLayerView layer;
                status.batch_error = batch.Get(current_row, &packet, &layer);
                if (status.batch_error != NpmPacketBatchError::kNone) {
                    status.error = NpmPacketBatchProcessError::kBatchViewError;
                    status.row = current_row;
                    return status;
                }

                const auto process = [&](const NpmPacketBindingView& binding) {
                    if (!advance_before_packet(packet, &next_events)) return;
                    const size_t ends_before_packet = next_events.size();
                    status.packet_status = is_control(layer)
                                               ? dispatch_control(packet, layer)
                                               : ProcessNpmBoundPacket(packet, layer, binding, 0, sessions, identifier,
                                                                       modules, writer, &next_events, streams);
                    if (status.packet_status.error != NpmPacketProcessError::kNone) {
                        status.error = NpmPacketBatchProcessError::kPacketError;
                        status.row = current_row;
                        return;
                    }
                    if (next_events.size() != ends_before_packet) drain_packet_ends(packet.meta.timestamp_ns);
                };
                NpmSessionPacketError binding_error;
                if (is_control(layer)) {
                    NpmInputEventV1 control_event;
                    binding_error = BuildNpmControlInput(domain_map, packet, layer, &control_event);
                    if (binding_error == NpmSessionPacketError::kNone) process(NpmPacketBindingView{});
                } else {
                    // The concrete stack key owns the binding through progress, Observe and SampleProtocol.
                    binding_error = WithPacketBinding(domain_map, packet, layer, process);
                }
                if (binding_error != NpmSessionPacketError::kNone) {
                    status.packet_status.binding_error = binding_error;
                    status.packet_status.error = NpmPacketProcessError::kBindingError;
                    status.error = NpmPacketBatchProcessError::kPacketError;
                    status.row = current_row;
                    return status;
                }
                if (status.error != NpmPacketBatchProcessError::kNone) return status;
            }
            *ended_events = std::move(next_events);
            return status;
        }

        struct StagedPacket {
            packet::PacketFamilyLayerView layer;
            packet::AddressFamily family = packet::AddressFamily::kNone;
            uint32_t key_index = 0;
            NpmPacketDirection direction = NpmPacketDirection::kAToB;
            NpmTransportPacketFacts transport;
            FlowLabelFactsV1 label_facts;
            size_t candidate_index = std::numeric_limits<size_t>::max();
            size_t payload_offset = 0;
            size_t payload_size = 0;
            bool control = false;
        };

        PacketScratchBudget scratch_budget(sessions.Budget());
        const uint64_t rows = static_cast<uint64_t>(batch.num_rows());
        const uint64_t row_bytes = sizeof(StagedPacket) + sizeof(uint32_t);
        if (rows > (std::numeric_limits<uint64_t>::max() - 256 * sizeof(FlowLabelFactsV1)) / row_bytes ||
            !scratch_budget.Reserve(rows * row_bytes + 256 * sizeof(FlowLabelFactsV1))) {
            status.packet_status.session_error = NpmSessionTableError::kSessionBudgetExceeded;
            status.packet_status.error = NpmPacketProcessError::kSessionError;
            status.error = NpmPacketBatchProcessError::kPacketError;
            return status;
        }
        std::vector<StagedPacket> staged_packets;
        staged_packets.reserve(static_cast<size_t>(batch.num_rows()));
        std::vector<FlowLabelFactsV1> candidate_facts;
        candidate_facts.reserve(256);
        std::vector<uint32_t> candidate_labels;
        candidate_labels.reserve(static_cast<size_t>(rows));
        std::vector<NpmSessionKey4> keys4;
        std::vector<NpmSessionKey6> keys6;
        keys4.reserve(batch.binding_key_count(packet::AddressFamily::kIPv4, control_enabled));
        keys6.reserve(batch.binding_key_count(packet::AddressFamily::kIPv6, control_enabled));
        const uint64_t actual_rows = staged_packets.capacity() * sizeof(StagedPacket) +
                                     candidate_labels.capacity() * sizeof(uint32_t) +
                                     candidate_facts.capacity() * sizeof(FlowLabelFactsV1);
        const uint64_t reserved_rows = rows * row_bytes + 256 * sizeof(FlowLabelFactsV1);
        const uint64_t key_arrays =
            keys4.capacity() * sizeof(NpmSessionKey4) + keys6.capacity() * sizeof(NpmSessionKey6);
        if (!scratch_budget.Reserve(key_arrays + (actual_rows > reserved_rows ? actual_rows - reserved_rows : 0))) {
            status.packet_status.session_error = NpmSessionTableError::kSessionBudgetExceeded;
            status.packet_status.error = NpmPacketProcessError::kSessionError;
            status.error = NpmPacketBatchProcessError::kPacketError;
            return status;
        }
        // All arrays are fully reserved before the first view; later emplace operations cannot relocate keys.
        NpmSessionAdmissionPlanner admission_plan(sessions);
        size_t candidate_window_begin = 0;
        const auto classify_candidates = [&]() {
            if (candidate_facts.empty()) return 0;
            const int error =
                matcher->ClassifyBatch(candidate_facts.data(), static_cast<uint32_t>(candidate_facts.size()),
                                       candidate_labels.data() + candidate_window_begin);
            candidate_window_begin = candidate_labels.size();
            candidate_facts.clear();
            return error;
        };
        for (current_row = 0; current_row < batch.num_rows(); ++current_row) {
            packet::PacketView packet;
            packet::PacketFamilyLayerView layer;
            status.batch_error = batch.Get(current_row, &packet, &layer);
            if (status.batch_error != NpmPacketBatchError::kNone) {
                status.error = NpmPacketBatchProcessError::kBatchViewError;
                status.row = current_row;
                return status;
            }

            StagedPacket staged;
            staged.layer = layer;
            if (is_control(layer)) {
                NpmInputEventV1 event;
                status.packet_status.binding_error = BuildNpmControlInput(domain_map, packet, layer, &event);
                if (status.packet_status.binding_error != NpmSessionPacketError::kNone) {
                    status.packet_status.error = NpmPacketProcessError::kBindingError;
                    status.error = NpmPacketBatchProcessError::kPacketError;
                    status.row = current_row;
                    return status;
                }
                status.packet_status.session_error = admission_plan.AdvanceControl(packet.meta.timestamp_ns);
                if (status.packet_status.session_error != NpmSessionTableError::kNone) {
                    status.packet_status.error = NpmPacketProcessError::kSessionError;
                    status.error = NpmPacketBatchProcessError::kPacketError;
                    status.row = current_row;
                    return status;
                }
                staged.control = true;
                staged_packets.push_back(std::move(staged));
                continue;
            }
            NpmPacketBindingView binding;
            const auto build = [&](auto& keys) {
                staged.key_index = static_cast<uint32_t>(keys.size());
                keys.emplace_back();
                return BuildNpmSessionPacketBinding(domain_map, packet, layer, &keys.back(), &binding);
            };
            if (layer.Get4()) {
                staged.family = packet::AddressFamily::kIPv4;
                status.packet_status.binding_error = build(keys4);
            } else if (layer.Get6()) {
                staged.family = packet::AddressFamily::kIPv6;
                status.packet_status.binding_error = build(keys6);
            } else {
                status.packet_status.binding_error = NpmSessionPacketError::kIncompleteEndpoint;
            }
            if (status.packet_status.binding_error != NpmSessionPacketError::kNone) {
                status.packet_status.error = NpmPacketProcessError::kBindingError;
                status.error = NpmPacketBatchProcessError::kPacketError;
                status.row = current_row;
                return status;
            }
            if (!scratch_budget.Reserve(binding.key.Namespace().capacity() + 1)) {
                status.packet_status.session_error = NpmSessionTableError::kSessionBudgetExceeded;
                status.packet_status.error = NpmPacketProcessError::kSessionError;
                status.error = NpmPacketBatchProcessError::kPacketError;
                status.row = current_row;
                return status;
            }
            staged.direction = binding.direction;
            staged.transport = binding.transport;
            staged.label_facts = binding.label_facts;
            staged.payload_offset = static_cast<size_t>(binding.payload.data - packet.bytes.data);
            staged.payload_size = binding.payload.size;

            bool requires_admission = false;
            status.packet_status.session_error =
                admission_plan.ObserveAndAdvance(binding, packet.meta, &requires_admission);
            if (status.packet_status.session_error != NpmSessionTableError::kNone) {
                status.packet_status.error = NpmPacketProcessError::kSessionError;
                status.error = NpmPacketBatchProcessError::kPacketError;
                status.row = current_row;
                return status;
            }
            if (requires_admission) {
                staged.candidate_index = candidate_labels.size();
                candidate_facts.push_back(binding.label_facts);
                candidate_labels.push_back(0);
                if (candidate_facts.size() == 256) {
                    status.labeling_error = classify_candidates();
                    if (status.labeling_error != 0) {
                        status.error = NpmPacketBatchProcessError::kLabelingError;
                        return status;
                    }
                }
            }
            staged_packets.push_back(std::move(staged));
        }

        status.labeling_error = classify_candidates();
        if (status.labeling_error != 0) {
            status.error = NpmPacketBatchProcessError::kLabelingError;
            return status;
        }

        NpmSessionSnapshotBatch next_events;
        for (current_row = 0; current_row < static_cast<int64_t>(staged_packets.size()); ++current_row) {
            auto& staged = staged_packets[static_cast<size_t>(current_row)];
            packet::PacketView packet;
            packet::PacketFamilyLayerView layer;
            status.batch_error = batch.Get(current_row, &packet, &layer);
            if (status.batch_error != NpmPacketBatchError::kNone || staged.payload_offset > packet.bytes.size ||
                staged.payload_size > packet.bytes.size - staged.payload_offset) {
                status.error = NpmPacketBatchProcessError::kBatchViewError;
                status.row = current_row;
                return status;
            }
            NpmPacketBindingView binding;
            if (!staged.control) {
                binding.key = staged.family == packet::AddressFamily::kIPv4
                                  ? NpmSessionKeyView(keys4[staged.key_index])
                                  : NpmSessionKeyView(keys6[staged.key_index]);
                binding.direction = staged.direction;
                binding.transport = staged.transport;
                binding.label_facts = staged.label_facts;
                binding.payload = Span<const uint8_t>(packet.bytes.data + staged.payload_offset, staged.payload_size);
            }
            const uint32_t primary_label_id = staged.candidate_index == std::numeric_limits<size_t>::max()
                                                  ? 0
                                                  : candidate_labels[staged.candidate_index];

            if (!advance_before_packet(packet, &next_events)) return status;

            const size_t ends_before_packet = next_events.size();
            status.packet_status =
                staged.control ? dispatch_control(packet, staged.layer)
                               : ProcessNpmBoundPacket(packet, staged.layer, binding, primary_label_id, sessions,
                                                       identifier, modules, writer, &next_events, streams);
            if (status.packet_status.error != NpmPacketProcessError::kNone) {
                status.error = NpmPacketBatchProcessError::kPacketError;
                status.row = current_row;
                return status;
            }
            if (next_events.size() != ends_before_packet && !drain_packet_ends(packet.meta.timestamp_ns)) return status;
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
