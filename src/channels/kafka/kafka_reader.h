// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_channel.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/ikafka_connection_service.h>
#include <atomic>
#include "kafka_structured.h"
namespace flowsql::channels::kafka {
class StructuredReader final : public IBlockStreamChannel,
                               public IBlockStreamAcknowledgementV1,
                               public IBlockStreamSchemaV1 {
 public:
    StructuredReader(std::string name, StructuredConfig config, std::shared_ptr<IKafkaClientSessionV1> session);
    ~StructuredReader() override { Close(); }
    const char* Category() override { return "kafka"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return schema_.c_str(); }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override { return arrow_schema_; }
    int Open() override;
    int Close() override;
    int Flush() override { return 0; }
    bool IsOpened() const override { return opened_; }
    BlockPollEvent PollBlock(int timeout_ms) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    void Cancel() override;
    bool IsFinished() const override { return cancelled_ || terminal_; }

 private:
    BlockPollEvent Error(int error);
    std::string name_, schema_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
    StructuredConfig config_;
    std::shared_ptr<IKafkaClientSessionV1> session_;
    IKafkaConsumerSessionV1* consumer_ = nullptr;
    std::atomic<bool> opened_{false}, cancelled_{false}, terminal_{false};
    int terminal_error_ = 0;
    bool acknowledged_ = false;
    std::shared_ptr<arrow::RecordBatch> outstanding_;
    std::vector<KafkaOffsetV1> offsets_;
};
}  // namespace flowsql::channels::kafka
