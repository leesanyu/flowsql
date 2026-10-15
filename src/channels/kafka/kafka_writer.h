// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ikafka_connection_service.h>
#include <framework/interfaces/iresult_sink.h>
#include <atomic>
#include <mutex>
#include "kafka_result_codec.h"
namespace flowsql::channels::kafka {
class ResultWriter final : public IResultSinkWriterV1 {
 public:
    ResultWriter(SinkConfig, ResultSinkContextV1, std::shared_ptr<IKafkaClientSessionV1>);
    ~ResultWriter() override { Cancel(); }
    int Open(std::shared_ptr<arrow::Schema>) override;
    int Write(const std::shared_ptr<arrow::RecordBatch>&) override;
    int Finish() override;
    void Cancel() override;
    ResultSinkReportV1 Report() const override;

 private:
    int Fail(int rc, const std::string& code, const char* stage);
    void Binding();
    SinkConfig config_;
    ResultSinkContextV1 context_;
    std::shared_ptr<IKafkaClientSessionV1> session_;
    IKafkaProducerSessionV1* producer_ = nullptr;
    std::unique_ptr<ResultCodec> codec_;
    mutable std::mutex mutex_;
    ResultSinkReportV1 report_;
    std::atomic<bool> cancelled_{false};
};
}  // namespace flowsql::channels::kafka
