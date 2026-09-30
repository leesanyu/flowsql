// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_PROVIDER_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_PROVIDER_H_

#include "npm_session_table.h"
#include "npm_tcp_stream_shared.h"

#include <atomic>
#include <map>
#include <vector>

namespace flowsql::npm {

class NpmProtocolModuleAdapter;

/** Borrowed adapter and consumer pointers are frozen at Open and outlive the provider. */
struct NpmTcpStreamRegistration {
    NpmProtocolModuleAdapter* adapter = nullptr;
    INpmTcpStreamConsumerV1* consumer = nullptr;
};

/** Task-private stream coordinator. The runtime operation gate serializes all calls and cancellation. */
class NpmTcpStreamProvider final {
 public:
    static NpmTcpStreamError Create(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                                    Span<const NpmTcpStreamRegistration> registrations,
                                    std::unique_ptr<NpmTcpStreamProvider>* output,
                                    const std::atomic<bool>* cancellation_requested = nullptr);
    ~NpmTcpStreamProvider();
    NpmTcpStreamProvider(const NpmTcpStreamProvider&) = delete;
    NpmTcpStreamProvider& operator=(const NpmTcpStreamProvider&) = delete;

    int OnPacket(const NpmPacketView& packet, const NpmSessionView& session);
    int OnSessionEnd(const NpmSessionSnapshot& session, int64_t observed_at_ns);
    int AdvanceWatermark(int64_t watermark_ns);
    std::optional<int64_t> NextEventDeadlineNs() const;
    void Abort() noexcept;
    size_t ActiveDirections() const noexcept;
    const NpmTcpStreamFailure& Failure() const noexcept { return failure_; }

 private:
    struct ConsumerGate final : INpmTcpStreamConsumerV1 {
        ConsumerGate(INpmTcpStreamConsumerV1* target, const std::atomic<bool>* cancellation_requested)
            : target(target), cancellation_requested(cancellation_requested) {}
        INpmTcpStreamConsumerV1* target = nullptr;
        const std::atomic<bool>* cancellation_requested = nullptr;
        int OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                INpmResultEmitterV1& emitter) override;
    };
    struct Entry {
        NpmSessionSnapshot snapshot;
        std::unique_ptr<NpmTcpStreamSharedDirection> directions[2];
        bool seen[2] = {};
        uint64_t charge = 0;
    };
    using Entries = std::map<uint64_t, Entry>;

    NpmTcpStreamProvider(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                         const std::atomic<bool>* cancellation_requested);
    void Initialize(Span<const NpmTcpStreamRegistration> registrations);
    void Reserve(uint64_t bytes);
    void Release(uint64_t bytes) noexcept;
    Entries::iterator AddSession(const NpmSessionView& session);
    void Refresh(Entry& entry, const NpmSessionView& session) noexcept;
    void Erase(Entries::iterator at) noexcept;
    void SaveFailure(const NpmTcpStreamSharedDirection& direction) noexcept;

    NpmTcpStreamConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_;
    std::vector<NpmTcpStreamRegistration> registrations_;
    std::vector<ConsumerGate> consumer_gates_;
    std::vector<NpmTcpStreamSubscription> scratch_;
    Entries entries_;
    NpmTcpStreamFailure failure_;
    uint64_t charged_ = 0;
    bool aborted_ = false;
    const std::atomic<bool>* cancellation_requested_ = nullptr;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_PROVIDER_H_
