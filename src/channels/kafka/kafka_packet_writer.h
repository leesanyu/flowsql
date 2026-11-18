// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ikafka_connection_service.h>
#include <framework/interfaces/ikafka_producer_session.h>
#include <framework/interfaces/ipacket_sink.h>
#include <atomic>
#include <mutex>
#include "kafka_packet.h"
namespace flowsql::channels::kafka {
class PacketWriter final : public IPacketSinkWriter {
 public:
    PacketWriter(PacketSinkConfig, PacketSinkContext, std::shared_ptr<IKafkaClientSession>,
                 std::shared_ptr<void> lease = {});
    ~PacketWriter() override { session_->Cancel(); }
    int Open(const CaptureSourceSet&, std::shared_ptr<arrow::Schema>) override;
    int Write(const CapturePollEvent&) override;
    int Finish() override;
    void Cancel() override;
    PacketSinkReport Report() const override;

 private:
    int Fail(int, const char*, const char*);
    void Binding();  // mutex held
    PacketSinkConfig config_;
    PacketSinkContext context_;
    std::shared_ptr<void> lease_;
    std::shared_ptr<IKafkaClientSession> session_;
    IKafkaProducerSession* producer_ = nullptr;
    PacketSinkCodec codec_;
    std::array<uint8_t, 16> epoch_{};
    uint64_t poll_sequence_ = 0, queue_peak_messages_ = 0, queue_peak_bytes_ = 0, encoded_peak_bytes_ = 0;
    mutable std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
    PacketSinkReport report_;
};
}  // namespace flowsql::channels::kafka
