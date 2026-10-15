// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
#include <cstdint>
#include <memory>
#include <string>
namespace arrow {
class Schema;
class RecordBatch;
}  // namespace arrow
namespace flowsql {
const Guid IID_RESULT_SINK_FACTORY_V1 = {0x86b65f11, 0x9acd, 0x42ae, {0x92, 0x81, 0x68, 0xc9, 0x57, 0x2a, 0x17, 0x51}};
struct ResultSinkContextV1 {
    uint32_t contract_version = 1;
    std::string target, task_id;
};
struct ResultSinkReportV1 {
    std::string publication_id, state, code, stage, binding_json;
    uint64_t input_rows = 0, confirmed_rows = 0, not_sent_rows = 0, unknown_rows = 0;
};
// Open/Write/Finish are serialized by the caller. Cancel/Report may run concurrently.
interface IResultSinkWriterV1 {
    virtual ~IResultSinkWriterV1() = default;
    virtual int Open(std::shared_ptr<arrow::Schema> schema) = 0;
    virtual int Write(const std::shared_ptr<arrow::RecordBatch>& batch) = 0;
    virtual int Finish() = 0;
    virtual void Cancel() = 0;
    virtual ResultSinkReportV1 Report() const = 0;
};
interface IResultSinkFactoryV1 {
    virtual ~IResultSinkFactoryV1() = default;
    virtual int CreateWriter(const ResultSinkContextV1&, std::unique_ptr<IResultSinkWriterV1>*) = 0;
};
}  // namespace flowsql
