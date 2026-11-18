// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ikafka_connection_service.h>
#include <framework/interfaces/iresult_sink.h>
#include <atomic>
#include <mutex>
#include "kafka_result_codec.h"
namespace flowsql::channels::kafka {
class ResultWriter final : public IResultSinkWriter {
 public:
    ResultWriter(SinkConfig, ResultSinkContext, std::shared_ptr<IKafkaClientSession>);
    ~ResultWriter() override { Cancel(); }
    int Open(std::shared_ptr<arrow::Schema>) override;
    int Write(const std::shared_ptr<arrow::RecordBatch>&) override;
    int Finish() override;
    void Cancel() override;
    ResultSinkReport Report() const override;

 private:
    int Fail(int rc, const std::string& code, const char* stage);
    void Binding();
    SinkConfig config_;
    ResultSinkContext context_;
    std::shared_ptr<IKafkaClientSession> session_;
    IKafkaProducerSession* producer_ = nullptr;
    std::unique_ptr<ResultCodec> codec_;
    mutable std::mutex mutex_;
    ResultSinkReport report_;
    std::atomic<bool> cancelled_{false};
};
}  // namespace flowsql::channels::kafka
