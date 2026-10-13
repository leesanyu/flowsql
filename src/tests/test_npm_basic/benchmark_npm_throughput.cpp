// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <tests/common/packet_family_test_support.h>

#include <arrow/util/byte_size.h>
#include <framework/core/capture_progress_tracker.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/idatabase_arrow_write.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/iflow_labeling.h>
#include <operators/npm_basic/npm_analysis_contract.h>
#include <plugins/npi/packet_decoder.h>
#include <tests/support/scoped_shared_library.h>
#include <common/loader.hpp>

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <rapidjson/document.h>

#include <malloc.h>
#include <sys/resource.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
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

std::shared_ptr<arrow::RecordBatch> MakeInput(int batch_rows, flowsql::IProtocol* protocol, std::string_view family,
                                              std::string_view pattern, int cardinality, int64_t time_base,
                                              int64_t time_step, int source_count, uint64_t* wire_digest = nullptr) {
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
        const bool six = family == "ipv6" || (family == "mixed" && (pattern == "bidir" ? i / 2 : i) % 2);
        auto packet_bytes = std::make_shared<std::vector<uint8_t>>(*(six ? bytes6 : bytes));
        const size_t tcp = six ? 54 : 34;
        const uint16_t port = static_cast<uint16_t>(1 + (pattern == "bidir" ? i / 2 : i) % cardinality);
        (*packet_bytes)[tcp] = static_cast<uint8_t>(port >> 8);
        (*packet_bytes)[tcp + 1] = static_cast<uint8_t>(port);
        (*packet_bytes)[tcp + 13] = pattern == "rst" ? 4 : 16;
        if (pattern == "payload" || pattern == "bidir" || pattern == "multiaddr") {
            const size_t payload_size = 16;
            if (six)
                (*packet_bytes)[19] = 20 + payload_size;
            else
                (*packet_bytes)[17] = 40 + payload_size;
            for (size_t j = 0; j < payload_size; ++j) (*packet_bytes)[tcp + 20 + j] = 'x';
            (*packet_bytes)[tcp + 7] = static_cast<uint8_t>(time_base);
        }
        if (pattern == "dns" || pattern == "http1" || pattern == "tls" || pattern == "icmp") {
            std::vector<uint8_t> body;
            const auto u16 = [&](uint16_t value) {
                body.push_back(static_cast<uint8_t>(value >> 8));
                body.push_back(static_cast<uint8_t>(value));
            };
            const bool udp = pattern == "dns", control = pattern == "icmp";
            if (udp) {
                body = {0x12, 0x34, 1,   0,   0,   1,   0, 0,   0,   0,   0, 0, 1, 'a', 7, 'e',
                        'x',  'a',  'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0,   1};
            } else if (pattern == "http1") {
                const std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
                body.assign(request.begin(), request.end());
            } else if (pattern == "tls") {
                // TLS record + a complete TLS 1.2 ClientHello without extensions.
                body = {22, 3, 3, 0, 45, 1, 0, 0, 41};
                u16(0x0303);
                body.insert(body.end(), 32, 'c');
                body.push_back(0);
                u16(2);
                u16(0x1301);
                body.push_back(1);
                body.push_back(0);
            } else {
                body.assign(16, 'e');
            }
            const size_t header = udp || control ? 8 : 20;
            packet_bytes->resize(std::max<size_t>(96, tcp + header + body.size()), 0);
            if (udp || control) {
                std::fill(packet_bytes->begin() + tcp, packet_bytes->begin() + tcp + header, 0);
                if (udp) {
                    (*packet_bytes)[tcp] = static_cast<uint8_t>(port >> 8);
                    (*packet_bytes)[tcp + 1] = static_cast<uint8_t>(port);
                    (*packet_bytes)[tcp + 3] = 53;
                    const auto length = static_cast<uint16_t>(header + body.size());
                    (*packet_bytes)[tcp + 4] = static_cast<uint8_t>(length >> 8);
                    (*packet_bytes)[tcp + 5] = static_cast<uint8_t>(length);
                } else {
                    (*packet_bytes)[tcp] = six ? 128 : 8;
                    (*packet_bytes)[tcp + 4] = static_cast<uint8_t>(port >> 8);
                    (*packet_bytes)[tcp + 5] = static_cast<uint8_t>(port);
                    (*packet_bytes)[tcp + 7] = 1;
                }
            } else {
                (*packet_bytes)[tcp + 2] = pattern == "http1" ? 0 : 1;
                (*packet_bytes)[tcp + 3] = pattern == "http1" ? 80 : 187;
                // These consumers accept stream parsing only after a known SYN origin.
                (*packet_bytes)[tcp + 13] = 2;
            }
            const uint16_t length = static_cast<uint16_t>((six ? 0 : 20) + header + body.size());
            const size_t ip_length = six ? 18 : 16;
            (*packet_bytes)[ip_length] = static_cast<uint8_t>(length >> 8);
            (*packet_bytes)[ip_length + 1] = static_cast<uint8_t>(length);
            (*packet_bytes)[six ? 20 : 23] = udp ? 17 : control ? (six ? 58 : 1) : 6;
            std::copy(body.begin(), body.end(), packet_bytes->begin() + tcp + header);
        }
        if (pattern == "multiaddr") {
            (*packet_bytes)[six ? 37 : 29] = static_cast<uint8_t>(1 + i % 32);
            (*packet_bytes)[six ? 53 : 33] = static_cast<uint8_t>(100 + i % 32);
        }
        if (pattern == "bidir" && i % 2) {
            const size_t a = six ? 22 : 26, b = six ? 38 : 30, width = six ? 16 : 4;
            for (size_t j = 0; j < width; ++j) std::swap((*packet_bytes)[a + j], (*packet_bytes)[b + j]);
            std::swap((*packet_bytes)[tcp], (*packet_bytes)[tcp + 2]);
            std::swap((*packet_bytes)[tcp + 1], (*packet_bytes)[tcp + 3]);
        }
        if (wire_digest) {
            for (uint8_t value : *packet_bytes) {
                *wire_digest ^= value;
                *wire_digest *= 1099511628211ULL;
            }
        }
        auto& record = records[static_cast<size_t>(i)];
        record.meta.timestamp_ns = time_base + static_cast<int64_t>(i) * time_step;
        record.meta.captured_len = static_cast<uint32_t>(packet_bytes->size());
        record.meta.wire_len = record.meta.captured_len;
        record.meta.link_type = 1;
        record.meta.source_id = static_cast<uint32_t>(i % source_count);
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
        Require(record.layer.status == flowsql::packet::LayerStatus::kDecoded &&
                    (record.layer.ports_valid || pattern == "icmp"),
                "source-stage layer decoding failed");
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto status = flowsql::testing::EncodePacketBatch(records, &batch, &error);
    Require(status == flowsql::packet::PacketBatchError::kNone, "input encoding failed: " + error);
    return batch;
}

uint64_t DigestOutputs(const std::vector<flowsql::BlockTransformOutputV1>& outputs,
                       const std::shared_ptr<arrow::Schema>& schema, uint64_t* digest) {
    uint64_t count = 0;
    const auto entity = schema->metadata()->Get("flowsql.entity").ValueOrDie();
    const int generated_clock_column =
        entity == "basic_ipv4" || entity == "basic_ipv6" ? schema->GetFieldIndex("observed_at") : -1;
    for (const auto& output : outputs) {
        Require(output.batch && output.batch->schema()->Equals(*schema, true), "output schema changed");
        Require(output.batch->ValidateFull().ok(), "output validation failed");
        for (int64_t row = 0; row < output.batch->num_rows(); ++row) {
            for (int index = 0; index < output.batch->num_columns(); ++index) {
                const auto scalar = output.batch->column(index)->GetScalar(row).ValueOrDie();
                // Offline Basic Emit replaces observed_at with the real generation wall clock.
                // Protocol capture timestamps and every other business field remain in the digest.
                const bool generated_clock = index == generated_clock_column;
                if (generated_clock)
                    Require(scalar->is_valid && std::static_pointer_cast<arrow::Int64Scalar>(scalar)->value > 0,
                            "invalid generated Basic wall clock");
                const auto value = generated_clock    ? "<GENERATION_WALL_CLOCK>"
                                   : scalar->is_valid ? scalar->ToString()
                                                      : "<NULL>";
                *digest ^= scalar->is_valid ? 1 : 0;
                *digest *= 1099511628211ULL;
                for (unsigned char c : value) {
                    *digest ^= c;
                    *digest *= 1099511628211ULL;
                }
                *digest ^= 255;
                *digest *= 1099511628211ULL;
            }
            ++count;
        }
    }
    return count;
}

// Runs after timing: compare all persisted entity fields, including non-observing families.
void VerifyManagedReadback(flowsql::IDatabaseChannel* channel, const std::string& result_json) {
    rapidjson::Document result;
    result.Parse(result_json.c_str());
    Require(result.IsObject() && result.HasMember("entities") && result.HasMember("run_id"),
            "invalid managed result metadata");
    Require(std::string_view(result["run_status"].GetString()) == "completed" &&
                std::string_view(result["metadata_status"].GetString()) == "known",
            "managed run did not complete with known metadata");
    for (const auto& entity : result["entities"].GetArray()) {
        const std::string id = entity["entity_id"].GetString();
        const std::string sql = "SELECT * FROM npm_" + id + "_v1_data WHERE __npm_run_id='" +
                                result["run_id"].GetString() + "' ORDER BY session_id,revision";
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (std::string_view(channel->Category()) == "clickhouse") {
            Require(channel->ExecuteQueryArrow(sql.c_str(), &batches) == 0, "managed readback failed");
        } else {
            flowsql::IBatchReader* raw = nullptr;
            Require(channel->CreateReader(sql.c_str(), &raw) == 0 && raw, "managed reader unavailable");
            const auto release = [](flowsql::IBatchReader* reader) {
                reader->Close();
                reader->Release();
            };
            std::unique_ptr<flowsql::IBatchReader, decltype(release)> reader(raw, release);
            for (;;) {
                const uint8_t* data = nullptr;
                size_t size = 0;
                const int rc = reader->Next(&data, &size);
                if (rc == 1) break;
                Require(rc == 0, "managed reader Next failed");
                auto buffer = arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), size));
                auto input = std::make_shared<arrow::io::BufferReader>(buffer);
                auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
                for (;;) {
                    auto batch = stream->Next().ValueOrDie();
                    if (!batch) break;
                    batches.push_back(std::move(batch));
                }
            }
        }
        uint64_t digest = 1469598103934665603ULL, count = 0;
        const auto hash = [&](std::string_view value) {
            for (unsigned char c : value) {
                digest ^= c;
                digest *= 1099511628211ULL;
            }
            digest ^= 255;
            digest *= 1099511628211ULL;
        };
        for (const auto& batch : batches) {
            Require(batch->ValidateFull().ok(), "invalid persisted Arrow result");
            for (int64_t row = 0; row < batch->num_rows(); ++row) {
                for (int column = 0; column < batch->num_columns(); ++column) {
                    const auto& name = batch->schema()->field(column)->name();
                    if (name == "__npm_run_id") continue;
                    const auto scalar = batch->column(column)->GetScalar(row).ValueOrDie();
                    hash(name);
                    if (name == "observed_at" && id.compare(0, 6, "basic_") == 0) {
                        Require(scalar->is_valid && std::stoll(scalar->ToString()) > 0,
                                "invalid persisted Basic clock");
                        hash("<GENERATION_WALL_CLOCK>");
                    } else {
                        hash(scalar->is_valid ? scalar->ToString() : "<NULL>");
                    }
                }
                ++count;
            }
        }
        Require(count == static_cast<uint64_t>(entity["rows_written"].GetInt64()), "persisted entity count differs");
        std::cout << "managed_readback," << id << ',' << count << ',' << digest << '\n';
    }
}

double Milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

class BenchmarkConfigRegistry final : public flowsql::IConfigChannelRegistryV1 {
 public:
    int Resolve(const char* reference, flowsql::ConfigChannelSnapshot* output, std::string* error) override {
        Require(reference && std::string_view(reference) == "config.throughput@1" && output,
                "unexpected label snapshot reference");
        flowsql::ConfigChannelSnapshot snapshot{};
        snapshot.channel_name = "throughput";
        snapshot.revision = 1;
        snapshot.format = "yaml";
        snapshot.schema_id = "flowsql.io/flow-labeling/v1alpha1";
        snapshot.sha256_hex = "frozen-in-benchmark-source";
        snapshot.content = std::make_shared<const std::string>(R"YAML(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 8388608
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 64
      reject_duplicate_label_priorities: true
  labels:
    - id: 1001
      priority: 3000
      name: throughput-fixture
  rules:
    - id: all-ports
      label_id: 1001
      direction: bidirectional
      matches:
        - field: destination_port
          min: 1
          max: 65535
)YAML");
        snapshot.content_bytes = snapshot.content->size();
        *output = std::move(snapshot);
        if (error) error->clear();
        return 0;
    }
};

class BenchmarkQuerier final : public flowsql::IQuerier {
 public:
    BenchmarkQuerier(flowsql::IQuerier* delegate, BenchmarkConfigRegistry* registry)
        : delegate_(delegate), registry_(registry) {}
    int Traverse(const flowsql::Guid& iid, flowsql::IQuerier::fntraverse callback) override {
        return delegate_->Traverse(iid, callback);
    }
    void* First(const flowsql::Guid& iid) override {
        const auto& registry_id = flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1;
        return !(iid < registry_id) && !(registry_id < iid) ? registry_ : delegate_->First(iid);
    }

 private:
    flowsql::IQuerier* delegate_;
    BenchmarkConfigRegistry* registry_;
};

std::string QuoteJson(std::string_view text) {
    std::string output = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') output += '\\';
        output += c;
    }
    return output + '"';
}

// Optional cost probe forwards the production database API and keeps timing out of frontend runs.
class DatabaseCostProbe final : public flowsql::IDatabaseChannel,
                                public flowsql::IDatabasePreparedCommandV1,
                                public flowsql::IDatabaseArrowWriteV1 {
 public:
    explicit DatabaseCostProbe(flowsql::IDatabaseChannel* channel) : channel_(channel) {
        prepared_ = dynamic_cast<flowsql::IDatabasePreparedCommandV1*>(channel_);
        Require(prepared_ != nullptr, "database prepared command interface missing");
    }
    const char* Category() override { return channel_->Category(); }
    const char* Name() override { return channel_->Name(); }
    const char* Type() override { return channel_->Type(); }
    const char* Schema() override { return channel_->Schema(); }
    int Open() override { return channel_->Open(); }
    int Close() override { return channel_->Close(); }
    bool IsOpened() const override { return channel_->IsOpened(); }
    int Flush() override { return channel_->Flush(); }
    int CreateReader(const char* query, flowsql::IBatchReader** reader) override {
        return channel_->CreateReader(query, reader);
    }
    int CreateWriter(const char* table, flowsql::IBatchWriter** writer) override {
        return channel_->CreateWriter(table, writer);
    }
    int CreateArrowReader(const char* query, flowsql::IArrowReader** reader) override {
        return channel_->CreateArrowReader(query, reader);
    }
    int CreateArrowWriter(const char* table, flowsql::IArrowWriter** writer) override {
        return channel_->CreateArrowWriter(table, writer);
    }
    int ExecuteQueryArrow(const char* query, std::vector<std::shared_ptr<arrow::RecordBatch>>* rows) override {
        return channel_->ExecuteQueryArrow(query, rows);
    }
    int WriteArrowBatches(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& rows) override {
        return channel_->WriteArrowBatches(table, rows);
    }
    int WriteArrowBatchesWithPool(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& rows,
                                  arrow::MemoryPool* pool) override {
        ++arrow_calls_;
        for (const auto& batch : rows) {
            arrow_rows_ += batch->num_rows();
            auto bytes = arrow::util::ReferencedBufferSize(*batch);
            Require(bytes.ok(), "Arrow logical byte measurement failed");
            arrow_bytes_ += *bytes;
        }
        auto* writer = dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(channel_);
        Require(writer != nullptr, "budgeted Arrow capability missing");
        const auto started = std::chrono::steady_clock::now();
        const int result = writer->WriteArrowBatchesWithPool(table, rows, pool);
        arrow_ms_ += Milliseconds(started);
        return result;
    }
    int ExecuteSql(const char* sql) override {
        ++sql_calls_;
        return channel_->ExecuteSql(sql);
    }
    const char* GetLastError() override { return channel_->GetLastError(); }
    bool IsConnected() override { return channel_->IsConnected(); }
    int ExecutePrepared(const char* sql, const flowsql::DatabaseParameterV1* parameters, size_t count) override {
        ++single_calls_;
        return Measure([&] { return prepared_->ExecutePrepared(sql, parameters, count); });
    }
    int ExecutePreparedBatch(const char* sql, const flowsql::DatabaseParameterV1* parameters, size_t width,
                             size_t rows) override {
        ++batch_calls_;
        batch_rows_ += rows;
        parameter_bytes_ += width * rows * sizeof(flowsql::DatabaseParameterV1);
        return Measure([&] { return prepared_->ExecutePreparedBatch(sql, parameters, width, rows); });
    }
    void BeginMeasurement() {
        sql_calls_ = single_calls_ = batch_calls_ = batch_rows_ = parameter_bytes_ = heap_peak_ = 0;
        prepared_ms_ = 0;
        arrow_calls_ = arrow_rows_ = arrow_bytes_ = 0;
        arrow_ms_ = 0;
    }
    void Report() const {
        std::cout << "database_cost,{\"sql_calls\":" << sql_calls_ << ",\"prepared_calls\":" << single_calls_
                  << ",\"prepared_batch_calls\":" << batch_calls_ << ",\"prepared_batch_rows\":" << batch_rows_
                  << ",\"parameter_slot_bytes_total\":" << parameter_bytes_ << ",\"prepared_ms\":" << prepared_ms_
                  << ",\"heap_at_driver_boundary_max\":" << heap_peak_ << ",\"arrow_write_calls\":" << arrow_calls_
                  << ",\"arrow_write_rows\":" << arrow_rows_ << ",\"arrow_logical_bytes\":" << arrow_bytes_
                  << ",\"arrow_write_ms\":" << arrow_ms_ << "}\n";
    }

 private:
    template <typename Callback>
    int Measure(Callback call) {
        SampleHeap();
        const auto started = std::chrono::steady_clock::now();
        const int rc = call();
        prepared_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        SampleHeap();
        return rc;
    }
    void SampleHeap() {
        const auto heap = mallinfo2();
        heap_peak_ = std::max(heap_peak_, static_cast<uint64_t>(heap.uordblks + heap.hblkhd));
    }
    flowsql::IDatabaseChannel* channel_;
    flowsql::IDatabasePreparedCommandV1* prepared_;
    uint64_t sql_calls_ = 0, single_calls_ = 0, batch_calls_ = 0, batch_rows_ = 0, parameter_bytes_ = 0, heap_peak_ = 0;
    double prepared_ms_ = 0;
    uint64_t arrow_calls_ = 0, arrow_rows_ = 0, arrow_bytes_ = 0;
    double arrow_ms_ = 0;
};

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
                  std::string_view family, std::string_view pattern, int namespace_length, int cardinality,
                  int64_t time_step, std::string_view mode, int source_count,
                  flowsql::IDatabaseChannel* managed_channel = nullptr) {
    // The source and production plugins belong to the frozen workspace. Only this harness is added.
    double decode_ms = 0, process_ms = 0, validation_ms = 0, release_ms = 0, input_release_ms = 0, flush_ms = 0;
    uint64_t digest = 14695981039346656037ULL, output_rows = 0, input_capacity = 0, heap_peak = 0;
    uint64_t wire_digest = 14695981039346656037ULL;
    BenchmarkConfigRegistry registry;
    BenchmarkQuerier querier(loader, &registry);
    flowsql::test::ScopedSharedLibrary library(FLOWSQL_NPM_BASIC_PLUGIN_PATH);
    Require(static_cast<bool>(library), "npm.basic library loading failed");
    const auto create = reinterpret_cast<flowsql::CppOperatorPluginCreateCapabilityV2Fn>(
        library.Symbol(flowsql::kCppOperatorPluginCreateCapabilityV2Symbol));
    BenchmarkCapability capability;
    capability.destroy_capability = reinterpret_cast<flowsql::CppOperatorPluginDestroyCapabilityV2Fn>(
        library.Symbol(flowsql::kCppOperatorPluginDestroyCapabilityV2Symbol));
    Require(create && capability.destroy_capability, "npm.basic capability symbols missing");
    capability.provider = static_cast<flowsql::IBlockTransformOperatorV1*>(create(0, &querier));
    Require(capability.provider != nullptr, "capability creation failed");
    const auto input =
        MakeInput(batch_rows, protocol, family, pattern, cardinality, 1000000000, 0, source_count, &wire_digest);
    std::string domains;
    for (int i = 0; i < source_count; ++i) {
        if (i) domains += ';';
        domains += std::to_string(i) + ':' + std::to_string(i + 7);
    }
    const bool protocol_pattern = pattern == "dns" || pattern == "http1" || pattern == "tls" || pattern == "icmp";
    const std::string entity_base = !protocol_pattern    ? "basic"
                                    : pattern == "dns"   ? "dns_transaction"
                                    : pattern == "http1" ? "http1_transaction"
                                    : pattern == "tls"   ? "tls_handshake"
                                                         : "icmp_event";
    const std::string entity = entity_base + (family == "ipv6" ? "_ipv6" : "_ipv4");
    const bool labeling = (protocol_pattern && pattern != "icmp") || mode == "labels-zero" || mode == "labels-dense";
    const std::string features = (mode == "basic" ? "basic" : "basic,session") +
                                 (protocol_pattern ? "," + std::string(pattern) : "") + (labeling ? ",labeling" : "");
    const bool periodic = mode == "periodic";
    std::string parameters = "{\"schema_version\":1,\"core\":{\"result_mode\":\"" +
                             std::string(periodic ? "periodic_snapshot" : "final") +
                             "\",\"output_interval_ns\":10000000";
    if (labeling) parameters += ",\"labeling\":\"config.throughput@1\"";
    parameters += '}';
    if (protocol_pattern && pattern != "icmp")
        parameters += ",\"" + std::string(pattern) + "\":{\"primary_label_ids\":[1001]}";
    parameters += '}';
    const std::string params = "{\"input_namespace\":\"" + std::string(namespace_length, 'n') +
                               "\",\"source_domains\":\"" + domains + "\",\"observing\":\"" + entity +
                               "\",\"features\":\"" + features + "\",\"parameters\":" + QuoteJson(parameters) + '}';
    flowsql::BlockTransformTaskConfigV1 config;
    config.task_id = "throughput-t1";
    config.with_params_json = params.c_str();
    config.pushed_filter_plan_json = R"({"version":1,"root":null})";
    Require(capability.provider->CreateTask(config, &capability.task) == 0 && capability.task, "CreateTask failed");
    flowsql::IBlockTransformManagedSinkTaskV1* managed_task = nullptr;
    std::string managed_target;
    if (managed_channel) {
        managed_task = dynamic_cast<flowsql::IBlockTransformManagedSinkTaskV1*>(capability.task);
        Require(managed_task != nullptr, "managed sink task interface missing");
        managed_target = std::string(managed_channel->Category()) + '.' + managed_channel->Name();
        flowsql::BlockTransformManagedSinkBindingV1 binding;
        binding.sink_channel = managed_channel;
        binding.target = managed_target.c_str();
        binding.category = managed_channel->Category();
        binding.name = managed_channel->Name();
        binding.relation = "";
        Require(managed_task->BindManagedSink(binding) == 0, "managed sink binding failed");
    }
    std::shared_ptr<arrow::Schema> schema;
    const int open_rc = capability.task->Open(input->schema(), &schema);
    Require(open_rc == 0, "Open failed: " + capability.task->LastError());
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    // Warm-up belongs to the same task. ACK cardinality is established by this batch.
    Require(capability.task->ProcessBlock(input, 1000, &outputs) ==
                static_cast<int>(flowsql::BlockTransformStatusV1::kContinue),
            "warm-up failed");
    output_rows += DigestOutputs(outputs, schema, &digest);
    outputs.clear();
    if (auto* probe = dynamic_cast<DatabaseCostProbe*>(managed_channel)) probe->BeginMeasurement();
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    const uint64_t packets = static_cast<uint64_t>(batch_rows) * iterations;
    for (int i = 0; i < iterations; ++i) {
        auto started = std::chrono::steady_clock::now();
        const int64_t base = 1000000000LL + (i + 1LL) * batch_rows * time_step;
        auto batch = MakeInput(batch_rows, protocol, family, pattern, cardinality, base, time_step, source_count);
        decode_ms += Milliseconds(started);  // NPI + input materialization, separately reported.
        input_capacity = std::max(input_capacity, BufferBytes(*batch));
        started = std::chrono::steady_clock::now();
        const int rc = capability.task->ProcessBlock(batch, 1000, &outputs);
        process_ms += Milliseconds(started);
        Require(rc == static_cast<int>(flowsql::BlockTransformStatusV1::kContinue),
                "ProcessBlock failed: " + capability.task->LastError());
        started = std::chrono::steady_clock::now();
        output_rows += DigestOutputs(outputs, schema, &digest);
        validation_ms += Milliseconds(started);
        const auto heap = mallinfo2();
        heap_peak = std::max(heap_peak, static_cast<uint64_t>(heap.uordblks + heap.hblkhd));
        started = std::chrono::steady_clock::now();
        outputs.clear();
        release_ms += Milliseconds(started);
        started = std::chrono::steady_clock::now();
        batch.reset();
        input_release_ms += Milliseconds(started);
    }
    auto started = std::chrono::steady_clock::now();
    Require(capability.task->Flush(&outputs) == 0, "Flush failed: " + capability.task->LastError());
    flush_ms = Milliseconds(started);
    const auto final_heap = mallinfo2();
    heap_peak = std::max(heap_peak, static_cast<uint64_t>(final_heap.uordblks + final_heap.hblkhd));
    started = std::chrono::steady_clock::now();
    output_rows += DigestOutputs(outputs, schema, &digest);
    validation_ms += Milliseconds(started);
    started = std::chrono::steady_clock::now();
    outputs.clear();
    release_ms += Milliseconds(started);
    getrusage(RUSAGE_SELF, &after);
    if (!periodic && !protocol_pattern) {
        const uint64_t expected =
            pattern == "rst"
                ? static_cast<uint64_t>(batch_rows) * (iterations + 1)
                : static_cast<uint64_t>(std::min(pattern == "bidir" ? batch_rows / 2 : batch_rows, cardinality));
        const uint64_t observed = family == "mixed" ? (expected + 1) / 2 : expected;
        Require(output_rows == observed, "foreground row count mismatch");
    }
    if (protocol_pattern) Require(output_rows > 0, "protocol fixture produced no observed records");
    const auto cpu = [](const rusage& usage) {
        return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
               (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
    };
    const double primary = process_ms + flush_ms + release_ms;
    const double historical = primary + validation_ms;
    const double end_to_end = historical + decode_ms + input_release_ms;
    std::cout << "measurement,family,pattern,mode,packets,batch_rows,namespace_bytes,key_cardinality,time_step_ns,"
                 "source_count,process_ms,flush_ms,release_ms,validation_ms,decode_ms,primary_pps,historical_pps,"
                 "end_to_end_pps,foreground_rows,business_digest,cpu_ms,heap_phase_peak,rss_peak_bytes,input_capacity,"
                 "wire_fixture_digest,input_release_ms\n";
    std::cout << std::fixed << std::setprecision(6) << "measurement," << family << ',' << pattern << ',' << mode << ','
              << packets << ',' << batch_rows << ',' << namespace_length << ',' << cardinality << ',' << time_step
              << ',' << source_count << ',' << process_ms << ',' << flush_ms << ',' << release_ms << ','
              << validation_ms << ',' << decode_ms << ',' << packets * 1000 / primary << ','
              << packets * 1000 / historical << ',' << packets * 1000 / end_to_end << ',' << output_rows << ','
              << digest << ',' << cpu(after) - cpu(before) << ',' << heap_peak << ',' << after.ru_maxrss * 1024 << ','
              << input_capacity << ',' << wire_digest << ',' << input_release_ms << '\n';
    if (managed_task) {
        const auto result_json = managed_task->ManagedSinkResultJson();
        std::cout << "managed_result," << result_json << '\n';
        VerifyManagedReadback(managed_channel, result_json);
    }
}

class LookupProbeBudget final : public flowsql::npm::INpmTaskBudget {
 public:
    flowsql::npm::NpmBudgetError Reserve(flowsql::npm::NpmBudgetCategory category, uint64_t bytes) override {
        return flowsql::npm::ReserveNpmBudget({}, category, bytes, &usage_);
    }
    flowsql::npm::NpmBudgetError Release(flowsql::npm::NpmBudgetCategory category, uint64_t bytes) override {
        return flowsql::npm::ReleaseNpmBudget(category, bytes, &usage_);
    }
    flowsql::npm::NpmBudgetUsage Usage() const override { return usage_; }

 private:
    flowsql::npm::NpmBudgetUsage usage_;
};

void RunLookupProbe(int rows, int iterations, int source_count, bool progress) {
    using namespace flowsql;
    npm::NpmObservationDomainMap domains;
    domains.input_namespace = "lookup.namespace";
    CaptureSourceSetV2 sources;
    for (int i = 0; i < source_count; ++i) {
        domains.bindings.push_back({static_cast<uint32_t>(i), static_cast<uint64_t>(i + 7)});
        CaptureQueueIdentityV1 identity;
        identity.source_id = static_cast<uint32_t>(i);
        identity.generation = 1;
        identity.source_name = "benchmark.capture";
        identity.observation_domain_id = 7;
        identity.link_type = 1;
        sources.inputs.push_back(identity);
    }
    Require(npm::ValidateNpmObservationDomainMap(domains) == npm::NpmObservationDomainError::kNone,
            "invalid domain probe configuration");
    std::unique_ptr<npm::NpmObservationDomainLookup> lookup;
    Require(npm::NpmObservationDomainLookup::Create(domains, std::make_shared<LookupProbeBudget>(), &lookup) ==
                npm::NpmObservationDomainLookupError::kNone,
            "domain lookup creation failed");
    CaptureProgressTrackerV2 tracker(sources);
    std::vector<CaptureProgressV1> facts(static_cast<size_t>(source_count));
    for (int i = 0; i < source_count; ++i) {
        facts[i].source_id = static_cast<uint32_t>(i);
        facts[i].generation = 1;
        facts[i].source_idle_confirmed = true;
        facts[i].backlog = CaptureBacklogV1::kEmpty;
    }
    if (progress) {
        for (auto& fact : facts) {
            fact.fact_sequence = 1;
            fact.capture_time_ns = 1;
        }
        auto warm = tracker;
        Require(warm.Observe(facts) == CaptureProgressErrorV1::kNone && warm.CommonCandidateNs() == 1,
                "capture probe warm-up failed");
    } else {
        uint64_t warm_domain = 0;
        Require(lookup->Resolve(static_cast<uint32_t>(source_count - 1), &warm_domain) ==
                    npm::NpmObservationDomainError::kNone,
                "lookup probe warm-up failed");
    }
    uint64_t digest = 0;
    const int calls = rows * iterations;
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) {
        if (progress) {
            for (auto& fact : facts) {
                fact.fact_sequence = static_cast<uint64_t>(i + 1);
                fact.capture_time_ns = i + 1;
            }
            // Mirrors AcceptCaptureFacts' outer transactional copy, then Observe's input-state copy.
            auto candidate = tracker;
            Require(candidate.Observe(facts) == CaptureProgressErrorV1::kNone, "capture-fact acceptance failed");
            tracker = std::move(candidate);
            const auto watermark = tracker.CommonCandidateNs();
            Require(watermark && *watermark == i + 1, "common progress mismatch");
            digest += static_cast<uint64_t>(*watermark);
        } else {
            uint64_t domain = 0;
            Require(lookup->Resolve(static_cast<uint32_t>(source_count - 1), &domain) ==
                        npm::NpmObservationDomainError::kNone,
                    "domain lookup failed");
            Require(domain == static_cast<uint64_t>(source_count + 6), "domain lookup value differs");
            digest += domain;
        }
    }
    const double ms = Milliseconds(start);
    getrusage(RUSAGE_SELF, &after);
    const auto heap = mallinfo2();
    const double cpu_ms =
        (after.ru_utime.tv_sec + after.ru_stime.tv_sec - before.ru_utime.tv_sec - before.ru_stime.tv_sec) * 1000.0 +
        (after.ru_utime.tv_usec + after.ru_stime.tv_usec - before.ru_utime.tv_usec - before.ru_stime.tv_usec) / 1000.0;
    std::cout << "measurement,family,pattern,mode,packets,batch_rows,namespace_bytes,key_cardinality,time_step_ns,"
                 "source_count,"
                 "process_ms,flush_ms,release_ms,validation_ms,decode_ms,primary_pps,historical_pps,end_to_end_pps,"
                 "foreground_rows,"
                 "business_digest,cpu_ms,heap_phase_peak,rss_peak_bytes,input_capacity,wire_fixture_digest\n";
    std::cout << std::fixed << std::setprecision(6) << "measurement,none," << (progress ? "progress" : "domain")
              << ",probe," << calls << ',' << rows << ",16,0,1," << source_count << ',' << ms << ",0,0,0,0,"
              << calls * 1000.0 / ms << ',' << calls * 1000.0 / ms << ',' << calls * 1000.0 / ms << ",0," << digest
              << ',' << cpu_ms << ',' << heap.uordblks + heap.hblkhd << ',' << after.ru_maxrss * 1024 << ",0,"
              << source_count << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    auto* loader = flowsql::PluginLoader::Single();
    try {
        Require(
            argc == 10 || argc == 13 || argc == 14,
            "usage: benchmark_npm_throughput rows iterations family rst|ack|payload namespace_len cardinality "
            "time_step_ns basic|session|periodic source_count [database-options-file category channel-name [cost]]");
        const int batch_rows = ParsePositive(argv[1], 30000);
        const int iterations = ParsePositive(argv[2], 10000);
        const std::string family = argv[3], pattern = argv[4], mode = argv[8];
        const int namespace_length = ParsePositive(argv[5], 256), cardinality = ParsePositive(argv[6], 30000);
        const int64_t time_step = std::stoll(argv[7]);
        const int source_count = ParsePositive(argv[9], 256);
        Require(family == "ipv4" || family == "ipv6" || family == "mixed", "invalid family");
        Require(pattern == "rst" || pattern == "ack" || pattern == "payload" || pattern == "bidir" ||
                    pattern == "multiaddr" || pattern == "domain" || pattern == "progress" || pattern == "dns" ||
                    pattern == "http1" || pattern == "tls" || pattern == "icmp",
                "invalid pattern");
        Require(mode == "basic" || mode == "session" || mode == "periodic" || mode == "labels-zero" ||
                    mode == "labels-dense",
                "invalid mode");
        Require(time_step >= 0 && time_step <= 10000000, "invalid time step");
        Require(cardinality % 2 == 0 || family != "mixed", "mixed requires even cardinality");
        if (pattern == "domain" || pattern == "progress") {
            RunLookupProbe(batch_rows, iterations, source_count, pattern == "progress");
            return 0;
        }
        const std::string npi_option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\"}";
        std::vector<const char*> libraries{FLOWSQL_NPI_PLUGIN_PATH}, options{npi_option.c_str()};
        const bool labeling = (pattern == "dns" || pattern == "http1" || pattern == "tls") || mode == "labels-zero" ||
                              mode == "labels-dense";
        if (labeling) {
#ifdef FLOWSQL_FLOW_LABELING_PLUGIN_PATH
            libraries.push_back(FLOWSQL_FLOW_LABELING_PLUGIN_PATH);
            options.push_back(nullptr);
#else
            throw std::runtime_error("frozen label/protocol workload requires the real flow-labeling plugin");
#endif
        }
        std::string database_options;
        if (argc >= 13) {
#ifdef FLOWSQL_DATABASE_PLUGIN_PATH
            std::ifstream input_options(argv[10]);
            Require(input_options.good(), "database options file unavailable");
            database_options.assign(std::istreambuf_iterator<char>(input_options), {});
            libraries.push_back(FLOWSQL_DATABASE_PLUGIN_PATH);
            options.push_back(database_options.c_str());
#else
            throw std::runtime_error("managed measurement requires the database benchmark target");
#endif
        }
        Require(loader->Load(".", libraries.data(), options.data(), static_cast<int>(libraries.size())) == 0,
                "source/label plugin loading failed");
        Require(loader->StartAll() == 0, "plugin start failed");
        auto* protocol = static_cast<flowsql::IProtocol*>(loader->First(flowsql::IID_PROTOCOL));
        Require(protocol != nullptr, "NPI protocol interface not registered");
        std::shared_ptr<flowsql::IDatabaseChannel> channel;
        if (argc >= 13) {
            auto* leases = static_cast<flowsql::IDatabaseChannelLeaseProviderV1*>(
                loader->First(flowsql::IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
            Require(leases != nullptr, "database lease interface missing");
            channel = leases->AcquireChannel(argv[11], argv[12]);
            Require(channel && channel->IsOpened() && channel->IsConnected(), "measurement database unavailable");
        }
        std::unique_ptr<DatabaseCostProbe> cost;
        if (argc == 14) {
            Require(std::string_view(argv[13]) == "cost", "invalid database probe mode");
            cost = std::make_unique<DatabaseCostProbe>(channel.get());
        }
        RunBenchmark(batch_rows, iterations, loader, protocol, family, pattern, namespace_length, cardinality,
                     time_step, mode, source_count, cost ? cost.get() : channel.get());
        if (cost) cost->Report();
        cost.reset();
        channel.reset();
        loader->StopAll();
        loader->Unload();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_npm_throughput failed: " << error.what() << '\n';
        loader->StopAll();
        loader->Unload();
        return 1;
    }
}
