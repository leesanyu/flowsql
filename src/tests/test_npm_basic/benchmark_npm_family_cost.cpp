// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_task_budget.h>
#include "npm_packet_test_support.h"

#include <malloc.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
thread_local bool record_allocations = false;
uint64_t allocation_counts[8193]{};
uint64_t allocation_bytes = 0;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
uint64_t HeapBytes() {
    const auto info = mallinfo2();
    return info.uordblks + info.hblkhd;
}
uint64_t RssBytes() {
    std::ifstream status("/proc/self/statm");
    uint64_t total = 0, resident = 0;
    status >> total >> resident;
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}
void Phase(const char* name, uint64_t initial_heap, const flowsql::npm::NpmTaskBudget& budget) {
    const auto usage = budget.Usage();
    std::printf(
        "phase,%s,heap_bytes,%lu,heap_delta,%ld,rss_bytes,%lu,session_budget,%lu,input_budget,%lu,pending_budget,%lu\n",
        name, HeapBytes(), static_cast<int64_t>(HeapBytes()) - static_cast<int64_t>(initial_heap), RssBytes(),
        usage.session_state_bytes, usage.input_batch_bytes, usage.pending_output_bytes);
}
flowsql::npm::NpmTestPacketBinding Binding(int index, bool ipv6) {
    using namespace flowsql::npm;
    NpmTestPacketBinding binding;
    const auto fill = [&](auto& key) {
        key.input_namespace = "cost.packet";
        key.observation_domain_id = 7;
        key.transport_protocol = 17;
        key.a_port = static_cast<uint16_t>(index + 1);
        key.b_port = 53;
        key.a_ip.bytes[0] = 1;
        key.b_ip.bytes[0] = 2;
    };
    if (ipv6) {
        NpmSessionKey6 key;
        fill(key);
        binding.key = std::move(key);
    } else {
        NpmSessionKey4 key;
        fill(key);
        binding.key = std::move(key);
    }
    return binding;
}
}  // namespace

// Only this isolated measurement executable intercepts C++ node allocations. Production is unchanged.
void* operator new(size_t size) {
    void* value = std::malloc(size ? size : 1);
    if (!value) throw std::bad_alloc();
    if (record_allocations) {
        ++allocation_counts[std::min<size_t>(size, 8192)];
        allocation_bytes += malloc_usable_size(value);
    }
    return value;
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

int main(int argc, char** argv) {
    using namespace flowsql::npm;
    try {
        const std::string family = argc == 2 ? argv[1] : "ipv4";
        Require(argc <= 2 && (family == "ipv4" || family == "ipv6" || family == "mixed"),
                "usage: benchmark_npm_family_cost [ipv4|ipv6|mixed]");
        constexpr int count = 30000;
        auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
        config.max_active_sessions = count;
        auto budget = std::make_shared<NpmTaskBudget>(config);
        std::printf(
            "family,%s,sessions,%d,max_tracked_bytes,%lu,max_pending_bytes,%lu,key4_size,%zu,key6_size,%zu,snapshot_"
            "batch_"
            "size,%zu\n",
            family.c_str(), count, config.max_tracked_bytes, config.max_pending_output_bytes, sizeof(NpmSessionKey4),
            sizeof(NpmSessionKey6), sizeof(NpmSessionSnapshotBatch));
        const auto initial_heap = HeapBytes();
        Phase("empty", initial_heap, *budget);
        uint64_t owned_snapshot_heap = 0, cleared_snapshot_heap = 0;
        {
            NpmSessionTable table(config, budget);
            const auto started = std::chrono::steady_clock::now();
            for (int index = 0; index < count; ++index) {
                auto binding = Binding(index, family == "ipv6" || (family == "mixed" && index % 2));
                flowsql::packet::PacketMeta meta;
                meta.timestamp_ns = index + 1;
                meta.wire_len = 96;
                NpmSessionObserveResult result;
                record_allocations = true;
                const auto status = table.Observe(binding, meta, &result);
                record_allocations = false;
                Require(status == NpmSessionTableError::kNone && result.has_active_session &&
                            result.active_session.session_id == static_cast<uint64_t>(index + 1),
                        "session admission failed");
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            Require(table.size() == count, "session count mismatch");
            const auto before_denied = table.tracked_bytes();
            auto excess = Binding(count, true);
            flowsql::packet::PacketMeta meta;
            meta.timestamp_ns = count + 1;
            NpmSessionObserveResult result;
            Require(table.Observe(excess, meta, &result) == NpmSessionTableError::kSessionLimitExceeded &&
                        table.size() == count && table.tracked_bytes() == before_denied,
                    "shared total limit changed state");
            Phase("table_and_indexes", initial_heap, *budget);
            std::printf("admission_ms,%.3f,node_allocated_usable_bytes,%lu\n", ms, allocation_bytes);
            for (size_t size = 0; size < 8193; ++size)
                if (allocation_counts[size])
                    std::printf("node_allocation,requested_size,%zu,count,%lu\n", size, allocation_counts[size]);
            std::vector<NpmSessionView> views;
            Require(table.SnapshotActive(&views) == NpmSessionTableError::kNone && views.size() == count,
                    "borrowed snapshot failed");
            Phase("borrowed_views", initial_heap, *budget);
            views.clear();
            views.shrink_to_fit();
            NpmSessionSnapshotBatch snapshots;
            Require(table.FinishAllAtEof(&snapshots) == NpmSessionTableError::kNone && snapshots.size() == count &&
                        table.size() == 0 && !table.NextEventDeadlineNs(),
                    "owned EOF failed");
            Phase("owned_snapshots", initial_heap, *budget);
            owned_snapshot_heap = HeapBytes();
            std::printf("snapshot_batch_tracked_bytes,%lu\n", NpmThroughputTestAccess::StorageBytes(snapshots));
            for (int index = 0; index < count; ++index) {
                const auto view = snapshots.View(index);
                Require(view.session_id == static_cast<uint64_t>(index + 1) && view.packets_ab == 1 &&
                            view.wire_bytes_ab == 96,
                        "snapshot values mismatch");
                Require((view.key->Get6() != nullptr) == (family == "ipv6" || (family == "mixed" && index % 2)),
                        "snapshot family mismatch");
            }
            snapshots.Clear();
            cleared_snapshot_heap = HeapBytes();
            Phase("snapshots_released_buckets_retained", initial_heap, *budget);
            Require(NpmTrackedBudgetBytes(budget->Usage()) == 0 && budget->Usage().pending_output_bytes == 0,
                    "budget not returned");
        }
        Phase("destroyed", initial_heap, *budget);
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::printf(
            "owned_snapshot_heap_bytes,%lu,tracked_peak,%lu,input_peak,%lu,rss_peak_bytes,%ld,limit_rejected,1,budget_"
            "returned,1\n",
            owned_snapshot_heap - cleared_snapshot_heap, budget->HighWaterMarks().tracked_bytes,
            budget->HighWaterMarks().input_batch_bytes, usage.ru_maxrss * 1024);
        return 0;
    } catch (const std::exception& error) {
        record_allocations = false;
        std::fprintf(stderr, "benchmark_npm_family_cost: %s\n", error.what());
        return 1;
    }
}
