// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/ikafka_connection_service.h>
#include <plugins/npi/iprotocol.h>
#include <atomic>
#include <map>
#include <mutex>
#include "kafka_packet.h"

namespace flowsql::channels::kafka {
class PacketReader final : public ICaptureBlockStreamReader,
                           public IBlockStreamSchema,
                           public IBlockStreamAcknowledgement {
 public:
    PacketReader(std::string name, PacketSourceConfig config, std::shared_ptr<IKafkaClientSession> session,
                 IProtocol* protocol);
    ~PacketReader() override;
    const char* Category() override { return "kafka"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return schema_text_.c_str(); }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override;
    int Open() override;
    int Close() override;
    int Flush() override { return 0; }
    bool IsOpened() const override { return opened_; }
    bool IsFinished() const override { return cancelled_ || terminal_; }
    BlockPollEvent PollBlock(int timeout_ms) override { return PollCapture(timeout_ms).block; }
    CapturePollEvent PollCapture(int timeout_ms) override;
    int DescribeSources(CaptureSourceSet*) const override;
    int ReadInputCounters(uint32_t source_id, CaptureCounters*) const override;
    int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>&) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override;
    void Cancel() override;
    std::string Diagnostics() const;

 private:
    BlockPollEvent Error(int error, const char* stage);
    std::string name_, schema_text_;
    PacketSourceConfig config_;
    const std::shared_ptr<IKafkaClientSession> session_;
    IKafkaConsumerSession* consumer_ = nullptr;
    IProtocol* protocol_;
    CaptureSourceSet sources_;
    mutable std::mutex mutex_;
    std::atomic<bool> opened_{false}, cancelled_{false}, terminal_{false};
    int terminal_error_ = 0;
    const char* stage_ = "open";
    bool bound_ = false, acknowledged_ = false, commit_uncertain_ = false;
    std::array<uint8_t, 16> epoch_{};
    uint64_t remote_generation_ = 0, poll_sequence_ = 0, packet_sequence_ = 0;
    int64_t first_offset_ = -1, current_offset_ = -1, acknowledged_offset_ = -1;
    std::map<uint32_t, uint64_t> fact_sequences_;
    std::map<uint32_t, CaptureCounters> counters_;
    std::shared_ptr<arrow::RecordBatch> outstanding_;
};
}  // namespace flowsql::channels::kafka
