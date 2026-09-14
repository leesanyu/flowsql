// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/loader.hpp>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <plugins/npi/packet_decoder.h>

#include <arrow/api.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

int ParsePositive(const char* value, int maximum) {
    int result = 0;
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    Require(parsed.ec == std::errc{} && parsed.ptr == end && result > 0 && result <= maximum,
            "invalid positive benchmark argument: " + std::string(value));
    return result;
}

std::shared_ptr<arrow::RecordBatch> MakeInput(int batch_rows, flowsql::IProtocol* protocol) {
    // Ethernet / IPv4 / TCP RST, with no payload. Each packet closes its own session instance.
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x66, 0x77, 0x88, 0x99, 0xaa, 0x08, 0x00,
        0x45, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x40, 0x06, 0x00, 0x00,
        0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02,
        0xa0, 0x28, 0x01, 0xbb, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x50, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
    flowsql::protocol::NpiPacketLayerDecoder decoder(protocol);
    std::vector<flowsql::packet::PacketRecord> records(static_cast<size_t>(batch_rows));
    for (int i = 0; i < batch_rows; ++i) {
        auto& record = records[static_cast<size_t>(i)];
        record.meta.timestamp_ns = 1'000'000'000;
        record.meta.captured_len = static_cast<uint32_t>(bytes->size());
        record.meta.wire_len = record.meta.captured_len;
        record.meta.link_type = 1;
        record.meta.source_id = 0;
        record.meta.sequence = static_cast<uint64_t>(i);
        record.raw_data = {bytes, bytes->data(), record.meta.captured_len};
        record.layer = decoder.Decode(
            {record.meta, {bytes->data(), bytes->size()}},
            {flowsql::packet::kExtractMac | flowsql::packet::kExtractIp | flowsql::packet::kExtractPort,
             flowsql::packet::EndpointScope::kInnermost});
        Require(record.layer.status == flowsql::packet::LayerStatus::kDecoded && record.layer.ports_valid,
                "source-stage layer decoding failed");
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto status = flowsql::packet::EncodePacketBatch(records, &batch, &error);
    Require(status == flowsql::packet::PacketBatchError::kNone, "input encoding failed: " + error);
    return batch;
}

uint64_t ValidateOutputs(const std::vector<flowsql::BlockTransformOutputV1>& outputs,
                         const std::shared_ptr<arrow::Schema>& schema,
                         uint64_t* next_session_id) {
    uint64_t count = 0;
    for (const auto& output : outputs) {
        Require(output.batch != nullptr && output.batch->schema()->Equals(*schema, true),
                "output schema changed");
        const auto ids = std::static_pointer_cast<arrow::UInt64Array>(output.batch->column(0));
        const auto revisions = std::static_pointer_cast<arrow::UInt64Array>(output.batch->column(2));
        const auto finals = std::static_pointer_cast<arrow::BooleanArray>(output.batch->column(4));
        const auto reasons = std::static_pointer_cast<arrow::StringArray>(output.batch->column(21));
        for (int64_t row = 0; row < output.batch->num_rows(); ++row) {
            Require(!ids->IsNull(row) && ids->Value(row) == *next_session_id &&
                        !revisions->IsNull(row) && revisions->Value(row) == 1 &&
                        !finals->IsNull(row) && finals->Value(row) &&
                        !reasons->IsNull(row) && reasons->GetString(row) == "closed",
                    "RST result identity or final semantics mismatch");
            ++*next_session_id;
            ++count;
        }
    }
    return count;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto* loader = flowsql::PluginLoader::Single();
    flowsql::IBlockTransformOperatorV1* provider = nullptr;
    flowsql::IBlockTransformTaskV1* task = nullptr;
    try {
        Require(argc == 1 || argc == 3, "usage: benchmark_npm_basic [batch_rows(1..4096) iterations(1..1000)]");
        const int batch_rows = argc == 3 ? ParsePositive(argv[1], 4096) : 512;
        const int iterations = argc == 3 ? ParsePositive(argv[2], 1000) : 100;
        const std::string npi_option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\"}";
        const char* libraries[] = {FLOWSQL_NPI_PLUGIN_PATH, FLOWSQL_NPM_BASIC_PLUGIN_PATH};
        const char* options[] = {npi_option.c_str(), nullptr};
        Require(loader->Load(".", libraries, options, 2) == 0, "plugin loading failed");
        Require(loader->StartAll() == 0, "plugin start failed");
        auto* protocol = static_cast<flowsql::IProtocol*>(loader->First(flowsql::IID_PROTOCOL));
        Require(protocol != nullptr, "NPI protocol interface not registered");
        const auto input = MakeInput(batch_rows, protocol);
        loader->Traverse(flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V1, [&](void* value) {
            auto* candidate = static_cast<flowsql::IBlockTransformOperatorV1*>(value);
            if (candidate->Category() == "npm" && candidate->Name() == "basic") provider = candidate;
            return 0;
        });
        Require(provider != nullptr, "npm.basic provider not registered");
        flowsql::BlockTransformTaskConfigV1 config;
        config.task_id = "npm-basic-benchmark";
        config.with_params_json = R"({"input_namespace":"benchmark.packet","source_domains":"0:7"})";
        config.pushed_filter_plan_json = R"({"version":1,"root":null})";
        Require(provider->CreateTask(config, &task) == 0 && task != nullptr, "task creation failed");
        std::shared_ptr<arrow::Schema> schema;
        const int open_rc = task->Open(input->schema(), &schema);
        Require(open_rc == 0, "task Open failed: " + task->LastError());
        Require(schema != nullptr && schema->num_fields() == 22 &&
                    schema->field(0)->type()->Equals(arrow::uint64()) &&
                    schema->field(2)->type()->Equals(arrow::uint64()) &&
                    schema->field(4)->type()->Equals(arrow::boolean()) &&
                    schema->field(21)->type()->Equals(arrow::utf8()),
                "expected fixed 22-column result schema");
        std::vector<flowsql::BlockTransformOutputV1> outputs;
        uint64_t next_session_id = 1;
        auto process = [&]() {
            const int rc = task->ProcessBlock(input, 1000, &outputs);
            Require(rc == static_cast<int>(flowsql::BlockTransformStatusV1::kContinue),
                    "ProcessBlock failed: " + task->LastError());
            const uint64_t rows = ValidateOutputs(outputs, schema, &next_session_id);
            Require(rows == static_cast<uint64_t>(batch_rows), "result count differs from input count");
            outputs.clear();  // Release Arrow output owners and their pending-output budget leases.
            return rows;
        };
        process();  // Warm-up is excluded from the measurement and reported row counts.
        uint64_t output_rows = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) output_rows += process();
        const double wall_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const uint64_t packets = static_cast<uint64_t>(batch_rows) * static_cast<uint64_t>(iterations);
        Require(output_rows == packets, "total result count mismatch");
        const int flush_rc = task->Flush(&outputs);
        Require(flush_rc == 0, "EOF Flush failed: " + task->LastError());
        Require(ValidateOutputs(outputs, schema, &next_session_id) == 0, "closed sessions survived until EOF");
        outputs.clear();
        schema.reset();
        provider->ReleaseTask(task);
        task = nullptr;
        loader->StopAll();
        loader->Unload();
        std::cout << "packets,batch_rows,iterations,wall_ms,packets_per_second,output_rows\n"
                  << packets << ',' << batch_rows << ',' << iterations << ',' << std::fixed << std::setprecision(3)
                  << wall_ms << ',' << static_cast<double>(packets) * 1000.0 / wall_ms << ',' << output_rows << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_npm_basic failed: " << error.what() << '\n';
        if (task != nullptr) {
            task->Cancel();
            provider->ReleaseTask(task);
        }
        loader->StopAll();
        loader->Unload();
        return 1;
    }
}
