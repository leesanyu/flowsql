// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_READER_H_
#define FLOWSQL_CHANNELS_NETADAPTER_READER_H_
#include <framework/core/capture_progress_tracker.h>
#include <framework/core/packet_filter_plan.h>
#include <framework/interfaces/icapture_backend.h>
#include <plugins/npi/iprotocol.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include "netadapter_budget.h"
#include "netadapter_config.h"
namespace flowsql::channels::netadapter {
class NetAdapterReader final : public ICaptureBlockStreamReaderV2 {
 public:
    using MonotonicNow = std::function<std::chrono::steady_clock::time_point()>;
    NetAdapterReader(std::string name, CaptureBackendConfigV1 config, uint64_t domain, uint64_t generation,
                     std::shared_ptr<ICaptureBackendSessionV1> backend, IProtocol* protocol,
                     std::shared_ptr<void> channel_lease, std::shared_ptr<const packet::PcapFilterPlan> filter,
                     MonotonicNow monotonic_now = std::chrono::steady_clock::now);
    ~NetAdapterReader() override;
    const char* Category() override { return "netadapter"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_ && !terminal_; }
    int Flush() override;
    BlockPollEvent PollBlock(int timeout_ms) override { return PollCapture(timeout_ms).block; }
    CapturePollEventV2 PollCapture(int timeout_ms) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    void Cancel() override;
    bool IsFinished() const override { return terminal_ != 0; }
    int DescribeSources(CaptureSourceSetV2* sources) const override;
    int ReadInputCounters(uint32_t source, CaptureCountersV1* counters) const override;
    std::string Diagnostics() const;

 private:
    std::string name_;
    CaptureBackendConfigV1 config_;
    CaptureSourceSetV2 sources_;
    std::vector<std::string> source_names_;
    std::shared_ptr<ICaptureBackendSessionV1> backend_;
    IProtocol* protocol_;
    std::shared_ptr<void> channel_lease_;
    std::shared_ptr<const packet::PcapFilterPlan> filter_;
    MonotonicNow monotonic_now_;
    std::shared_ptr<CaptureMemoryPool> pool_;
    std::vector<CaptureCountersV1> counters_;
    std::vector<uint64_t> fact_sequences_;
    std::vector<CaptureBacklogV1> backlogs_;
    std::vector<int64_t> candidates_;
    std::map<const arrow::RecordBatch*, std::weak_ptr<arrow::RecordBatch>> outstanding_;
    std::atomic<int> terminal_{0};
    std::atomic<bool> opened_{false};
    bool eof_reported_ = false;
    int cancel_fd_ = -1;
    uint64_t sequence_ = 0, backend_bytes_ = 0;
    size_t next_input_ = 0;
    mutable std::mutex mutex_;
};
}  // namespace flowsql::channels::netadapter
#endif
