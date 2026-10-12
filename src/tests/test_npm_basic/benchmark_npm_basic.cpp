// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <tests/common/packet_family_test_support.h>

#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <plugins/npi/packet_decoder.h>
#include <tests/support/scoped_shared_library.h>
#include <common/loader.hpp>

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
#include <unordered_set>
#include <vector>

namespace {

uint64_t BufferBytes(const arrow::RecordBatch& batch) {
    std::unordered_set<const arrow::Buffer*> seen;
    const auto visit = [&](const auto& self, const std::shared_ptr<arrow::ArrayData>& data) -> uint64_t {
        uint64_t bytes = 0;
        for (const auto& buffer : data->buffers)
            if (buffer && seen.insert(buffer.get()).second) bytes += buffer->capacity();
        for (const auto& child : data->child_data) bytes += self(self, child);
        return bytes;
    };
    uint64_t bytes = 0;
    for (const auto& column : batch.columns()) bytes += visit(visit, column->data());
    return bytes;
}

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

std::shared_ptr<arrow::RecordBatch> MakeInput(int batch_rows, flowsql::IProtocol* protocol, std::string_view family) {
    // Ethernet / IPv4 / TCP RST, with no payload. Each packet closes its own session instance.
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::initializer_list<uint8_t>{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x66, 0x77, 0x88, 0x99, 0xaa, 0x08, 0x00, 0x45, 0x00, 0x00, 0x28,
        0x00, 0x00, 0x00, 0x00, 0x40, 0x06, 0x00, 0x00, 0xc0, 0x00, 0x02, 0x01, 0xc6, 0x33, 0x64, 0x02, 0xa0, 0x28,
        0x01, 0xbb, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x50, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
    auto bytes6 = std::make_shared<std::vector<uint8_t>>(96, 0);
    std::copy(bytes->begin(), bytes->begin() + 12, bytes6->begin());
    (*bytes6)[12] = 0x86;
    (*bytes6)[13] = 0xdd;
    (*bytes6)[14] = 0x60;
    (*bytes6)[19] = 20;
    (*bytes6)[20] = 6;
    (*bytes6)[21] = 64;
    (*bytes6)[22] = 0x20;
    (*bytes6)[23] = 1;
    (*bytes6)[24] = 0x0d;
    (*bytes6)[25] = 0xb8;
    (*bytes6)[37] = 1;
    std::copy(bytes6->begin() + 22, bytes6->begin() + 38, bytes6->begin() + 38);
    (*bytes6)[53] = 2;
    std::copy(bytes->begin() + 34, bytes->end(), bytes6->begin() + 54);
    bytes->resize(96, 0);
    flowsql::protocol::NpiPacketLayerDecoder decoder(protocol);
    std::vector<flowsql::testing::PacketRecord> records(static_cast<size_t>(batch_rows));
    for (int i = 0; i < batch_rows; ++i) {
        const auto packet_bytes = family == "ipv6" || (family == "mixed" && i % 2) ? bytes6 : bytes;
        auto& record = records[static_cast<size_t>(i)];
        record.meta.timestamp_ns = 1'000'000'000;
        record.meta.captured_len = static_cast<uint32_t>(packet_bytes->size());
        record.meta.wire_len = record.meta.captured_len;
        record.meta.link_type = 1;
        record.meta.source_id = 0;
        record.meta.sequence = static_cast<uint64_t>(i);
        record.raw_data = {packet_bytes, packet_bytes->data(), record.meta.captured_len};
        flowsql::packet::PacketFamilyBatch addresses;
        flowsql::packet::PacketLayerFacts facts;
        flowsql::packet::PacketFamilyBatchAddressSink sink(addresses);
        Require(
            decoder.Decode({record.meta, {packet_bytes->data(), packet_bytes->size()}},
                           {flowsql::packet::kExtractMac | flowsql::packet::kExtractIp | flowsql::packet::kExtractPort,
                            flowsql::packet::EndpointScope::kInnermost},
                           &facts, sink) == 0,
            "source-stage decoder failed");
        record.layer = flowsql::packet::PacketFamilyBorrowLayer(facts, sink.address, addresses);
        Require(record.layer.status == flowsql::packet::LayerStatus::kDecoded && record.layer.ports_valid,
                "source-stage layer decoding failed");
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto status = flowsql::testing::EncodePacketBatch(records, &batch, &error);
    Require(status == flowsql::packet::PacketBatchError::kNone, "input encoding failed: " + error);
    return batch;
}

uint64_t ValidateOutputs(const std::vector<flowsql::BlockTransformOutputV1>& outputs,
                         const std::shared_ptr<arrow::Schema>& schema, uint64_t* next_session_id, uint64_t stride) {
    uint64_t count = 0;
    for (const auto& output : outputs) {
        Require(output.batch != nullptr && output.batch->schema()->Equals(*schema, true), "output schema changed");
        const auto ids = std::static_pointer_cast<arrow::UInt64Array>(output.batch->column(0));
        const auto revisions = std::static_pointer_cast<arrow::UInt64Array>(output.batch->column(2));
        const auto finals = std::static_pointer_cast<arrow::BooleanArray>(output.batch->column(4));
        const auto reasons = std::static_pointer_cast<arrow::StringArray>(output.batch->GetColumnByName("end_reason"));
        for (int64_t row = 0; row < output.batch->num_rows(); ++row) {
            Require(!ids->IsNull(row) && ids->Value(row) == *next_session_id && !revisions->IsNull(row) &&
                        revisions->Value(row) == 1 && !finals->IsNull(row) && finals->Value(row) &&
                        !reasons->IsNull(row) && reasons->GetString(row) == "closed",
                    "RST result identity or final semantics mismatch");
            *next_session_id += stride;
            ++count;
        }
    }
    return count;
}

struct BenchmarkCapability {
    flowsql::IBlockTransformOperatorV1* provider = nullptr;
    flowsql::IBlockTransformTaskV1* task = nullptr;
    flowsql::CppOperatorPluginDestroyCapabilityV2Fn destroy_capability = nullptr;

    ~BenchmarkCapability() {
        if (task != nullptr) {
            task->Cancel();
            provider->ReleaseTask(task);
        }
        if (provider != nullptr && destroy_capability != nullptr) destroy_capability(0, provider);
    }
};

void RunBenchmark(int batch_rows, int iterations, flowsql::PluginLoader* loader, flowsql::IProtocol* protocol,
                  std::string_view family) {
    const auto input = MakeInput(batch_rows, protocol, family);
    std::cout << "packet_arrow_capacity_bytes," << BufferBytes(*input) << '\n';
    flowsql::test::ScopedSharedLibrary operator_library(FLOWSQL_NPM_BASIC_PLUGIN_PATH);
    Require(static_cast<bool>(operator_library), "npm.basic library loading failed");
    const auto version = reinterpret_cast<flowsql::CppOperatorPluginAbiVersionFn>(
        operator_library.Symbol(flowsql::kCppOperatorPluginAbiVersionSymbol));
    const auto count = reinterpret_cast<flowsql::CppOperatorPluginCountFn>(
        operator_library.Symbol(flowsql::kCppOperatorPluginCountSymbol));
    const auto describe = reinterpret_cast<flowsql::CppOperatorPluginDescribeV2Fn>(
        operator_library.Symbol(flowsql::kCppOperatorPluginDescribeV2Symbol));
    const auto create = reinterpret_cast<flowsql::CppOperatorPluginCreateCapabilityV2Fn>(
        operator_library.Symbol(flowsql::kCppOperatorPluginCreateCapabilityV2Symbol));
    BenchmarkCapability capability;
    capability.destroy_capability = reinterpret_cast<flowsql::CppOperatorPluginDestroyCapabilityV2Fn>(
        operator_library.Symbol(flowsql::kCppOperatorPluginDestroyCapabilityV2Symbol));
    auto& provider = capability.provider;
    auto& task = capability.task;
    const auto destroy_capability = capability.destroy_capability;
    Require(version && count && describe && create && destroy_capability, "npm.basic V2 plugin symbols missing");
    Require(version() == flowsql::kCppOperatorPluginAbiVersionV2 && count() == 2, "npm.basic plugin ABI mismatch");
    flowsql::CppOperatorDescriptorV2 descriptor{};
    descriptor.struct_size = flowsql::kCppOperatorDescriptorV2Size;
    Require(describe(0, &descriptor) == 0 && descriptor.category != nullptr && descriptor.name != nullptr &&
                std::string_view(descriptor.category) == "npm" && std::string_view(descriptor.name) == "basic",
            "npm.basic operator descriptor mismatch");
    provider = static_cast<flowsql::IBlockTransformOperatorV1*>(create(0, loader));
    Require(provider != nullptr, "npm.basic capability creation failed");
    const std::string entity = family == "ipv6" ? "basic_ipv6" : "basic_ipv4";
    const uint64_t stride = family == "mixed" ? 2 : 1;
    const uint64_t packets = static_cast<uint64_t>(batch_rows) * static_cast<uint64_t>(iterations);
    std::cout << "family,mode,packets,wire_bytes,batch_rows,iterations,wall_ms,packets_per_second,output_rows\n";
    const auto measure = [&](const char* mode, const std::string& with_params_json) {
        flowsql::BlockTransformTaskConfigV1 config;
        config.task_id = mode;
        config.with_params_json = with_params_json.c_str();
        config.pushed_filter_plan_json = R"({"version":1,"root":null})";
        Require(provider->CreateTask(config, &task) == 0 && task != nullptr, "task creation failed");
        std::shared_ptr<arrow::Schema> schema;
        const int open_rc = task->Open(input->schema(), &schema);
        Require(open_rc == 0, "task Open failed: " + task->LastError());
        Require(schema && schema->metadata() && schema->metadata()->Get("flowsql.entity").ValueOrDie() == entity &&
                    schema->GetFieldByName("a_ip")->type()->id() ==
                        (family == "ipv6" ? arrow::Type::FIXED_SIZE_BINARY : arrow::Type::UINT32) &&
                    !schema->GetFieldByName("ip_family"),
                "concrete Basic Schema mismatch");
        std::vector<flowsql::BlockTransformOutputV1> outputs;
        uint64_t next_session_id = 1;
        const auto process = [&]() {
            const int rc = task->ProcessBlock(input, 1000, &outputs);
            Require(rc == static_cast<int>(flowsql::BlockTransformStatusV1::kContinue),
                    "ProcessBlock failed: " + task->LastError());
            const uint64_t rows = ValidateOutputs(outputs, schema, &next_session_id, stride);
            Require(rows == static_cast<uint64_t>(batch_rows) / stride, "result count differs from input count");
            outputs.clear();  // Release Arrow output owners and their pending-output budget leases.
            return rows;
        };
        process();  // Each mode has its own warm-up and fresh task state.
        uint64_t output_rows = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) output_rows += process();
        const double wall_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Require(output_rows == packets / stride && wall_ms > 0.0, "invalid benchmark measurement");
        const int flush_rc = task->Flush(&outputs);
        Require(flush_rc == 0, "EOF Flush failed: " + task->LastError());
        Require(ValidateOutputs(outputs, schema, &next_session_id, stride) == 0, "closed sessions survived until EOF");
        outputs.clear();
        schema.reset();
        provider->ReleaseTask(task);
        task = nullptr;
        std::cout << family << ',' << mode << ',' << packets << ',' << packets * 96 << ',' << batch_rows << ','
                  << iterations << ',' << std::fixed << std::setprecision(3) << wall_ms << ','
                  << static_cast<double>(packets) * 1000.0 / wall_ms << ',' << output_rows << '\n';
    };
    const std::string common =
        "{\"input_namespace\":\"benchmark.packet\",\"source_domains\":\"0:7\",\"result_mode\":\"final\",\"observing\":"
        "\"" +
        entity + "\"";
    measure("basic-only", common + '}');
    measure("basic+session", common + ",\"features\":\"basic,session\"}");
}

}  // namespace

int main(int argc, char* argv[]) {
    auto* loader = flowsql::PluginLoader::Single();
    try {
        Require(argc == 1 || argc == 3 || argc == 4,
                "usage: benchmark_npm_basic [batch_rows(1..4096) iterations(1..1000) [ipv4|ipv6|mixed]]");
        const int batch_rows = argc >= 3 ? ParsePositive(argv[1], 4096) : 512;
        const int iterations = argc >= 3 ? ParsePositive(argv[2], 1000) : 100;
        const std::string family = argc == 4 ? argv[3] : "ipv4";
        Require(family == "ipv4" || family == "ipv6" || family == "mixed", "invalid family");
        Require(family != "mixed" || batch_rows % 2 == 0, "mixed requires an even batch");
        const std::string npi_option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\"}";
        const char* libraries[] = {FLOWSQL_NPI_PLUGIN_PATH};
        const char* options[] = {npi_option.c_str()};
        Require(loader->Load(".", libraries, options, 1) == 0, "NPI plugin loading failed");
        Require(loader->StartAll() == 0, "plugin start failed");
        auto* protocol = static_cast<flowsql::IProtocol*>(loader->First(flowsql::IID_PROTOCOL));
        Require(protocol != nullptr, "NPI protocol interface not registered");
        RunBenchmark(batch_rows, iterations, loader, protocol, family);
        loader->StopAll();
        loader->Unload();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_npm_basic failed: " << error.what() << '\n';
        loader->StopAll();
        loader->Unload();
        return 1;
    }
}
