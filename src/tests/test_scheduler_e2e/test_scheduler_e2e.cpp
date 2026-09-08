// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <common/loader.hpp>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/filter_planner.h>
#include <framework/core/packet_codec.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/interfaces/iblock_stream_operator.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_operator.h>
#include <plugins/npi/iprotocol.h>

using namespace flowsql;

#define ASSERT_TRUE(expr)                                                                   \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #expr);                   \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)                                                                     \
    do {                                                                                    \
        auto _a = (a);                                                                      \
        auto _b = (b);                                                                      \
        if (!(_a == _b)) {                                                                  \
            std::printf("[FAIL] %s:%d %s != %s\n", __FILE__, __LINE__, #a, #b);            \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

class SchedulerE2eProtocol final : public IProtocol {
 public:
    void Concurrency(int32_t) override {}
    protocol::Protocol Identify(int32_t, const uint8_t*, int32_t, const protocol::Layers*) override {
        return {};
    }
    int32_t Layer(int32_t, const uint8_t* data, int32_t size, protocol::Layers* layers) override {
        ++layer_calls;
        if (!data || size <= 0 || !layers) return -1;
        *layers = {};
        return 0;
    }
    protocol::IDictionary* Dictionary() override { return nullptr; }

    void Reset() { layer_calls = 0; }

    int layer_calls = 0;
};

class SchedulerE2eBlockOperator final : public IBlockStreamOperator {
 public:
    std::string Category() override { return "test"; }
    std::string Name() override { return "packet_counter"; }
    std::string Description() override { return "scheduler pcapfile E2E packet counter"; }
    int Configure(const char*, const char*) override { return 0; }
    int Init(const char*) override {
        ++init_calls;
        return 0;
    }
    int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) override {
        ++schema_calls;
        schema_matches_packet = schema && schema->Equals(packet::PacketSchema());
        return schema_matches_packet ? 0 : EINVAL;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& batch, int64_t) override {
        ++process_calls;
        if (!batch) return EINVAL;
        auto sequence = std::dynamic_pointer_cast<arrow::UInt64Array>(batch->GetColumnByName("sequence"));
        auto raw = std::dynamic_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("raw_data"));
        if (!sequence || !raw || sequence->length() != batch->num_rows() || raw->length() != batch->num_rows()) {
            return EINVAL;
        }
        rows_seen += batch->num_rows();
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            sequences.push_back(sequence->Value(row));
            const auto bytes = raw->GetView(row);
            raw_packets.emplace_back(bytes.data(), bytes.size());
        }
        return 0;
    }
    int Flush() override {
        ++flush_calls;
        return 0;
    }
    std::string LastError() override { return "scheduler pcapfile E2E operator failed"; }

    void Reset() {
        init_calls = 0;
        schema_calls = 0;
        process_calls = 0;
        flush_calls = 0;
        rows_seen = 0;
        schema_matches_packet = false;
        sequences.clear();
        raw_packets.clear();
    }

    int init_calls = 0;
    int schema_calls = 0;
    int process_calls = 0;
    int flush_calls = 0;
    int64_t rows_seen = 0;
    bool schema_matches_packet = false;
    std::vector<uint64_t> sequences;
    std::vector<std::string> raw_packets;
};

static std::shared_ptr<arrow::Schema> SchedulerE2eTransformSchema() {
    return arrow::schema({
        arrow::field("sequence", arrow::uint64(), false),
        arrow::field("captured_len", arrow::uint32(), false),
        arrow::field("protocol", arrow::utf8(), false),
    });
}

enum class SchedulerE2eTransformKind {
    kPacketToProtocol,
    kPassthrough,
};

struct SchedulerE2eTransformSnapshot {
    int open_calls = 0;
    int process_calls = 0;
    int flush_calls = 0;
    int cancel_calls = 0;
    std::vector<int64_t> input_rows;
    bool input_schema_matched = false;
};

class SchedulerE2eTransformTask final : public IBlockTransformTaskV1 {
 public:
    explicit SchedulerE2eTransformTask(SchedulerE2eTransformKind kind) : kind_(kind) {}

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override {
        ++open_calls;
        if (!input_schema || !output_schema || open_calls != 1) return EINVAL;
        const auto expected = kind_ == SchedulerE2eTransformKind::kPacketToProtocol
                                  ? packet::PacketSchema()
                                  : SchedulerE2eTransformSchema();
        input_schema_matched = input_schema->Equals(*expected, true);
        if (!input_schema_matched) return EINVAL;
        *output_schema = SchedulerE2eTransformSchema();
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        ++process_calls;
        if (!input || !outputs || !outputs->empty()) return -EINVAL;
        input_rows.push_back(input->num_rows());
        if (kind_ == SchedulerE2eTransformKind::kPassthrough) {
            outputs->push_back({input, ts_ms});
            return static_cast<int>(BlockTransformStatusV1::kContinue);
        }

        auto sequence = std::dynamic_pointer_cast<arrow::UInt64Array>(
            input->GetColumnByName("sequence"));
        auto captured_len = std::dynamic_pointer_cast<arrow::UInt32Array>(
            input->GetColumnByName("captured_len"));
        if (!sequence || !captured_len || sequence->length() != input->num_rows() ||
            captured_len->length() != input->num_rows()) {
            last_error_ = "packet columns do not match the transform contract";
            return -EINVAL;
        }

        arrow::UInt64Builder sequence_builder;
        arrow::UInt32Builder captured_len_builder;
        arrow::StringBuilder protocol_builder;
        for (int64_t row = 0; row < input->num_rows(); ++row) {
            if (!sequence_builder.Append(sequence->Value(row)).ok() ||
                !captured_len_builder.Append(captured_len->Value(row)).ok() ||
                !protocol_builder.Append(sequence->Value(row) == 0 ? "DROP" : "HTTP").ok()) {
                last_error_ = "failed to build transform output";
                return -EIO;
            }
        }
        std::shared_ptr<arrow::Array> output_sequence;
        std::shared_ptr<arrow::Array> output_captured_len;
        std::shared_ptr<arrow::Array> output_protocol;
        if (!sequence_builder.Finish(&output_sequence).ok() ||
            !captured_len_builder.Finish(&output_captured_len).ok() ||
            !protocol_builder.Finish(&output_protocol).ok()) {
            last_error_ = "failed to finish transform output";
            return -EIO;
        }
        outputs->push_back({arrow::RecordBatch::Make(
                                SchedulerE2eTransformSchema(),
                                input->num_rows(),
                                {output_sequence, output_captured_len, output_protocol}),
                            ts_ms});
        return static_cast<int>(BlockTransformStatusV1::kContinue);
    }

    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        ++flush_calls;
        return outputs && outputs->empty() ? 0 : EINVAL;
    }

    void Cancel() override { ++cancel_calls; }
    std::string LastError() const override { return last_error_; }

    int open_calls = 0;
    int process_calls = 0;
    int flush_calls = 0;
    int cancel_calls = 0;
    std::vector<int64_t> input_rows;
    bool input_schema_matched = false;

 private:
    SchedulerE2eTransformKind kind_;
    std::string last_error_;
};

class SchedulerE2eTransformProvider final : public IBlockTransformOperatorV1 {
 public:
    SchedulerE2eTransformProvider(std::string name, SchedulerE2eTransformKind kind)
        : name_(std::move(name)), kind_(kind) {}

    std::string Category() const override { return "test"; }
    std::string Name() const override { return name_; }
    std::string Description() const override { return "scheduler cross-module E2E transform"; }

    int CreateTask(const BlockTransformTaskConfigV1& config,
                   IBlockTransformTaskV1** task) override {
        ++create_calls;
        if (!task || !config.task_id || !config.with_params_json ||
            !config.pushed_filter_plan_json ||
            config.contract_version != kBlockTransformContractVersionV1) {
            return EINVAL;
        }
        task_ids.emplace_back(config.task_id);
        with_params.emplace_back(config.with_params_json);
        pushed_plans.emplace_back(config.pushed_filter_plan_json);
        *task = new SchedulerE2eTransformTask(kind_);
        ++live_tasks;
        return 0;
    }

    void ReleaseTask(IBlockTransformTaskV1* task) override {
        auto* concrete = dynamic_cast<SchedulerE2eTransformTask*>(task);
        ASSERT_TRUE(concrete != nullptr);
        SchedulerE2eTransformSnapshot snapshot;
        snapshot.open_calls = concrete->open_calls;
        snapshot.process_calls = concrete->process_calls;
        snapshot.flush_calls = concrete->flush_calls;
        snapshot.cancel_calls = concrete->cancel_calls;
        snapshot.input_rows = concrete->input_rows;
        snapshot.input_schema_matched = concrete->input_schema_matched;
        released.push_back(std::move(snapshot));
        delete concrete;
        ++release_calls;
        --live_tasks;
    }

    void Reset() {
        create_calls = 0;
        release_calls = 0;
        live_tasks = 0;
        task_ids.clear();
        with_params.clear();
        pushed_plans.clear();
        released.clear();
    }

    int create_calls = 0;
    int release_calls = 0;
    int live_tasks = 0;
    std::vector<std::string> task_ids;
    std::vector<std::string> with_params;
    std::vector<std::string> pushed_plans;
    std::vector<SchedulerE2eTransformSnapshot> released;

 private:
    std::string name_;
    SchedulerE2eTransformKind kind_;
};

static void AppendPcapLe16(std::vector<uint8_t>* bytes, uint16_t value) {
    bytes->push_back(static_cast<uint8_t>(value));
    bytes->push_back(static_cast<uint8_t>(value >> 8));
}

static void AppendPcapLe32(std::vector<uint8_t>* bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes->push_back(static_cast<uint8_t>(value >> shift));
    }
}

static std::vector<uint8_t> MakeSchedulerE2ePcap(bool truncate_last_packet) {
    std::vector<uint8_t> bytes = {0xd4, 0xc3, 0xb2, 0xa1};
    AppendPcapLe16(&bytes, 2);
    AppendPcapLe16(&bytes, 4);
    AppendPcapLe32(&bytes, 0);
    AppendPcapLe32(&bytes, 0);
    AppendPcapLe32(&bytes, 65535);
    AppendPcapLe32(&bytes, 1);
    const auto append_record = [&](uint32_t seconds, const std::vector<uint8_t>& packet) {
        AppendPcapLe32(&bytes, seconds);
        AppendPcapLe32(&bytes, 0);
        AppendPcapLe32(&bytes, 4);
        AppendPcapLe32(&bytes, 4);
        bytes.insert(bytes.end(), packet.begin(), packet.end());
    };
    append_record(1, {1, 2, 3, 4});
    append_record(2, truncate_last_packet ? std::vector<uint8_t>{5} : std::vector<uint8_t>{5, 6, 7, 8});
    return bytes;
}

static void WriteSchedulerE2eBinary(const std::filesystem::path& path,
                                    const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good());
}

static std::string MakePcapSourceAddRequest(const std::string& name,
                                            const std::filesystem::path& path) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String(name.c_str());
    writer.Key("role");
    writer.String("source");
    writer.Key("options");
    writer.StartObject();
    writer.Key("path");
    writer.String(path.string().c_str());
    writer.Key("format");
    writer.String("pcap");
    writer.Key("batch_packets");
    writer.Uint(1);
    writer.EndObject();
    writer.EndObject();
    return buffer.GetString();
}

static std::string MakePcapSourceRemoveRequest(const std::string& name) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String(name.c_str());
    writer.EndObject();
    return buffer.GetString();
}

static std::shared_ptr<arrow::Buffer> SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_w = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)ipc_w->WriteRecordBatch(*batch);
    (void)ipc_w->Close();
    return sink->Finish().ValueOrDie();
}

static int64_t CountRowsInIpc(const uint8_t* data, size_t len) {
    auto buf = std::make_shared<arrow::Buffer>(data, len);
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buf)).ValueOrDie();
    int64_t rows = 0;
    while (true) {
        auto batch = reader->Next().ValueOrDie();
        if (!batch) break;
        rows += batch->num_rows();
    }
    return rows;
}

static std::string MakeReq(const std::string& sql) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("sql");
    w.String(sql.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeStreamReq(const std::string& sql_text) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("sql_text");
    w.String(sql_text.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeTaskReq(const std::string& task_id) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::shared_ptr<arrow::RecordBatch> MakeStreamBatch(int64_t base, int64_t rows = 1) {
    auto schema = arrow::schema({arrow::field("v", arrow::int64())});
    arrow::Int64Builder b;
    for (int64_t i = 0; i < rows; ++i) {
        (void)b.Append(base + i);
    }
    auto arr = b.Finish().ValueOrDie();
    return arrow::RecordBatch::Make(schema, rows, {arr});
}

static std::string ParseStatus(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("status") || !d["status"].IsString()) {
        return "";
    }
    return d["status"].GetString();
}

static std::string ParseTaskId(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("runtime_task_id") || !d["runtime_task_id"].IsString()) {
        return "";
    }
    return d["runtime_task_id"].GetString();
}

static int ParseShardCount(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("shard_count") || !d["shard_count"].IsUint()) {
        return -1;
    }
    return static_cast<int>(d["shard_count"].GetUint());
}

static bool IsValidStreamChannelList(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("channels") || !d["channels"].IsArray()) {
        return false;
    }
    const auto& arr = d["channels"].GetArray();
    for (auto it = arr.Begin(); it != arr.End(); ++it) {
        if (!it->IsObject()) return false;
        if (!it->HasMember("type") || !(*it)["type"].IsString()) return false;
        if (!it->HasMember("name") || !(*it)["name"].IsString()) return false;
        if (!it->HasMember("status") || !(*it)["status"].IsString()) return false;
    }
    return true;
}

class ParallelPassthroughStreamOperator final : public IOperator, public IStreamOperator {
 public:
    std::string Category() override { return "builtin"; }
    std::string Name() override { return "passthrough_stream"; }
    std::string Description() override { return "test parallel passthrough stream op"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel*, IChannel*) override { return -1; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        last_error_.clear();
        output_.reset();
        if (!sink_ctx.sink_channel) {
            last_error_ = "sink channel is null";
            return -1;
        }
        if (sink_ctx.sink_type == ChannelType::kStream) {
            auto* out = dynamic_cast<IStreamChannel*>(sink_ctx.sink_channel);
            if (!out) {
                last_error_ = "stream sink cast failed";
                return -1;
            }
            output_ = std::shared_ptr<IStreamChannel>(out, [](IStreamChannel*) {});
            return 0;
        }
        if (sink_ctx.sink_type == ChannelType::kDataFrame) {
            auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(sink_ctx.sink_channel);
            if (!appendable) {
                last_error_ = "dataframe sink must be appendable";
                return -1;
            }
            output_ = StreamChannelAdapter::MakeDataFrameAppend(
                "stream_adapter",
                sink_ctx.into_raw,
                std::shared_ptr<IAppendableDataFrameChannel>(appendable, [](IAppendableDataFrameChannel*) {}));
            return output_ ? 0 : -1;
        }
        if (sink_ctx.sink_type == ChannelType::kDatabase) {
            auto* db = dynamic_cast<IDatabaseChannel*>(sink_ctx.sink_channel);
            if (!db) {
                last_error_ = "database sink cast failed";
                return -1;
            }
            if (sink_ctx.table_name.empty()) {
                last_error_ = "builtin stream operator requires explicit table, use INTO <db_type>.<db_name>.<table>";
                return -1;
            }
            output_ = StreamChannelAdapter::MakeDatabaseWriter(
                "stream_adapter",
                sink_ctx.into_raw,
                std::shared_ptr<IDatabaseChannel>(db, [](IDatabaseChannel*) {}),
                sink_ctx.table_name);
            return output_ ? 0 : -1;
        }
        last_error_ = "unsupported sink type";
        return -1;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override { return 0; }

    int Process(const arrow::RecordBatch& batch, int64_t ts_ms) override {
        if (!output_) return -1;
        return output_->Put(batch.Slice(0, batch.num_rows()), ts_ms);
    }

    int Tick(int64_t) override { return 0; }
    int Flush() override { return 0; }
    std::string GetStats() override { return "{}"; }
    std::string LastError() override { return last_error_; }

    ParallelStrategy GetParallelStrategy() const override {
        return ParallelStrategy::STATELESS;
    }
    int GetParallelism() const override {
        return 4;
    }

 private:
    std::shared_ptr<IStreamChannel> output_;
    std::string last_error_;
};

class DbDirectWriterStreamOperator final : public IOperator, public IStreamOperator {
 public:
    std::string Category() override { return "custom"; }
    std::string Name() override { return "db_direct_writer_stream"; }
    std::string Description() override { return "test stream op writes to database channel directly"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel*, IChannel*) override { return -1; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        last_error_.clear();
        auto* db = dynamic_cast<IDatabaseChannel*>(sink_ctx.sink_channel);
        if (!db) {
            last_error_ = "output must be IDatabaseChannel";
            return -1;
        }
        db_ = std::shared_ptr<IDatabaseChannel>(db, [](IDatabaseChannel*) {});
        total_rows_ = 0;
        return 0;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override {
        if (!db_) return -1;
        if (db_->ExecuteSql("CREATE TABLE IF NOT EXISTS t44_two_segment_custom(total_rows INTEGER)") != 0) {
            last_error_ = "create table failed";
            return -1;
        }
        return 0;
    }

    int Process(const arrow::RecordBatch& batch, int64_t) override {
        total_rows_ += batch.num_rows();
        return 0;
    }

    int Tick(int64_t) override { return 0; }

    int Flush() override {
        if (!db_) return 0;
        if (total_rows_ <= 0) return 0;
        const std::string sql =
            "INSERT INTO t44_two_segment_custom(total_rows) VALUES (" + std::to_string(total_rows_) + ")";
        if (db_->ExecuteSql(sql.c_str()) != 0) {
            last_error_ = "insert failed";
            return -1;
        }
        return 0;
    }

    std::string GetStats() override { return "{}"; }
    std::string LastError() override { return last_error_; }

 private:
    std::shared_ptr<IDatabaseChannel> db_;
    int64_t total_rows_ = 0;
    std::string last_error_;
};

class SlowPassthroughStreamOperator final : public IOperator, public IStreamOperator {
 public:
    std::string Category() override { return "custom"; }
    std::string Name() override { return "slow_passthrough_stream"; }
    std::string Description() override { return "slow stream passthrough for coordinated-drop e2e"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel*, IChannel*) override { return -1; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        last_error_.clear();
        output_.reset();
        if (sink_ctx.sink_type != ChannelType::kStream) {
            last_error_ = "slow passthrough requires stream sink";
            return -1;
        }
        auto* out = dynamic_cast<IStreamChannel*>(sink_ctx.sink_channel);
        if (!out) {
            last_error_ = "stream sink cast failed";
            return -1;
        }
        output_ = std::shared_ptr<IStreamChannel>(out, [](IStreamChannel*) {});
        return 0;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override { return 0; }

    int Process(const arrow::RecordBatch& batch, int64_t ts_ms) override {
        if (!output_) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return output_->Put(batch.Slice(0, batch.num_rows()), ts_ms);
    }

    int Tick(int64_t) override { return 0; }
    int Flush() override { return 0; }
    std::string GetStats() override { return "{}"; }
    std::string LastError() override { return last_error_; }

 private:
    std::shared_ptr<IStreamChannel> output_;
    std::string last_error_;
};

static bool ListContainsTaskId(const std::string& rsp, const std::string& task_id) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("tasks") || !d["tasks"].IsArray()) {
        return false;
    }
    const auto& arr = d["tasks"];
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        const auto& item = arr[i];
        if (!item.IsObject()) continue;
        if (!item.HasMember("task_id") || !item["task_id"].IsString()) continue;
        if (task_id == item["task_id"].GetString()) return true;
    }
    return false;
}

static fnRouterHandler FindRouteHandler(PluginLoader* loader, const char* method, const char* uri) {
    fnRouterHandler h;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* rh = static_cast<IRouterHandle*>(p);
        rh->EnumRoutes([&](const RouteItem& item) {
            if (item.method == method && item.uri == uri) {
                h = item.handler;
            }
        });
        return h ? -1 : 0;
    });
    return h;
}

static fnRouterHandler FindExecuteHandler(PluginLoader* loader) {
    return FindRouteHandler(loader, "POST", "/scheduler/batch/execute");
}

static void SeedSourceTable(IDatabaseChannel* db, const char* table) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int64()),
        arrow::field("name", arrow::utf8()),
        arrow::field("score", arrow::float64()),
    });
    arrow::Int64Builder id_b;
    arrow::StringBuilder name_b;
    arrow::DoubleBuilder score_b;
    (void)id_b.Append(1); (void)id_b.Append(2); (void)id_b.Append(3);
    (void)name_b.Append("a"); (void)name_b.Append("b"); (void)name_b.Append("c");
    (void)score_b.Append(10.0); (void)score_b.Append(20.0); (void)score_b.Append(30.0);
    auto batch = arrow::RecordBatch::Make(schema, 3, {
        id_b.Finish().ValueOrDie(),
        name_b.Finish().ValueOrDie(),
        score_b.Finish().ValueOrDie(),
    });

    IBatchWriter* writer = nullptr;
    ASSERT_EQ(db->CreateWriter(table, &writer), 0);
    ASSERT_TRUE(writer != nullptr);
    auto buf = SerializeBatch(batch);
    ASSERT_EQ(writer->Write(buf->data(), static_cast<size_t>(buf->size())), 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
    ASSERT_EQ(stats.rows_written, 3);
}

static bool DrainStreamInt64Columns(IStreamChannel* ch,
                                    std::vector<int64_t>* col0,
                                    std::vector<int64_t>* col1 = nullptr) {
    if (!ch || !col0) return false;
    col0->clear();
    if (col1) col1->clear();
    bool saw_eof = false;
    for (int i = 0; i < 400; ++i) {
        PollEvent ev = ch->PollNext(10);
        if (ev.kind == PollEventKind::kData && ev.batch.data) {
            auto c0 = std::dynamic_pointer_cast<arrow::Int64Array>(ev.batch.data->column(0));
            if (!c0) continue;
            std::shared_ptr<arrow::Int64Array> c1;
            if (col1) c1 = std::dynamic_pointer_cast<arrow::Int64Array>(ev.batch.data->column(1));
            for (int64_t r = 0; r < ev.batch.data->num_rows(); ++r) {
                col0->push_back(c0->Value(r));
                if (col1 && c1) col1->push_back(c1->Value(r));
            }
            continue;
        }
        if (ev.kind == PollEventKind::kEof) {
            saw_eof = true;
            break;
        }
    }
    return saw_eof;
}

static int64_t QueryCount(IDatabaseChannel* db, const std::string& query) {
    IBatchReader* reader = nullptr;
    if (db->CreateReader(query.c_str(), &reader) != 0 || !reader) return -1;
    int64_t rows = 0;
    while (true) {
        const uint8_t* data = nullptr;
        size_t len = 0;
        int rc = reader->Next(&data, &len);
        if (rc == 1) break;
        if (rc != 0) {
            rows = -1;
            break;
        }
        rows += CountRowsInIpc(data, len);
    }
    reader->Close();
    reader->Release();
    return rows;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("=== Scheduler E2E Tests (Story 9.3) ===");

    const std::string suffix = std::to_string(::getpid());
    const std::filesystem::path db_path = std::filesystem::temp_directory_path() / ("flowsql_s9_3_" + suffix + ".db");
    const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_df_" + suffix);
    const std::filesystem::path operator_db_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_catalog_" + suffix);
    const std::filesystem::path stream_cfg = std::filesystem::temp_directory_path() / ("flowsql_s9_3_stream_" + suffix + ".yml");
    const std::filesystem::path stream_meta_db = std::filesystem::temp_directory_path() / ("flowsql_s9_3_stream_meta_" + suffix + ".db");
    const std::filesystem::path pcap_ok =
        std::filesystem::temp_directory_path() / ("flowsql_scheduler_pcap_ok_" + suffix + ".pcap");
    const std::filesystem::path pcap_error =
        std::filesystem::temp_directory_path() / ("flowsql_scheduler_pcap_error_" + suffix + ".pcap");
    std::filesystem::remove(db_path);
    std::filesystem::remove(stream_cfg);
    std::filesystem::remove(stream_meta_db);
    std::filesystem::remove(pcap_ok);
    std::filesystem::remove(pcap_error);
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(operator_db_dir);
    WriteSchedulerE2eBinary(pcap_ok, MakeSchedulerE2ePcap(false));
    WriteSchedulerE2eBinary(pcap_error, MakeSchedulerE2ePcap(true));

    {
        std::ofstream out(stream_cfg);
        ASSERT_TRUE(out.is_open());
        out << "channels:\n";
        out << "  stream_channels:\n";
        out << "    - type: ring\n";
        out << "      name: in\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: stop_in\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: stop_out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: svc_out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: df_out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: tcp_session_mock\n";
        out << "      name: tcp_src\n";
        out << "      option: \"mode=keyed;total_records=64;batch_rows=8;partition_count=4;emit_interval_ms=0;ring_size=256;overflow=drop\"\n";
        out << "    - type: tcp_session_mock\n";
        out << "      name: tcp_src_stateless\n";
        out << "      option: \"mode=stateless;total_records=64;batch_rows=8;emit_interval_ms=0;ring_size=256;overflow=drop\"\n";
        out.flush();
    }

    PluginLoader* loader = PluginLoader::Single();
    SchedulerE2eProtocol pcap_protocol;
    SchedulerE2eBlockOperator block_operator;
    SchedulerE2eTransformProvider packet_transform(
        "packet_to_protocol", SchedulerE2eTransformKind::kPacketToProtocol);
    SchedulerE2eTransformProvider passthrough_transform(
        "protocol_passthrough", SchedulerE2eTransformKind::kPassthrough);
    loader->Regist(IID_PROTOCOL, &pcap_protocol);
    loader->Regist(IID_BLOCK_STREAM_OPERATOR, &block_operator);
    loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V1, &packet_transform);
    loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V1, &passthrough_transform);
    const char* libs[] = {"libflowsql_database.so", "libflowsql_builtin.so", "libflowsql_catalog.so",
                          "libflowsql_pcapfile.so", "libflowsql_scheduler.so", "libflowsql_stream.so"};
    std::string db_opt = "type=sqlite;name=local;path=" + db_path.string();
    std::string catalog_opt = "data_dir=" + data_dir.string() + ";operator_db_dir=" + operator_db_dir.string();
    std::string stream_opt = "config_file=" + stream_cfg.string() + ";db_path=" + stream_meta_db.string();
    const char* opts[] = {db_opt.c_str(), nullptr, catalog_opt.c_str(), nullptr, nullptr, stream_opt.c_str()};
    ASSERT_EQ(loader->Load(get_absolute_process_path(), libs, opts, 6), 0);
    std::puts("[INFO] plugins loaded");
    ASSERT_EQ(loader->StartAll(), 0);
    std::puts("[INFO] plugins started");

    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* stream_factory = static_cast<IStreamFactory*>(loader->First(IID_STREAM_FACTORY));
    auto* op_registry = static_cast<IOperatorRegistry*>(loader->First(IID_OPERATOR_REGISTRY));
    ASSERT_TRUE(factory != nullptr);
    ASSERT_TRUE(registry != nullptr);
    ASSERT_TRUE(stream_factory != nullptr);
    ASSERT_TRUE(op_registry != nullptr);
    auto* db = dynamic_cast<IDatabaseChannel*>(factory->Get("sqlite", "local"));
    ASSERT_TRUE(db != nullptr);

    SeedSourceTable(db, "src");
    std::puts("[INFO] source table seeded");
    auto exec = FindExecuteHandler(loader);
    auto stream_exec = FindRouteHandler(loader, "POST", "/scheduler/stream/execute");
    auto stream_stop = FindRouteHandler(loader, "POST", "/scheduler/stream/stop");
    auto stream_status = FindRouteHandler(loader, "POST", "/scheduler/stream/status");
    auto stream_list = FindRouteHandler(loader, "POST", "/scheduler/stream/list");
    auto stream_add = FindRouteHandler(loader, "POST", "/channels/stream/add");
    auto stream_remove = FindRouteHandler(loader, "POST", "/channels/stream/remove");
    auto stream_reset = FindRouteHandler(loader, "POST", "/channels/stream/reset");
    auto stream_definitions_query = FindRouteHandler(loader, "POST", "/channels/stream/definitions/query");
    auto sql_classify = FindRouteHandler(loader, "POST", "/scheduler/sql/classify");
    auto activate = FindRouteHandler(loader, "POST", "/operators/activate");
    auto deactivate = FindRouteHandler(loader, "POST", "/operators/deactivate");
    auto upsert_batch = FindRouteHandler(loader, "POST", "/operators/upsert_batch");
    ASSERT_TRUE(exec != nullptr);
    ASSERT_TRUE(stream_exec != nullptr);
    ASSERT_TRUE(stream_stop != nullptr);
    ASSERT_TRUE(stream_status != nullptr);
    ASSERT_TRUE(stream_list != nullptr);
    ASSERT_TRUE(stream_add != nullptr);
    ASSERT_TRUE(stream_remove != nullptr);
    ASSERT_TRUE(stream_reset != nullptr);
    ASSERT_TRUE(stream_definitions_query != nullptr);
    ASSERT_TRUE(sql_classify != nullptr);
    ASSERT_TRUE(activate != nullptr);
    ASSERT_TRUE(deactivate != nullptr);
    ASSERT_TRUE(upsert_batch != nullptr);
    std::puts("[INFO] execute handler ready");

    // npm-offline-import T5.5: real pcapfile provider -> Scheduler -> block operator.
    {
        const std::string channel_name = "scheduler_pcap_ok";
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             MakePcapSourceAddRequest(channel_name, pcap_ok),
                             rsp),
                  error::OK);

        pcap_protocol.Reset();
        block_operator.Reset();
        const std::string sql =
            "SELECT * FROM pcapfile." + channel_name + " USING test.packet_counter";
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq(sql), rsp), error::OK);
        ASSERT_TRUE(rsp.find("BLOCK_STREAM_NOT_IMPLEMENTED") == std::string::npos);
        rapidjson::Document completed;
        completed.Parse(rsp.c_str());
        ASSERT_TRUE(!completed.HasParseError() && completed.IsObject());
        ASSERT_TRUE(completed.HasMember("status") && completed["status"].IsString());
        ASSERT_EQ(std::string(completed["status"].GetString()), "completed");
        ASSERT_TRUE(completed.HasMember("rows") && completed["rows"].IsInt64());
        ASSERT_EQ(completed["rows"].GetInt64(), 2);
        ASSERT_TRUE(completed.HasMember("result_row_count") && completed["result_row_count"].IsInt64());
        ASSERT_EQ(completed["result_row_count"].GetInt64(), 2);
        ASSERT_EQ(pcap_protocol.layer_calls, 2);
        ASSERT_EQ(block_operator.init_calls, 1);
        ASSERT_EQ(block_operator.schema_calls, 1);
        ASSERT_TRUE(block_operator.schema_matches_packet);
        ASSERT_EQ(block_operator.process_calls, 2);
        ASSERT_EQ(block_operator.flush_calls, 1);
        ASSERT_EQ(block_operator.rows_seen, 2);
        ASSERT_EQ(block_operator.sequences, std::vector<uint64_t>({0, 1}));
        ASSERT_EQ(block_operator.raw_packets,
                  std::vector<std::string>({std::string("\x01\x02\x03\x04", 4),
                                            std::string("\x05\x06\x07\x08", 4)}));

        ASSERT_EQ(stream_remove("/channels/stream/remove",
                                MakePcapSourceRemoveRequest(channel_name),
                                rsp),
                  error::OK);
    }
    {
        const std::string channel_name = "scheduler_pcap_dataframe";
        const std::string dataframe_name = "scheduler_pcap_dataframe";
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             MakePcapSourceAddRequest(channel_name, pcap_ok),
                             rsp),
                  error::OK);

        pcap_protocol.Reset();
        const std::string sql = "SELECT * FROM pcapfile." + channel_name +
                                " INTO dataframe." + dataframe_name;
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq(sql), rsp), error::OK);
        rapidjson::Document completed;
        completed.Parse(rsp.c_str());
        ASSERT_TRUE(!completed.HasParseError() && completed.IsObject());
        ASSERT_EQ(completed.MemberCount(), rapidjson::SizeType(4));
        ASSERT_TRUE(!completed.HasMember("data"));
        ASSERT_TRUE(completed.HasMember("status") && completed["status"].IsString());
        ASSERT_EQ(std::string(completed["status"].GetString()), "completed");
        ASSERT_TRUE(completed.HasMember("rows") && completed["rows"].IsInt64());
        ASSERT_EQ(completed["rows"].GetInt64(), 2);
        ASSERT_TRUE(completed.HasMember("result_row_count") && completed["result_row_count"].IsInt64());
        ASSERT_EQ(completed["result_row_count"].GetInt64(), 2);
        ASSERT_TRUE(completed.HasMember("result_target") && completed["result_target"].IsString());
        ASSERT_EQ(std::string(completed["result_target"].GetString()), "dataframe." + dataframe_name);
        ASSERT_EQ(pcap_protocol.layer_calls, 2);

        auto output = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(dataframe_name.c_str()));
        ASSERT_TRUE(output != nullptr);
        DataFrame packets;
        ASSERT_EQ(output->Read(&packets), 0);
        const auto packet_batch = packets.ToArrow();
        ASSERT_TRUE(packet_batch != nullptr);
        ASSERT_EQ(packet_batch->num_rows(), 2);
        ASSERT_TRUE(packet_batch->schema()->Equals(packet::PacketSchema(), true));
        auto sequence = std::dynamic_pointer_cast<arrow::UInt64Array>(
            packet_batch->GetColumnByName("sequence"));
        auto raw_data = std::dynamic_pointer_cast<arrow::BinaryArray>(
            packet_batch->GetColumnByName("raw_data"));
        ASSERT_TRUE(sequence != nullptr && raw_data != nullptr);
        ASSERT_EQ(sequence->Value(0), 0);
        ASSERT_EQ(sequence->Value(1), 1);
        ASSERT_EQ(std::string(raw_data->GetView(0)), std::string("\x01\x02\x03\x04", 4));
        ASSERT_EQ(std::string(raw_data->GetView(1)), std::string("\x05\x06\x07\x08", 4));

        ASSERT_EQ(stream_remove("/channels/stream/remove",
                                MakePcapSourceRemoveRequest(channel_name),
                                rsp),
                  error::OK);
        ASSERT_EQ(registry->Unregister(dataframe_name.c_str()), 0);
    }
    {
        const std::string channel_name = "scheduler_pcap_transform";
        const std::string dataframe_name = "scheduler_pcap_transform";
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             MakePcapSourceAddRequest(channel_name, pcap_ok),
                             rsp),
                  error::OK);

        pcap_protocol.Reset();
        packet_transform.Reset();
        passthrough_transform.Reset();
        const std::string sql =
            "SELECT * FROM pcapfile." + channel_name +
            " WHERE captured_len >= 4"
            " USING test.packet_to_protocol WITH mode=decode"
            " WHERE sequence >= 1 AND protocol != 'DROP'"
            " THEN test.protocol_passthrough WITH mode=pass"
            " WHERE protocol = 'HTTP'"
            " INTO dataframe." + dataframe_name;
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq(sql), rsp), error::OK);

        rapidjson::Document completed;
        completed.Parse(rsp.c_str());
        ASSERT_TRUE(!completed.HasParseError() && completed.IsObject());
        ASSERT_EQ(completed.MemberCount(), rapidjson::SizeType(4));
        ASSERT_TRUE(!completed.HasMember("data"));
        ASSERT_TRUE(completed.HasMember("status") && completed["status"].IsString());
        ASSERT_EQ(std::string(completed["status"].GetString()), "completed");
        ASSERT_TRUE(completed.HasMember("rows") && completed["rows"].IsInt64());
        ASSERT_EQ(completed["rows"].GetInt64(), 1);
        ASSERT_TRUE(completed.HasMember("result_row_count") &&
                    completed["result_row_count"].IsInt64());
        ASSERT_EQ(completed["result_row_count"].GetInt64(), 1);
        ASSERT_TRUE(completed.HasMember("result_target") &&
                    completed["result_target"].IsString());
        ASSERT_EQ(std::string(completed["result_target"].GetString()),
                  "dataframe." + dataframe_name);
        ASSERT_EQ(pcap_protocol.layer_calls, 2);

        auto output = std::dynamic_pointer_cast<IDataFrameChannel>(
            registry->Get(dataframe_name.c_str()));
        ASSERT_TRUE(output != nullptr);
        DataFrame result;
        ASSERT_EQ(output->Read(&result), 0);
        auto result_batch = result.ToArrow();
        ASSERT_TRUE(result_batch != nullptr);
        ASSERT_EQ(result_batch->num_rows(), 1);
        ASSERT_TRUE(result_batch->schema()->Equals(*SchedulerE2eTransformSchema(), true));
        auto sequence = std::dynamic_pointer_cast<arrow::UInt64Array>(
            result_batch->GetColumnByName("sequence"));
        auto captured_len = std::dynamic_pointer_cast<arrow::UInt32Array>(
            result_batch->GetColumnByName("captured_len"));
        auto protocol = std::dynamic_pointer_cast<arrow::StringArray>(
            result_batch->GetColumnByName("protocol"));
        ASSERT_TRUE(sequence != nullptr && captured_len != nullptr && protocol != nullptr);
        ASSERT_EQ(sequence->Value(0), 1);
        ASSERT_EQ(captured_len->Value(0), 4);
        ASSERT_EQ(protocol->GetString(0), "HTTP");

        const auto verify_provider = [](const SchedulerE2eTransformProvider& provider,
                                        const std::string& expected_mode,
                                        const std::vector<int64_t>& execution_input_rows) {
            ASSERT_EQ(provider.create_calls, 2);
            ASSERT_EQ(provider.release_calls, 2);
            ASSERT_EQ(provider.live_tasks, 0);
            ASSERT_EQ(provider.task_ids.size(), 2u);
            ASSERT_TRUE(provider.task_ids[0] != provider.task_ids[1]);
            ASSERT_EQ(provider.with_params.size(), 2u);
            ASSERT_TRUE(provider.with_params[0].find(expected_mode) != std::string::npos);
            ASSERT_TRUE(provider.with_params[1].find(expected_mode) != std::string::npos);
            ASSERT_EQ(provider.pushed_plans,
                      std::vector<std::string>({kEmptyCanonicalFilterPlanV1,
                                                kEmptyCanonicalFilterPlanV1}));
            ASSERT_EQ(provider.released.size(), 2u);
            ASSERT_EQ(provider.released[0].open_calls, 1);
            ASSERT_EQ(provider.released[0].process_calls, 0);
            ASSERT_EQ(provider.released[0].flush_calls, 0);
            ASSERT_EQ(provider.released[0].cancel_calls, 1);
            ASSERT_TRUE(provider.released[0].input_schema_matched);
            ASSERT_EQ(provider.released[1].open_calls, 1);
            ASSERT_EQ(provider.released[1].process_calls, 2);
            ASSERT_EQ(provider.released[1].flush_calls, 1);
            ASSERT_EQ(provider.released[1].cancel_calls, 0);
            ASSERT_EQ(provider.released[1].input_rows, execution_input_rows);
            ASSERT_TRUE(provider.released[1].input_schema_matched);
        };
        verify_provider(packet_transform, "decode", {1, 1});
        verify_provider(passthrough_transform, "pass", {0, 1});

        ASSERT_EQ(stream_remove("/channels/stream/remove",
                                MakePcapSourceRemoveRequest(channel_name),
                                rsp),
                  error::OK);
        ASSERT_EQ(registry->Unregister(dataframe_name.c_str()), 0);
    }
    {
        const std::string channel_name = "scheduler_pcap_error";
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             MakePcapSourceAddRequest(channel_name, pcap_error),
                             rsp),
                  error::OK);

        pcap_protocol.Reset();
        block_operator.Reset();
        const std::string sql =
            "SELECT * FROM pcapfile." + channel_name + " USING test.packet_counter";
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq(sql), rsp), error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("\"status\":\"completed\"") == std::string::npos);
        rapidjson::Document failed;
        failed.Parse(rsp.c_str());
        ASSERT_TRUE(!failed.HasParseError() && failed.IsObject());
        ASSERT_TRUE(failed.HasMember("error_code") && failed["error_code"].IsString());
        ASSERT_EQ(std::string(failed["error_code"].GetString()), "OP_EXEC_FAIL");
        ASSERT_TRUE(failed.HasMember("error_stage") && failed["error_stage"].IsString());
        ASSERT_EQ(std::string(failed["error_stage"].GetString()), "execute");
        ASSERT_EQ(pcap_protocol.layer_calls, 1);
        ASSERT_EQ(block_operator.init_calls, 1);
        ASSERT_EQ(block_operator.schema_calls, 1);
        ASSERT_TRUE(block_operator.schema_matches_packet);
        ASSERT_EQ(block_operator.process_calls, 1);
        ASSERT_EQ(block_operator.flush_calls, 0);
        ASSERT_EQ(block_operator.rows_seen, 1);
        ASSERT_EQ(block_operator.sequences, std::vector<uint64_t>({0}));
        ASSERT_EQ(block_operator.raw_packets,
                  std::vector<std::string>({std::string("\x01\x02\x03\x04", 4)}));

        ASSERT_EQ(stream_remove("/channels/stream/remove",
                                MakePcapSourceRemoveRequest(channel_name),
                                rsp),
                  error::OK);
    }
    std::puts("[PASS] npm-offline-import Scheduler pcapfile E2E");

    // T17a: SQL classify 返回批/流类型
    {
        std::string rsp;
        ASSERT_EQ(sql_classify("/scheduler/sql/classify",
                               MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.classify_batch"),
                               rsp),
                  error::OK);
        rapidjson::Document batch_doc;
        batch_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!batch_doc.HasParseError() && batch_doc.IsObject());
        ASSERT_TRUE(batch_doc.HasMember("task_kind") && batch_doc["task_kind"].IsString());
        ASSERT_EQ(std::string(batch_doc["task_kind"].GetString()), "batch");

        ASSERT_EQ(sql_classify("/scheduler/sql/classify",
                               MakeReq("SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.classify_stream"),
                               rsp),
                  error::OK);
        rapidjson::Document stream_doc;
        stream_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!stream_doc.HasParseError() && stream_doc.IsObject());
        ASSERT_TRUE(stream_doc.HasMember("task_kind") && stream_doc["task_kind"].IsString());
        ASSERT_EQ(std::string(stream_doc["task_kind"].GetString()), "stream");
    }
    std::puts("[PASS] T17a");

    // T17b: Stream 通道定义元数据查询
    {
        std::string rsp;
        ASSERT_EQ(stream_definitions_query("/channels/stream/definitions/query", "{}", rsp), error::OK);
        rapidjson::Document doc;
        doc.Parse(rsp.c_str());
        ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
        ASSERT_TRUE(doc.HasMember("definitions") && doc["definitions"].IsArray());

        bool found_ring = false;
        bool found_stream_hub = false;
        for (const auto& item : doc["definitions"].GetArray()) {
            ASSERT_TRUE(item.IsObject());
            ASSERT_TRUE(item.HasMember("channel_type") && item["channel_type"].IsString());
            ASSERT_TRUE(item.HasMember("display_name") && item["display_name"].IsString());
            ASSERT_TRUE(item.HasMember("allowed_roles") && item["allowed_roles"].IsArray());
            ASSERT_TRUE(item.HasMember("option_schema") && item["option_schema"].IsArray());
            const std::string channel_type = item["channel_type"].GetString();
            if (channel_type == "ring") found_ring = true;
            if (channel_type == "stream_hub") found_stream_hub = true;
        }
        ASSERT_TRUE(found_ring);
        ASSERT_TRUE(found_stream_hub);
    }
    std::puts("[PASS] T17b");

    // T18: INTO dataframe.result 后可通过 Registry 读取
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.result"), rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch != nullptr);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM src"), 3);
    }
    std::puts("[PASS] T18");

    // T19: FROM dataframe.result INTO sqlite.local.t2
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result INTO sqlite.local.t2"), rsp);
        ASSERT_EQ(rc, error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t2"), 3);
    }
    std::puts("[PASS] T19");

    // T20: FROM dataframe.<不存在> 返回 NOT_FOUND
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.not_exists INTO sqlite.local.t3"), rsp);
        ASSERT_EQ(rc, error::NOT_FOUND);
    }
    std::puts("[PASS] T20");

    // T21: INTO dataframe.result 覆盖语义（第二次覆盖第一次）
    {
        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id <= 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch1 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch1 != nullptr);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id > 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch2 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch2 != nullptr);

        DataFrame out;
        ASSERT_EQ(ch2->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);
    }
    std::puts("[PASS] T21");

    // T22: 无 INTO 的匿名结果行为不变（仅响应返回，不落入 Registry 新名称）
    {
        std::string rsp;
        size_t before = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before; });
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq("SELECT * FROM sqlite.local.src"), rsp), error::OK);
        rapidjson::Document doc;
        doc.Parse(rsp.c_str());
        ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
        ASSERT_TRUE(doc.HasMember("rows"));
        ASSERT_TRUE(doc.HasMember("result_row_count"));
        ASSERT_EQ(doc["rows"].GetInt64(), 3);
        ASSERT_EQ(doc["result_row_count"].GetInt64(), 3);
        size_t after = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after; });
        ASSERT_EQ(after, before);
    }
    std::puts("[PASS] T22");

    // T23: 跨通道链路 + 内置算子 passthrough
    {
        std::string activate_rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", activate_rsp), error::OK);

        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.out"), rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("out")) != nullptr);
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe.out INTO sqlite.local.t_passthrough"), rsp),
                  error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t_passthrough"), 3);
    }
    std::puts("[PASS] T23");

    // T24: 去激活仅阻止新任务，不中断已 running 任务
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        int32_t running_rc = error::INTERNAL_ERROR;
        std::thread worker([&]() {
            std::string local_rsp;
            running_rc = exec("/scheduler/batch/execute",
                              MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough "
                                      "WITH delay_ms=800 INTO dataframe.running_out"),
                              local_rsp);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        ASSERT_EQ(deactivate("/operators/deactivate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        worker.join();
        ASSERT_EQ(running_rc, error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("running_out")) != nullptr);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.blocked"), rsp),
                  error::CONFLICT);
    }
    std::puts("[PASS] T24");

    // T25: 双算子串行链路成功（每个算子独立 WITH）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH delay_ms=10 "
                               "THEN builtin.passthrough WITH delay_ms=0 "
                               "INTO dataframe.chain_ok"),
                       rsp),
                  error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_ok"));
        ASSERT_TRUE(ch != nullptr);
        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
    }
    std::puts("[PASS] T25");

    // T26: 双算子链第 2 步失败（独立 WITH 不复用）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        size_t before_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before_cnt; });
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src "
                                  "USING builtin.passthrough WITH force_fail=0 "
                                  "THEN builtin.passthrough WITH force_fail=1 "
                                  "INTO dataframe.chain_fail"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_fail")) == nullptr);

        size_t after_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after_cnt; });
        ASSERT_EQ(after_cnt, before_cnt);  // 失败后不应新增/泄漏具名通道

        // 失败后再次执行，验证执行器状态未污染
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH force_fail=0 "
                               "THEN builtin.passthrough WITH force_fail=0 "
                               "INTO dataframe.chain_after_fail"),
                       rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_after_fail")) != nullptr);
    }
    std::puts("[PASS] T26");

    // T27: 多源 + USING builtin.passthrough 走统一多输入入口（默认回退到首输入）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.passthrough INTO dataframe.multi_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("multi_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);  // dataframe.result 在 T21 被覆盖为 1 行
        ASSERT_EQ(std::get<int64_t>(out.GetRow(0)[0]), 3);
    }
    std::puts("[PASS] T27");

    // T28: 多源无 USING 算子应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out INTO dataframe.multi_no_op"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM requires USING operator") != std::string::npos);
    }
    std::puts("[PASS] T28");

    // T29: 多源包含非 dataframe.* 应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src,dataframe.result "
                                  "USING builtin.passthrough INTO dataframe.multi_mixed"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM only supports dataframe.* in Sprint 10") != std::string::npos);
    }
    std::puts("[PASS] T29");

    // T30: 阶段过滤语法在 parser 层明确拒绝多 source 的 source-stage WHERE。
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "WHERE id > 1 USING builtin.passthrough INTO dataframe.multi_where"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("source-stage WHERE does not support multiple sources") !=
                    std::string::npos);
    }
    std::puts("[PASS] T30");

    // T31: INTO 非法目标（未限定名）应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO t2"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("invalid INTO destination") != std::string::npos);
    }
    std::puts("[PASS] T31");

    // T32: 激活 concat/hstack
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.concat"})", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.hstack"})", rsp), error::OK);
    }
    std::puts("[PASS] T32");

    // T33: concat 成功（按行合并）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.concat INTO dataframe.concat_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 6);
        ASSERT_EQ(out.GetSchema().size(), 3u);
    }
    std::puts("[PASS] T33");

    // T34: concat schema 不兼容应失败
    {
        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT id FROM sqlite.local.src INTO dataframe.only_id"),
                       rsp),
                  error::OK);

        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.only_id "
                                  "USING builtin.concat INTO dataframe.concat_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("concat schema mismatch") != std::string::npos);
    }
    std::puts("[PASS] T34");

    // T35: hstack 成功（按列合并）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.hstack INTO dataframe.hstack_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
        ASSERT_EQ(out.GetSchema().size(), 6u);
    }
    std::puts("[PASS] T35");

    // T36: hstack 行数不一致应失败
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.hstack INTO dataframe.hstack_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("hstack row count mismatch") != std::string::npos);
    }
    std::puts("[PASS] T36");

    // T37: concat 覆盖多类型（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL）
    {
        auto build_typed_channel = [&](const char* name, int32_t base) {
            auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
            ch->Open();

            DataFrame df;
            df.SetSchema({
                {"c_i32", DataType::INT32, 0, ""},
                {"c_i64", DataType::INT64, 0, ""},
                {"c_f32", DataType::FLOAT, 0, ""},
                {"c_f64", DataType::DOUBLE, 0, ""},
                {"c_str", DataType::STRING, 0, ""},
                {"c_bool", DataType::BOOLEAN, 0, ""},
            });
            df.AppendRow({base + 1, int64_t(base + 1000), float(base + 0.5f), double(base + 0.25), std::string("n") + std::to_string(base + 1), true});
            df.AppendRow({base + 2, int64_t(base + 2000), float(base + 1.5f), double(base + 1.25), std::string("n") + std::to_string(base + 2), false});
            ASSERT_EQ(ch->Write(&df), 0);

            (void)registry->Unregister(name);
            ASSERT_EQ(registry->Register(name, std::static_pointer_cast<IChannel>(ch)), 0);
        };

        build_typed_channel("typed_a", 10);
        build_typed_channel("typed_b", 20);

        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.typed_a,dataframe.typed_b "
                                  "USING builtin.concat INTO dataframe.concat_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 4);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::INT64);
        ASSERT_EQ(schema[2].type, DataType::FLOAT);
        ASSERT_EQ(schema[3].type, DataType::DOUBLE);
        ASSERT_EQ(schema[4].type, DataType::STRING);
        ASSERT_EQ(schema[5].type, DataType::BOOLEAN);

        auto row3 = out.GetRow(3);
        ASSERT_EQ(std::get<int32_t>(row3[0]), 22);
        ASSERT_EQ(std::get<int64_t>(row3[1]), 2020);
        ASSERT_EQ(std::get<std::string>(row3[4]), "n22");
        ASSERT_EQ(std::get<bool>(row3[5]), false);
    }
    std::puts("[PASS] T37");

    // T38: hstack 覆盖多类型（按列合并）
    {
        auto left = std::make_shared<DataFrameChannel>("dataframe", "hleft");
        auto right = std::make_shared<DataFrameChannel>("dataframe", "hright");
        left->Open();
        right->Open();

        DataFrame ldf;
        ldf.SetSchema({
            {"a_i32", DataType::INT32, 0, ""},
            {"a_str", DataType::STRING, 0, ""},
            {"a_bool", DataType::BOOLEAN, 0, ""},
        });
        ldf.AppendRow({int32_t(1), std::string("x"), true});
        ldf.AppendRow({int32_t(2), std::string("y"), false});
        ASSERT_EQ(left->Write(&ldf), 0);

        DataFrame rdf;
        rdf.SetSchema({
            {"b_i64", DataType::INT64, 0, ""},
            {"b_f32", DataType::FLOAT, 0, ""},
            {"b_f64", DataType::DOUBLE, 0, ""},
        });
        rdf.AppendRow({int64_t(100), float(1.5f), double(10.25)});
        rdf.AppendRow({int64_t(200), float(2.5f), double(20.25)});
        ASSERT_EQ(right->Write(&rdf), 0);

        (void)registry->Unregister("hleft");
        (void)registry->Unregister("hright");
        ASSERT_EQ(registry->Register("hleft", std::static_pointer_cast<IChannel>(left)), 0);
        ASSERT_EQ(registry->Register("hright", std::static_pointer_cast<IChannel>(right)), 0);

        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.hleft,dataframe.hright "
                                  "USING builtin.hstack INTO dataframe.hstack_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 2);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::STRING);
        ASSERT_EQ(schema[2].type, DataType::BOOLEAN);
        ASSERT_EQ(schema[3].type, DataType::INT64);
        ASSERT_EQ(schema[4].type, DataType::FLOAT);
        ASSERT_EQ(schema[5].type, DataType::DOUBLE);

        auto row0 = out.GetRow(0);
        ASSERT_EQ(std::get<int32_t>(row0[0]), 1);
        ASSERT_EQ(std::get<std::string>(row0[1]), "x");
        ASSERT_EQ(std::get<bool>(row0[2]), true);
        ASSERT_EQ(std::get<int64_t>(row0[3]), 100);
    }
    std::puts("[PASS] T38");

    // T38A: batch execute 支持 dataframe -> stream
    {
        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.df2stream_src"),
                       rsp),
                  error::OK);

        auto* df_out = stream_factory->Get("ring", "df_out");
        ASSERT_TRUE(df_out != nullptr);

        while (true) {
            PollEvent ev = df_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe.df2stream_src INTO ring.df_out"),
                       rsp),
                  error::OK);
        ASSERT_TRUE(rsp.find("\"status\":\"completed\"") != std::string::npos);

        int rows = 0;
        bool saw_eof = false;
        for (int i = 0; i < 200; ++i) {
            PollEvent ev = df_out->PollNext(10);
            if (ev.kind == PollEventKind::kData && ev.batch.data) {
                rows += static_cast<int>(ev.batch.data->num_rows());
            } else if (ev.kind == PollEventKind::kEof) {
                saw_eof = true;
                break;
            }
        }
        ASSERT_EQ(rows, 3);
        ASSERT_TRUE(saw_eof);
    }
    std::puts("[PASS] T38A");

    // T38B: dataframe_dispatch_stream 默认无参数 => range(auto) 平均分段
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.dataframe_dispatch_stream"})", rsp), error::OK);
        const std::string seed_name = "dispatch_seed_" + suffix;
        auto rebuild_dispatch_seed = [&](const std::string& df_name) {
            auto seed = std::make_shared<DataFrameChannel>("dataframe", df_name);
            ASSERT_EQ(seed->Open(), 0);
            DataFrame data;
            data.SetSchema({
                {"id", DataType::INT64, 0, ""},
                {"service_id", DataType::INT64, 0, ""},
            });
            for (int64_t i = 1; i <= 4; ++i) {
                ASSERT_EQ(data.AppendRow({i, 10 + (i - 1) / 2}), 0);
            }
            ASSERT_EQ(seed->Write(&data), 0);
            if (registry->Get(df_name.c_str())) {
                ASSERT_EQ(registry->Unregister(df_name.c_str()), 0);
            }
            ASSERT_EQ(registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(seed)), 0);
        };
        rebuild_dispatch_seed(seed_name);

        const std::string hub_name = "dispatch_hub_auto_" + suffix;
        rapidjson::StringBuffer add_buf;
        rapidjson::Writer<rapidjson::StringBuffer> add_w(add_buf);
        add_w.StartObject();
        add_w.Key("type");
        add_w.String("stream_hub");
        add_w.Key("name");
        add_w.String(hub_name.c_str());
        add_w.Key("role");
        add_w.String("both");
        add_w.Key("options");
        add_w.StartObject();
        add_w.Key("mode");
        add_w.String("split");
        add_w.Key("partition_count");
        add_w.Int(4);
        add_w.Key("partition_ring_mode");
        add_w.String("spsc");
        add_w.Key("partition_ring_size");
        add_w.Int(256);
        add_w.EndObject();
        add_w.EndObject();
        ASSERT_EQ(stream_add("/channels/stream/add", add_buf.GetString(), rsp), error::OK);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe." + seed_name +
                               " USING builtin.dataframe_dispatch_stream INTO stream_hub." + hub_name),
                       rsp),
                  error::OK);

        auto* hub = stream_factory->Get("stream_hub", hub_name.c_str());
        ASSERT_TRUE(hub != nullptr);
        ASSERT_TRUE(hub->IsHubChannel());
        ASSERT_EQ(hub->HubPartitionCount(), size_t(4));

        for (size_t i = 0; i < 4; ++i) {
            auto p = hub->HubPartition(i);
            ASSERT_TRUE(p != nullptr);
            std::vector<int64_t> ids;
            ASSERT_TRUE(DrainStreamInt64Columns(p.get(), &ids));
            ASSERT_EQ(ids.size(), size_t(1));
            ASSERT_EQ(ids[0], static_cast<int64_t>(i + 1));
        }
    }
    std::puts("[PASS] T38B");

    // T38C: dataframe_dispatch_stream WITH field_name=... => hash
    {
        std::string rsp;
        const std::string seed_name = "dispatch_seed_" + suffix;
        auto seed = std::make_shared<DataFrameChannel>("dataframe", seed_name);
        ASSERT_EQ(seed->Open(), 0);
        DataFrame data;
        data.SetSchema({
            {"id", DataType::INT64, 0, ""},
            {"service_id", DataType::INT64, 0, ""},
        });
        for (int64_t i = 1; i <= 4; ++i) {
            ASSERT_EQ(data.AppendRow({i, 10 + (i - 1) / 2}), 0);
        }
        ASSERT_EQ(seed->Write(&data), 0);
        if (registry->Get(seed_name.c_str())) {
            ASSERT_EQ(registry->Unregister(seed_name.c_str()), 0);
        }
        ASSERT_EQ(registry->Register(seed_name.c_str(), std::static_pointer_cast<IChannel>(seed)), 0);

        const std::string hub_name = "dispatch_hub_hash_" + suffix;
        rapidjson::StringBuffer add_buf;
        rapidjson::Writer<rapidjson::StringBuffer> add_w(add_buf);
        add_w.StartObject();
        add_w.Key("type");
        add_w.String("stream_hub");
        add_w.Key("name");
        add_w.String(hub_name.c_str());
        add_w.Key("role");
        add_w.String("both");
        add_w.Key("options");
        add_w.StartObject();
        add_w.Key("mode");
        add_w.String("split");
        add_w.Key("partition_count");
        add_w.Int(4);
        add_w.Key("partition_ring_mode");
        add_w.String("spsc");
        add_w.Key("partition_ring_size");
        add_w.Int(256);
        add_w.EndObject();
        add_w.EndObject();
        ASSERT_EQ(stream_add("/channels/stream/add", add_buf.GetString(), rsp), error::OK);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe." + seed_name +
                               " USING builtin.dataframe_dispatch_stream WITH field_name=service_id INTO stream_hub." +
                               hub_name),
                       rsp),
                  error::OK);

        auto* hub = stream_factory->Get("stream_hub", hub_name.c_str());
        ASSERT_TRUE(hub != nullptr);
        ASSERT_TRUE(hub->IsHubChannel());
        ASSERT_EQ(hub->HubPartitionCount(), size_t(4));

        std::map<int64_t, std::set<size_t>> service_partitions;
        std::map<int64_t, int> service_counts;
        int total_rows = 0;
        for (size_t i = 0; i < 4; ++i) {
            auto p = hub->HubPartition(i);
            ASSERT_TRUE(p != nullptr);
            std::vector<int64_t> ids;
            std::vector<int64_t> service_ids;
            ASSERT_TRUE(DrainStreamInt64Columns(p.get(), &ids, &service_ids));
            ASSERT_EQ(ids.size(), service_ids.size());
            total_rows += static_cast<int>(ids.size());
            for (size_t k = 0; k < service_ids.size(); ++k) {
                service_partitions[service_ids[k]].insert(i);
                service_counts[service_ids[k]] += 1;
            }
        }
        ASSERT_EQ(total_rows, 4);
        ASSERT_EQ(service_counts.size(), size_t(2));
        for (const auto& kv : service_counts) {
            ASSERT_EQ(kv.second, 2);
            ASSERT_TRUE(service_partitions[kv.first].size() == 1);
        }
    }
    std::puts("[PASS] T38C");

    // T38D: 参数规则校验（仅 range_rows 且无 strategy -> 报错）
    {
        std::string rsp;
        const std::string seed_name = "dispatch_seed_" + suffix;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe." + seed_name +
                               " USING builtin.dataframe_dispatch_stream WITH range_rows=100 INTO stream_hub.dispatch_hub_auto_" +
                               suffix),
                       rsp),
                  error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("DATAFRAME_DISPATCH_STRATEGY_REQUIRED") != std::string::npos);
    }
    std::puts("[PASS] T38D");

    // T39: 流式 execute/status/list + 数据流转
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        auto* stream_in = stream_factory->Get("ring", "in");
        auto* stream_out = stream_factory->Get("ring", "out");
        ASSERT_TRUE(stream_in != nullptr);
        ASSERT_TRUE(stream_out != nullptr);

        // 清理输出通道残留数据
        while (true) {
            PollEvent ev = stream_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        ASSERT_EQ(stream_in->Put(MakeStreamBatch(1), 1001), 0);
        ASSERT_EQ(stream_in->Put(MakeStreamBatch(2), 1002), 0);
        ASSERT_EQ(stream_in->Put(MakeStreamBatch(3), 1003), 0);
        stream_in->CloseStream();

        std::string final_status;
        bool done = false;
        for (int i = 0; i < 300; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            final_status = ParseStatus(s_rsp);
            if (final_status == "stopped" || final_status == "cancelled" || final_status == "failed") {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(final_status, "stopped");

        int rows = 0;
        for (int i = 0; i < 200 && rows < 3; ++i) {
            PollEvent ev = stream_out->PollNext(10);
            if (ev.kind == PollEventKind::kData && ev.batch.data) {
                rows += static_cast<int>(ev.batch.data->num_rows());
            }
        }
        ASSERT_EQ(rows, 3);

        std::string list_rsp;
        ASSERT_EQ(stream_list("/scheduler/stream/list", "{}", list_rsp), error::OK);
        ASSERT_TRUE(ListContainsTaskId(list_rsp, task_id));
    }
    std::puts("[PASS] T39");

    // T40: 流式 stop 终止运行中任务
    {
        std::string rsp;
        auto* stop_in = stream_factory->Get("ring", "stop_in");
        auto* stop_out = stream_factory->Get("ring", "stop_out");
        ASSERT_TRUE(stop_in != nullptr);
        ASSERT_TRUE(stop_out != nullptr);

        while (true) {
            PollEvent ev = stop_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM ring.stop_in USING builtin.passthrough_stream INTO stream.stop_out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        bool seen_running = false;
        for (int i = 0; i < 100; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            const std::string st = ParseStatus(s_rsp);
            if (st == "running" || st == "stopping") {
                seen_running = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(seen_running);

        std::string stop_rsp;
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(task_id), stop_rsp), error::OK);
        const std::string stop_status = ParseStatus(stop_rsp);
        ASSERT_EQ(stop_status, "stopped");

        std::string status_rsp;
        ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), status_rsp), error::OK);
        const std::string final_status = ParseStatus(status_rsp);
        ASSERT_EQ(final_status, "stopped");

        std::string list_rsp;
        ASSERT_EQ(stream_list("/scheduler/stream/list", "{}", list_rsp), error::OK);
        ASSERT_TRUE(ListContainsTaskId(list_rsp, task_id));
    }
    std::puts("[PASS] T40");

    // T41: 内置 tcp_session_mock 通道 + tcp_service_merge_stream 算子链路
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.tcp_service_merge_stream"})", rsp), error::OK);

        auto* tcp_src = stream_factory->Get("tcp_session_mock", "tcp_src");
        auto* svc_out = stream_factory->Get("ring", "svc_out");
        ASSERT_TRUE(tcp_src != nullptr);
        ASSERT_TRUE(svc_out != nullptr);

        while (true) {
            PollEvent ev = svc_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.tcp_service_merge_stream INTO stream.svc_out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        std::string final_status;
        bool done = false;
        for (int i = 0; i < 300; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            final_status = ParseStatus(s_rsp);
            if (final_status == "stopped" || final_status == "cancelled" || final_status == "failed") {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(final_status, "stopped");

        int out_rows = 0;
        bool schema_ok = false;
        for (int i = 0; i < 200; ++i) {
            PollEvent ev = svc_out->PollNext(20);
            if (ev.kind != PollEventKind::kData || !ev.batch.data) continue;
            out_rows += static_cast<int>(ev.batch.data->num_rows());
            auto schema = ev.batch.data->schema();
            ASSERT_TRUE(schema != nullptr);
            ASSERT_TRUE(schema->GetFieldIndex("clientIP") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("serverIP") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("serverPort") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("bps") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("pps") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("clientPort") < 0);
            schema_ok = true;
            if (out_rows > 0) break;
        }
        ASSERT_TRUE(schema_ok);
        ASSERT_TRUE(out_rows > 0);
    }
    std::puts("[PASS] T41");

    // T42: 非 stream sink 在并行写能力不足时直接失败（无隐式降级）
    {
        std::string rsp;
        ASSERT_EQ(op_registry->Register("custom.parallel_passthrough_stream", []() -> IOperator* {
            return new ParallelPassthroughStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"parallel_passthrough_stream",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e parallel passthrough stream",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.parallel_passthrough_stream"})", rsp), error::OK);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src_stateless "
                                      "USING custom.parallel_passthrough_stream INTO dataframe.stream_single_writer"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_SINK_CAPABILITY_MISMATCH") != std::string::npos);
    }
    std::puts("[PASS] T42");

    // T43: INTO 数据库目标规则（builtin 两段式报错；普通算子可接收两段式 DB 通道）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);
        ASSERT_EQ(op_registry->Register("custom.db_direct_writer_stream", []() -> IOperator* {
            return new DbDirectWriterStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"db_direct_writer_stream",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e custom stream db writer",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.db_direct_writer_stream"})", rsp), error::OK);

        auto wait_stream_terminal = [&](const std::string& task_id, std::string* final_status) -> bool {
            for (int i = 0; i < 400; ++i) {
                std::string s_rsp;
                if (stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp) != error::OK) {
                    return false;
                }
                const std::string st = ParseStatus(s_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    if (final_status) *final_status = st;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        // case 1: builtin + 三段式目标成功
        const int64_t src_rows_before = QueryCount(db, "SELECT * FROM src");
        ASSERT_EQ(src_rows_before, 3);
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream "
                                      "INTO sqlite.local.t44_into"),
                              rsp),
                  error::OK);
        std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());
        std::string terminal_status;
        ASSERT_TRUE(wait_stream_terminal(task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");
        ASSERT_TRUE(QueryCount(db, "SELECT * FROM t44_into") > 0);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM src"), src_rows_before);

        // case 2: WITH sink_table 不再作为框架兜底语义
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream WITH sink_table=t44_with "
                                      "INTO sqlite.local"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("sink_table") != std::string::npos);

        // case 3: builtin + 两段式 DB 目标失败（要求显式三段式）
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO sqlite.local"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("requires explicit table") != std::string::npos);

        // case 4: 普通算子可接收两段式 DB 通道并自行写入
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING custom.db_direct_writer_stream INTO sqlite.local"),
                              rsp),
                  error::OK);
        task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());
        terminal_status.clear();
        ASSERT_TRUE(wait_stream_terminal(task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");
        ASSERT_TRUE(QueryCount(db, "SELECT * FROM t44_two_segment_custom") > 0);
    }
    std::puts("[PASS] T43");

    // T44: Story 14.11 T5 回归（缺失注册/非法配置/重复创建）
    {
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"no_such_stream_type","name":"x_missing","option":""})",
                             rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("add stream channel failed") != std::string::npos ||
                    rsp.find("unsupported stream channel type") != std::string::npos);

        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_invalid","option":"ring_mode=spsc;ring_size=3;overflow=drop;finite=false"})",
                             rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("add stream channel failed") != std::string::npos ||
                    rsp.find("invalid option") != std::string::npos);

        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_dup","option":"ring_mode=spsc;ring_size=256;overflow=drop;finite=false;batch_rows=64"})",
                             rsp),
                  error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_dup","option":"ring_mode=spsc;ring_size=256;overflow=drop;finite=false;batch_rows=64"})",
                             rsp),
                  error::CONFLICT);
    }
    std::puts("[PASS] T44");

    // T46: Story 14.12 回归（结构化 add/query + split/merge selector 语义）
    {
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"stream_hub",
            "name":"npm_hub",
            "role":"both",
            "options":{
                "mode":"split",
                "partition_count":2,
                "partition_ring_mode":"spsc",
                "partition_ring_size":256
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"stream_hub",
            "name":"npm_merge",
            "role":"both",
            "options":{
                "mode":"merge",
                "partition_count":2,
                "partition_ring_mode":"spsc",
                "partition_ring_size":256
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"ring",
            "name":"source_only_ring",
            "role":"source",
            "options":{
                "ring_mode":"spsc",
                "ring_size":256,
                "overflow":"drop",
                "finite":false
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"ring",
            "name":"sink_only_ring",
            "role":"sink",
            "options":{
                "ring_mode":"spsc",
                "ring_size":256,
                "overflow":"drop",
                "finite":false
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.source_only_ring"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_CHANNEL_ROLE_MISMATCH") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM stream.sink_only_ring "
                                      "USING builtin.passthrough_stream INTO dataframe.sink_role_bad"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_CHANNEL_ROLE_MISMATCH") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.npm_hub[0]"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM stream.npm_merge[*] "
                                      "USING builtin.passthrough_stream INTO dataframe.npm_merge_bad"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.npm_hub"),
                              rsp),
                  error::OK);
        std::string producer_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!producer_task_id.empty());

        auto wait_stream_terminal = [&](const std::string& task_id, std::string* final_status) -> bool {
            for (int i = 0; i < 500; ++i) {
                std::string s_rsp;
                if (stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp) != error::OK) {
                    return false;
                }
                const std::string st = ParseStatus(s_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    if (final_status) *final_status = st;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        std::string terminal_status;
        ASSERT_TRUE(wait_stream_terminal(producer_task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeStreamReq("SELECT * FROM stream.npm_hub "
                                      "USING builtin.passthrough_stream INTO dataframe.npm_auto"),
                              rsp),
                  error::OK);

        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("resolved_sources") && exec_doc["resolved_sources"].IsArray());
        ASSERT_EQ(exec_doc["resolved_sources"].Size(), 2u);
        ASSERT_TRUE(exec_doc.HasMember("source_expand_rule") && exec_doc["source_expand_rule"].IsString());
        ASSERT_EQ(std::string(exec_doc["source_expand_rule"].GetString()), "auto_wildcard");

        std::string consumer_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!consumer_task_id.empty());
        std::string status_rsp;
        ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(consumer_task_id), status_rsp), error::OK);
        rapidjson::Document status_doc;
        status_doc.Parse(status_rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
        ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);
        ASSERT_TRUE(status_doc.HasMember("source_expand_rule") && status_doc["source_expand_rule"].IsString());
        ASSERT_EQ(std::string(status_doc["source_expand_rule"].GetString()), "auto_wildcard");

        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(consumer_task_id), status_rsp), error::OK);
        status_doc.Parse(status_rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
        ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);

        std::string query_rsp;
        auto stream_query = FindRouteHandler(loader, "POST", "/channels/stream/query");
        ASSERT_TRUE(stream_query != nullptr);
        ASSERT_EQ(stream_query("/channels/stream/query", "{}", query_rsp), error::OK);
        rapidjson::Document qdoc;
        qdoc.Parse(query_rsp.c_str());
        ASSERT_TRUE(!qdoc.HasParseError() && qdoc.IsObject());
        ASSERT_TRUE(qdoc.HasMember("channels") && qdoc["channels"].IsArray());
        bool found_split_hub = false;
        bool found_merge_hub = false;
        for (const auto& item : qdoc["channels"].GetArray()) {
            if (!item.IsObject()) continue;
            if (!item.HasMember("name") || !item["name"].IsString()) continue;
            const std::string name = item["name"].GetString();
            if (name == "npm_hub") {
                found_split_hub = true;
                ASSERT_TRUE(item.HasMember("role") && item["role"].IsString());
                ASSERT_EQ(std::string(item["role"].GetString()), "both");
                ASSERT_TRUE(item.HasMember("option_json") && item["option_json"].IsObject());
                ASSERT_TRUE(item.HasMember("derived_channels") && item["derived_channels"].IsArray());
                ASSERT_EQ(item["derived_channels"].Size(), 2u);
            }
            if (name == "npm_merge") {
                found_merge_hub = true;
                ASSERT_TRUE(item.HasMember("derived_channels") && item["derived_channels"].IsArray());
                ASSERT_EQ(item["derived_channels"].Size(), 0u);
            }
        }
        ASSERT_TRUE(found_split_hub);
        ASSERT_TRUE(found_merge_hub);
    }
    std::puts("[PASS] T46");

    // T46b: Stream 通道重置（同配置重建，清理 cancel 状态）
    {
        std::string rsp;
        const std::string reset_name = "reset_ring_" + suffix;
        rapidjson::StringBuffer add_buf;
        rapidjson::Writer<rapidjson::StringBuffer> add_w(add_buf);
        add_w.StartObject();
        add_w.Key("type");
        add_w.String("ring");
        add_w.Key("name");
        add_w.String(reset_name.c_str());
        add_w.Key("role");
        add_w.String("both");
        add_w.Key("options");
        add_w.StartObject();
        add_w.Key("ring_mode");
        add_w.String("spsc");
        add_w.Key("ring_size");
        add_w.Int(256);
        add_w.Key("overflow");
        add_w.String("drop");
        add_w.Key("finite");
        add_w.Bool(false);
        add_w.EndObject();
        add_w.EndObject();

        ASSERT_EQ(stream_add("/channels/stream/add", add_buf.GetString(), rsp), error::OK);
        auto* reset_ch = dynamic_cast<IStreamChannel*>(stream_factory->Get("ring", reset_name.c_str()));
        ASSERT_TRUE(reset_ch != nullptr);
        reset_ch->Cancel();
        ASSERT_EQ(reset_ch->Put(MakeStreamBatch(7001), 0), ECANCELED);

        rapidjson::StringBuffer reset_buf;
        rapidjson::Writer<rapidjson::StringBuffer> reset_w(reset_buf);
        reset_w.StartObject();
        reset_w.Key("type");
        reset_w.String("ring");
        reset_w.Key("name");
        reset_w.String(reset_name.c_str());
        reset_w.EndObject();
        ASSERT_EQ(stream_reset("/channels/stream/reset", reset_buf.GetString(), rsp), error::OK);

        auto* reset_ch_after = dynamic_cast<IStreamChannel*>(stream_factory->Get("ring", reset_name.c_str()));
        ASSERT_TRUE(reset_ch_after != nullptr);
        ASSERT_EQ(reset_ch_after->Put(MakeStreamBatch(7002), 0), 0);
    }
    std::puts("[PASS] T46b");

    // T46c: Stream 通道重置冲突与不存在校验
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string busy_name = "reset_busy_" + suffix;
        rapidjson::StringBuffer add_buf;
        rapidjson::Writer<rapidjson::StringBuffer> add_w(add_buf);
        add_w.StartObject();
        add_w.Key("type");
        add_w.String("ring");
        add_w.Key("name");
        add_w.String(busy_name.c_str());
        add_w.Key("role");
        add_w.String("both");
        add_w.Key("options");
        add_w.StartObject();
        add_w.Key("ring_mode");
        add_w.String("spsc");
        add_w.Key("ring_size");
        add_w.Int(256);
        add_w.Key("overflow");
        add_w.String("drop");
        add_w.Key("finite");
        add_w.Bool(false);
        add_w.EndObject();
        add_w.EndObject();
        ASSERT_EQ(stream_add("/channels/stream/add", add_buf.GetString(), rsp), error::OK);

        rapidjson::StringBuffer exec_buf;
        rapidjson::Writer<rapidjson::StringBuffer> exec_w(exec_buf);
        exec_w.StartObject();
        exec_w.Key("sql_text");
        exec_w.String(("SELECT * FROM ring." + busy_name +
                       " USING builtin.passthrough_stream INTO dataframe.reset_busy_out").c_str());
        exec_w.EndObject();
        ASSERT_EQ(stream_exec("/scheduler/stream/execute", exec_buf.GetString(), rsp), error::OK);
        const std::string busy_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!busy_task_id.empty());

        rapidjson::StringBuffer reset_busy_buf;
        rapidjson::Writer<rapidjson::StringBuffer> reset_busy_w(reset_busy_buf);
        reset_busy_w.StartObject();
        reset_busy_w.Key("type");
        reset_busy_w.String("ring");
        reset_busy_w.Key("name");
        reset_busy_w.String(busy_name.c_str());
        reset_busy_w.EndObject();
        ASSERT_EQ(stream_reset("/channels/stream/reset", reset_busy_buf.GetString(), rsp), error::CONFLICT);

        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(busy_task_id), rsp), error::OK);

        ASSERT_EQ(stream_reset("/channels/stream/reset",
                               R"({"type":"ring","name":"reset_not_exists"})",
                               rsp),
                  error::NOT_FOUND);
    }
    std::puts("[PASS] T46c");

    // T47: Web 代理流式通道查询接口（严格语义：上游不可达时返回 UNAVAILABLE）
    {
        const std::filesystem::path web_db_path = std::filesystem::temp_directory_path() /
                                                  ("flowsql_s9_3_web_" + suffix + ".db");
        std::filesystem::remove(web_db_path);

        // 当前 e2e 用例未加载 Gateway/Router 网络服务，Web 代理请求应返回 UNAVAILABLE。
        std::string web_opt = "host=127.0.0.1;port=18081;db_path=" + web_db_path.string() +
                              ";gateway=127.0.0.1:59883";
        const char* web_libs[] = {"libflowsql_web.so"};
        const char* web_opts[] = {web_opt.c_str()};
        ASSERT_EQ(loader->Load(get_absolute_process_path(), web_libs, web_opts, 1), 0);

        fnRouterHandler web_stream_query = nullptr;
        fnRouterHandler web_stream_definitions_query = nullptr;
        fnRouterHandler web_stream_reset = nullptr;
        web_stream_query = FindRouteHandler(loader, "POST", "/api/channels/stream/query");
        web_stream_definitions_query = FindRouteHandler(loader, "POST", "/api/channels/stream/definitions/query");
        web_stream_reset = FindRouteHandler(loader, "POST", "/api/channels/stream/reset");
        ASSERT_TRUE(web_stream_query != nullptr);
        ASSERT_TRUE(web_stream_definitions_query != nullptr);
        ASSERT_TRUE(web_stream_reset != nullptr);

        std::string rsp;
        ASSERT_EQ(web_stream_query("/api/channels/stream/query", "{}", rsp), error::UNAVAILABLE);
        ASSERT_TRUE(rsp.find("service unreachable") != std::string::npos);
        ASSERT_EQ(web_stream_definitions_query("/api/channels/stream/definitions/query", "{}", rsp), error::UNAVAILABLE);
        ASSERT_TRUE(rsp.find("service unreachable") != std::string::npos);
        ASSERT_EQ(web_stream_reset("/api/channels/stream/reset", R"({"type":"ring","name":"x"})", rsp), error::UNAVAILABLE);
        ASSERT_TRUE(rsp.find("service unreachable") != std::string::npos);
        std::filesystem::remove(web_db_path);
    }
    std::puts("[PASS] T47");

    // T48: Stream Group DAG（线性两节点）执行/状态/列表
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string group_src_name = "group_src_" + suffix;
        const std::string group_mid_name = "group_mid_" + suffix;
        const std::string group_out_name = "group_out_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(group_src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(group_mid_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(group_out_name), rsp), error::OK);

        auto* group_src = stream_factory->Get("ring", group_src_name.c_str());
        auto* group_out = stream_factory->Get("ring", group_out_name.c_str());
        ASSERT_TRUE(group_src != nullptr);
        ASSERT_TRUE(group_out != nullptr);

        while (true) {
            PollEvent ev = group_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        const std::string sql1 =
            "SELECT * FROM ring." + group_src_name +
            " USING builtin.passthrough_stream INTO stream." + group_mid_name;
        const std::string sql2 =
            "SELECT * FROM stream." + group_mid_name +
            " USING builtin.passthrough_stream INTO stream." + group_out_name;

        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        rapidjson::Document submit_doc;
        submit_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!submit_doc.HasParseError() && submit_doc.IsObject());
        ASSERT_TRUE(submit_doc.HasMember("runtime_kind") && submit_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(submit_doc["runtime_kind"].GetString()), "group");
        ASSERT_TRUE(submit_doc.HasMember("node_count") && submit_doc["node_count"].IsUint());
        ASSERT_EQ(submit_doc["node_count"].GetUint(), 2u);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        ASSERT_EQ(group_src->Put(MakeStreamBatch(11), 2001), 0);
        ASSERT_EQ(group_src->Put(MakeStreamBatch(12), 2002), 0);
        ASSERT_EQ(group_src->Put(MakeStreamBatch(13), 2003), 0);
        group_src->CloseStream();

        bool group_running_seen = false;
        for (int i = 0; i < 300; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("runtime_kind") && status_doc["runtime_kind"].IsString());
            ASSERT_EQ(std::string(status_doc["runtime_kind"].GetString()), "group");
            ASSERT_TRUE(status_doc.HasMember("nodes") && status_doc["nodes"].IsArray());
            ASSERT_EQ(status_doc["nodes"].Size(), 2u);
            ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
            ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);
            bool found_n1_sources = false;
            bool found_n2_sources = false;
            for (const auto& rs : status_doc["resolved_sources"].GetArray()) {
                ASSERT_TRUE(rs.IsObject());
                ASSERT_TRUE(rs.HasMember("node_id") && rs["node_id"].IsString());
                ASSERT_TRUE(rs.HasMember("sources") && rs["sources"].IsArray());
                const std::string node_id = rs["node_id"].GetString();
                if (node_id == "n1") {
                    found_n1_sources = true;
                    ASSERT_TRUE(rs["sources"].Size() >= 1u);
                } else if (node_id == "n2") {
                    found_n2_sources = true;
                    ASSERT_TRUE(rs["sources"].Size() >= 1u);
                }
            }
            ASSERT_TRUE(found_n1_sources);
            ASSERT_TRUE(found_n2_sources);
            const std::string st = ParseStatus(status_rsp);
            if (st == "running" || st == "preparing" || st == "stopping") {
                group_running_seen = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(group_running_seen);

        int out_rows = 0;
        for (int i = 0; i < 300 && out_rows < 3; ++i) {
            PollEvent ev = group_out->PollNext(10);
            if (ev.kind == PollEventKind::kData && ev.batch.data) {
                out_rows += static_cast<int>(ev.batch.data->num_rows());
            }
        }
        ASSERT_EQ(out_rows, 3);

        std::string stop_rsp;
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(group_task_id), stop_rsp), error::OK);
        ASSERT_EQ(ParseStatus(stop_rsp), "stopped");
        rapidjson::Document stop_doc;
        stop_doc.Parse(stop_rsp.c_str());
        ASSERT_TRUE(!stop_doc.HasParseError() && stop_doc.IsObject());
        ASSERT_TRUE(stop_doc.HasMember("resolved_sources") && stop_doc["resolved_sources"].IsArray());
        ASSERT_EQ(stop_doc["resolved_sources"].Size(), 2u);

        std::string list_rsp;
        ASSERT_EQ(stream_list("/scheduler/stream/list", "{}", list_rsp), error::OK);
        ASSERT_TRUE(ListContainsTaskId(list_rsp, group_task_id));
    }
    std::puts("[PASS] T48");

    // T49: Stream Group DAG source_share_sets 同源广播（单读多分支）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "share_src_" + suffix;
        const std::string out1_name = "share_out1_" + suffix;
        const std::string out2_name = "share_out2_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out1_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out2_name), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        auto* out1 = stream_factory->Get("ring", out1_name.c_str());
        auto* out2 = stream_factory->Get("ring", out2_name.c_str());
        ASSERT_TRUE(src != nullptr);
        ASSERT_TRUE(out1 != nullptr);
        ASSERT_TRUE(out2 != nullptr);

        while (true) {
            PollEvent ev = out1->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }
        while (true) {
            PollEvent ev = out2->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        const std::string sql1 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out1_name;
        const std::string sql2 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out2_name;

        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        rapidjson::Document submit_doc;
        submit_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!submit_doc.HasParseError() && submit_doc.IsObject());
        ASSERT_TRUE(submit_doc.HasMember("runtime_kind") && submit_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(submit_doc["runtime_kind"].GetString()), "group");
        ASSERT_TRUE(submit_doc.HasMember("share_set_count") && submit_doc["share_set_count"].IsUint());
        ASSERT_EQ(submit_doc["share_set_count"].GetUint(), 1u);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        ASSERT_EQ(src->Put(MakeStreamBatch(21), 3001), 0);
        ASSERT_EQ(src->Put(MakeStreamBatch(22), 3002), 0);
        ASSERT_EQ(src->Put(MakeStreamBatch(23), 3003), 0);
        src->CloseStream();

        bool group_done = false;
        for (int i = 0; i < 800; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("share_sets") && status_doc["share_sets"].IsArray());
            ASSERT_TRUE(!status_doc["share_sets"].Empty());
            ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
            ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);
            for (const auto& rs : status_doc["resolved_sources"].GetArray()) {
                ASSERT_TRUE(rs.IsObject());
                ASSERT_TRUE(rs.HasMember("sources") && rs["sources"].IsArray());
                ASSERT_TRUE(rs["sources"].Size() >= 1u);
            }
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                group_done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(group_done);

        int rows1 = 0;
        int rows2 = 0;
        for (int i = 0; i < 300 && (rows1 < 3 || rows2 < 3); ++i) {
            PollEvent ev1 = out1->PollNext(10);
            if (ev1.kind == PollEventKind::kData && ev1.batch.data) {
                rows1 += static_cast<int>(ev1.batch.data->num_rows());
            }
            PollEvent ev2 = out2->PollNext(10);
            if (ev2.kind == PollEventKind::kData && ev2.batch.data) {
                rows2 += static_cast<int>(ev2.batch.data->num_rows());
            }
        }
        ASSERT_EQ(rows1, 3);
        ASSERT_EQ(rows2, 3);
    }
    std::puts("[PASS] T49");

    // T49.1: source_share_set 高压下 coordinated drop 指标一致性
    {
        std::string rsp;
        ASSERT_EQ(op_registry->Register("custom.slow_passthrough_stream_late", []() -> IOperator* {
            return new SlowPassthroughStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"slow_passthrough_stream_late",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e slow passthrough stream",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.slow_passthrough_stream_late"})", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "share_pressure_src_" + suffix;
        const std::string out_fast_name = "share_pressure_fast_" + suffix;
        const std::string out_slow_name = "share_pressure_slow_" + suffix;

        auto make_add_ring_req = [](const std::string& name, int ring_size) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(ring_size);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name, 16384), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out_fast_name, 16384), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out_slow_name, 16384), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        auto* out_fast = stream_factory->Get("ring", out_fast_name.c_str());
        auto* out_slow = stream_factory->Get("ring", out_slow_name.c_str());
        ASSERT_TRUE(src != nullptr);
        ASSERT_TRUE(out_fast != nullptr);
        ASSERT_TRUE(out_slow != nullptr);

        while (true) {
            PollEvent ev = out_fast->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }
        while (true) {
            PollEvent ev = out_slow->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        const std::string sql_fast =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out_fast_name;
        const std::string sql_slow =
            "SELECT * FROM ring." + src_name +
            " USING custom.slow_passthrough_stream_late INTO stream." + out_slow_name;
        const std::string group_sql_text = sql_fast + ";\n" + sql_slow + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("timeout_s");
        w.Int(40);
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        constexpr int kInputBatches = 2600;
        int accepted_batches = 0;
        for (int i = 0; i < kInputBatches; ++i) {
            if (src->Put(MakeStreamBatch(100000 + i), 7000 + i) == 0) {
                ++accepted_batches;
            }
        }
        src->CloseStream();
        ASSERT_TRUE(accepted_batches > 0);

        bool done = false;
        std::string final_rsp;
        for (int i = 0; i < 1800; ++i) {
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), final_rsp), error::OK);
            const std::string st = ParseStatus(final_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);

        rapidjson::Document final_doc;
        final_doc.Parse(final_rsp.c_str());
        ASSERT_TRUE(!final_doc.HasParseError() && final_doc.IsObject());
        ASSERT_TRUE(final_doc.HasMember("share_sets") && final_doc["share_sets"].IsArray());
        ASSERT_EQ(final_doc["share_sets"].Size(), 1u);
        const auto& ss = final_doc["share_sets"][0];
        ASSERT_TRUE(ss.IsObject());
        ASSERT_TRUE(ss.HasMember("input_batches") && ss["input_batches"].IsUint64());
        ASSERT_TRUE(ss.HasMember("delivered_batches") && ss["delivered_batches"].IsUint64());
        ASSERT_TRUE(ss.HasMember("dropped_batches_shared") && ss["dropped_batches_shared"].IsUint64());
        ASSERT_TRUE(ss.HasMember("input_rows") && ss["input_rows"].IsUint64());
        ASSERT_TRUE(ss.HasMember("delivered_rows") && ss["delivered_rows"].IsUint64());
        ASSERT_TRUE(ss.HasMember("dropped_rows_shared") && ss["dropped_rows_shared"].IsUint64());
        ASSERT_TRUE(ss.HasMember("last_delivered_seq") && ss["last_delivered_seq"].IsUint64());
        ASSERT_TRUE(ss.HasMember("last_dropped_seq") && ss["last_dropped_seq"].IsUint64());

        const uint64_t input_batches = ss["input_batches"].GetUint64();
        const uint64_t delivered_batches = ss["delivered_batches"].GetUint64();
        const uint64_t dropped_batches = ss["dropped_batches_shared"].GetUint64();
        const uint64_t input_rows = ss["input_rows"].GetUint64();
        const uint64_t delivered_rows = ss["delivered_rows"].GetUint64();
        const uint64_t dropped_rows = ss["dropped_rows_shared"].GetUint64();
        const uint64_t last_delivered_seq = ss["last_delivered_seq"].GetUint64();
        const uint64_t last_dropped_seq = ss["last_dropped_seq"].GetUint64();

        ASSERT_EQ(input_batches, delivered_batches + dropped_batches);
        ASSERT_EQ(input_rows, delivered_rows + dropped_rows);
        ASSERT_TRUE(input_batches > 0);
        ASSERT_TRUE(dropped_batches > 0);
        ASSERT_TRUE(last_delivered_seq <= input_batches);
        ASSERT_TRUE(last_dropped_seq > 0 && last_dropped_seq <= input_batches);

        ASSERT_TRUE(final_doc.HasMember("nodes") && final_doc["nodes"].IsArray());
        ASSERT_EQ(final_doc["nodes"].Size(), 2u);
        uint64_t node1_processed_rows = 0;
        uint64_t node2_processed_rows = 0;
        uint64_t node1_output_rows = 0;
        uint64_t node2_output_rows = 0;
        bool found_n1 = false;
        bool found_n2 = false;
        for (const auto& node : final_doc["nodes"].GetArray()) {
            ASSERT_TRUE(node.IsObject());
            ASSERT_TRUE(node.HasMember("id") && node["id"].IsString());
            ASSERT_TRUE(node.HasMember("processed_rows") && node["processed_rows"].IsUint64());
            ASSERT_TRUE(node.HasMember("output_rows") && node["output_rows"].IsUint64());
            const std::string node_id = node["id"].GetString();
            if (node_id == "n1") {
                found_n1 = true;
                node1_processed_rows = node["processed_rows"].GetUint64();
                node1_output_rows = node["output_rows"].GetUint64();
            } else if (node_id == "n2") {
                found_n2 = true;
                node2_processed_rows = node["processed_rows"].GetUint64();
                node2_output_rows = node["output_rows"].GetUint64();
            }
        }
        ASSERT_TRUE(found_n1);
        ASSERT_TRUE(found_n2);
        ASSERT_EQ(node1_processed_rows, node2_processed_rows);
        ASSERT_EQ(node1_output_rows, node2_output_rows);
        ASSERT_EQ(node1_processed_rows, delivered_rows);
        ASSERT_EQ(node2_processed_rows, delivered_rows);
    }
    std::puts("[PASS] T49.1");

    // T50: Group 共享 stream sink 并发写能力校验（SPSC 拒绝，MPSC 允许）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src1_name = "sink_cap_src1_" + suffix;
        const std::string src2_name = "sink_cap_src2_" + suffix;
        const std::string spsc_sink_name = "sink_cap_spsc_" + suffix;
        const std::string mpsc_sink_name = "sink_cap_mpsc_" + suffix;

        auto make_add_ring_req = [](const std::string& name, const char* mode) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String(mode);
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src1_name, "spsc"), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src2_name, "spsc"), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(spsc_sink_name, "spsc"), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(mpsc_sink_name, "mpsc"), rsp), error::OK);

        const std::string sql1 =
            "SELECT * FROM ring." + src1_name +
            " USING builtin.passthrough_stream INTO stream." + spsc_sink_name;
        const std::string sql2 =
            "SELECT * FROM ring." + src2_name +
            " USING builtin.passthrough_stream INTO stream." + spsc_sink_name;

        const std::string invalid_sql_text = sql1 + ";\n" + sql2 + ";";
        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(invalid_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SINK_CAPABILITY_MISMATCH") != std::string::npos);
        rapidjson::Document invalid_doc;
        invalid_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!invalid_doc.HasParseError() && invalid_doc.IsObject());
        ASSERT_TRUE(invalid_doc.HasMember("sink_key") && invalid_doc["sink_key"].IsString());
        ASSERT_TRUE(invalid_doc.HasMember("required") && invalid_doc["required"].IsObject());
        ASSERT_TRUE(invalid_doc["required"].HasMember("writers") && invalid_doc["required"]["writers"].IsUint());
        ASSERT_EQ(invalid_doc["required"]["writers"].GetUint(), 2u);
        ASSERT_TRUE(invalid_doc.HasMember("actual") && invalid_doc["actual"].IsObject());
        ASSERT_TRUE(invalid_doc["actual"].HasMember("put_mode") && invalid_doc["actual"]["put_mode"].IsString());
        ASSERT_EQ(std::string(invalid_doc["actual"]["put_mode"].GetString()), "SINGLE");

        const std::string sql3 =
            "SELECT * FROM ring." + src1_name +
            " USING builtin.passthrough_stream INTO stream." + mpsc_sink_name;
        const std::string sql4 =
            "SELECT * FROM ring." + src2_name +
            " USING builtin.passthrough_stream INTO stream." + mpsc_sink_name;

        const std::string valid_sql_text = sql3 + ";\n" + sql4 + ";";
        rapidjson::StringBuffer req_ok_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w_ok(req_ok_buf);
        w_ok.StartObject();
        w_ok.Key("execution_kind");
        w_ok.String("group");
        w_ok.Key("group_mode");
        w_ok.String("dag");
        w_ok.Key("sql_text");
        w_ok.String(valid_sql_text.c_str());
        w_ok.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_ok_buf.GetString(), rsp), error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        auto* src1 = stream_factory->Get("ring", src1_name.c_str());
        auto* src2 = stream_factory->Get("ring", src2_name.c_str());
        auto* sink = stream_factory->Get("ring", mpsc_sink_name.c_str());
        ASSERT_TRUE(src1 != nullptr);
        ASSERT_TRUE(src2 != nullptr);
        ASSERT_TRUE(sink != nullptr);

        while (true) {
            PollEvent ev = sink->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(src1->Put(MakeStreamBatch(31), 4001), 0);
        ASSERT_EQ(src2->Put(MakeStreamBatch(32), 4002), 0);
        src1->CloseStream();
        src2->CloseStream();

        bool done = false;
        for (int i = 0; i < 600; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);

        int rows = 0;
        for (int i = 0; i < 200 && rows < 2; ++i) {
            PollEvent ev = sink->PollNext(10);
            if (ev.kind == PollEventKind::kData && ev.batch.data) {
                rows += static_cast<int>(ev.batch.data->num_rows());
            }
        }
        ASSERT_EQ(rows, 2);
    }
    std::puts("[PASS] T50");

    // T50.1: 非 stream sink 并发写限制（同一 dataframe sink 多 writer 拒绝）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src1_name = "nssw_src1_" + suffix;
        const std::string src2_name = "nssw_src2_" + suffix;
        const std::string sink_df_name = "nssw_sink_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src1_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src2_name), rsp), error::OK);

        const std::string sql1 =
            "SELECT * FROM ring." + src1_name +
            " USING builtin.passthrough_stream INTO dataframe." + sink_df_name;
        const std::string sql2 =
            "SELECT * FROM ring." + src2_name +
            " USING builtin.passthrough_stream INTO dataframe." + sink_df_name;
        const std::string sql_text = sql1 + ";\n" + sql2 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER") != std::string::npos);
    }
    std::puts("[PASS] T50.1");

    // T51: Group timeout 返回语义错误码 STREAM_GROUP_TIMEOUT
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string timeout_src_name = "timeout_src_" + suffix;
        const std::string timeout_mid_name = "timeout_mid_" + suffix;
        const std::string timeout_out_name = "timeout_out_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(timeout_src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(timeout_mid_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(timeout_out_name), rsp), error::OK);
        auto* timeout_src = stream_factory->Get("ring", timeout_src_name.c_str());
        ASSERT_TRUE(timeout_src != nullptr);

        const std::string sql1 =
            "SELECT * FROM ring." + timeout_src_name +
            " USING builtin.passthrough_stream INTO stream." + timeout_mid_name;
        const std::string sql2 =
            "SELECT * FROM stream." + timeout_mid_name +
            " USING builtin.passthrough_stream INTO stream." + timeout_out_name;
        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("timeout_s");
        w.Int(1);
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());
        ASSERT_EQ(timeout_src->Put(MakeStreamBatch(99), 5001), 0);

        bool done = false;
        std::string final_status;
        std::string final_rsp;
        for (int i = 0; i < 600; ++i) {
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), final_rsp), error::OK);
            final_status = ParseStatus(final_rsp);
            if (final_status == "failed" || final_status == "stopped" || final_status == "cancelled") {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(final_status, "failed");

        rapidjson::Document final_doc;
        final_doc.Parse(final_rsp.c_str());
        ASSERT_TRUE(!final_doc.HasParseError() && final_doc.IsObject());
        ASSERT_TRUE(final_doc.HasMember("error_code") && final_doc["error_code"].IsString());
        ASSERT_EQ(std::string(final_doc["error_code"].GetString()), "STREAM_GROUP_TIMEOUT");
        ASSERT_TRUE(final_doc.HasMember("error_message") && final_doc["error_message"].IsString());
        ASSERT_TRUE(std::string(final_doc["error_message"].GetString()).find("unfinished_nodes=") != std::string::npos);
    }
    std::puts("[PASS] T51");

    // T52: Scheduler stream execute 契约护栏（拒绝 legacy/dag 冲突输入）
    {
        std::string rsp;

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"group",
                                  "group_mode":"dag",
                                  "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;SELECT * FROM stream.b USING builtin.passthrough_stream INTO stream.c;",
                                  "dag":{"nodes":[]}
                              })",
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"group",
                                  "group_mode":"dag",
                                  "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"single",
                                  "group_mode":"dag",
                                  "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"single",
                                  "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;SELECT * FROM ring.c USING builtin.passthrough_stream INTO stream.d;"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);

        // Single execute plan-stage validation error should still return typed error contract.
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"single",
                                  "sql_text":"SELECT * FROM ring.in INTO stream.out"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        rapidjson::Document single_plan_err_doc;
        single_plan_err_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!single_plan_err_doc.HasParseError() && single_plan_err_doc.IsObject());
        ASSERT_TRUE(single_plan_err_doc.HasMember("error_code") && single_plan_err_doc["error_code"].IsString());
        ASSERT_TRUE(single_plan_err_doc.HasMember("error_stage") && single_plan_err_doc["error_stage"].IsString());
        ASSERT_EQ(std::string(single_plan_err_doc["error_code"].GetString()), "SQL_TEXT_INVALID");
        ASSERT_EQ(std::string(single_plan_err_doc["error_stage"].GetString()), "parse");

        // sql_text split error should expose 0-based sql_index.
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"single",
                                  "sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out;;SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out;"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        rapidjson::Document split_doc;
        split_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!split_doc.HasParseError() && split_doc.IsObject());
        ASSERT_TRUE(split_doc.HasMember("error_code") && split_doc["error_code"].IsString());
        ASSERT_EQ(std::string(split_doc["error_code"].GetString()), "STREAM_GROUP_SQL_TEXT_INVALID");
        ASSERT_TRUE(split_doc.HasMember("sql_index") && split_doc["sql_index"].IsUint64());
        ASSERT_EQ(split_doc["sql_index"].GetUint64(), 1u);

        // Group node parse error should expose 0-based sql_index.
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"group",
                                  "group_mode":"dag",
                                  "sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out;SELECT FROM ring.in USING builtin.passthrough_stream INTO stream.out;"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        rapidjson::Document node_parse_doc;
        node_parse_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!node_parse_doc.HasParseError() && node_parse_doc.IsObject());
        ASSERT_TRUE(node_parse_doc.HasMember("error_code") && node_parse_doc["error_code"].IsString());
        ASSERT_EQ(std::string(node_parse_doc["error_code"].GetString()), "STREAM_GROUP_DAG_INVALID");
        ASSERT_TRUE(node_parse_doc.HasMember("sql_index") && node_parse_doc["sql_index"].IsUint64());
        ASSERT_EQ(node_parse_doc["sql_index"].GetUint64(), 1u);

        // timeout_s must not exceed max_stream_group_timeout_s (default 86400).
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              R"({
                                  "execution_kind":"group",
                                  "group_mode":"dag",
                                  "timeout_s":86401,
                                  "sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out;SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out;"
                              })",
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);
        ASSERT_TRUE(rsp.find("max_stream_group_timeout_s") != std::string::npos);
    }
    std::puts("[PASS] T52");

    // T53: Hybrid Group DAG（stream -> batch）执行与 node_kind/sql_index 可观测
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "hybrid_src_" + suffix;
        const std::string mid_df_name = "hybrid_mid_" + suffix;
        const std::string out_df_name = "hybrid_out_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name), rsp), error::OK);
        auto* src = stream_factory->Get("ring", src_name.c_str());
        ASSERT_TRUE(src != nullptr);

        const std::string sql1 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO dataframe." + mid_df_name;
        const std::string sql2 =
            "SELECT * FROM dataframe." + mid_df_name +
            " INTO dataframe." + out_df_name;
        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        ASSERT_EQ(src->Put(MakeStreamBatch(41), 5001), 0);
        ASSERT_EQ(src->Put(MakeStreamBatch(42), 5002), 0);
        src->CloseStream();

        bool done = false;
        for (int i = 0; i < 800; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("nodes") && status_doc["nodes"].IsArray());
            ASSERT_EQ(status_doc["nodes"].Size(), 2u);
            for (const auto& node : status_doc["nodes"].GetArray()) {
                ASSERT_TRUE(node.IsObject());
                ASSERT_TRUE(node.HasMember("node_kind") && node["node_kind"].IsString());
                ASSERT_TRUE(node.HasMember("sql_index") && node["sql_index"].IsUint64());
                ASSERT_TRUE(node.HasMember("phase") && node["phase"].IsString());
            }
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);

        auto out_ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(out_df_name.c_str()));
        ASSERT_TRUE(out_ch != nullptr);
        DataFrame out;
        ASSERT_EQ(out_ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 2);
    }
    std::puts("[PASS] T53");

    // T54: Hybrid Group DAG（batch -> stream -> batch）三段式执行
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string seed_df_name = "hybrid_seed_df_" + suffix;
        const std::string stage_stream_name = "hybrid_stage_stream_" + suffix;
        const std::string stage_df_name = "hybrid_stage_df_" + suffix;
        const std::string out_df_name = "hybrid_out_df_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(512);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(stage_stream_name), rsp), error::OK);
        auto* stage_stream = stream_factory->Get("ring", stage_stream_name.c_str());
        ASSERT_TRUE(stage_stream != nullptr);
        while (true) {
            PollEvent ev = stage_stream->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src INTO dataframe." + seed_df_name),
                       rsp),
                  error::OK);

        const std::string sql1 =
            "SELECT * FROM dataframe." + seed_df_name +
            " INTO ring." + stage_stream_name;
        const std::string sql2 =
            "SELECT * FROM ring." + stage_stream_name +
            " USING builtin.passthrough_stream INTO dataframe." + stage_df_name;
        const std::string sql3 =
            "SELECT * FROM dataframe." + stage_df_name +
            " INTO dataframe." + out_df_name;
        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";\n" + sql3 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        bool done = false;
        for (int i = 0; i < 1000; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("runtime_kind") && status_doc["runtime_kind"].IsString());
            ASSERT_EQ(std::string(status_doc["runtime_kind"].GetString()), "group");
            ASSERT_TRUE(status_doc.HasMember("nodes") && status_doc["nodes"].IsArray());
            ASSERT_EQ(status_doc["nodes"].Size(), 3u);

            bool seen_batch = false;
            bool seen_stream = false;
            for (const auto& node : status_doc["nodes"].GetArray()) {
                ASSERT_TRUE(node.IsObject());
                ASSERT_TRUE(node.HasMember("node_kind") && node["node_kind"].IsString());
                ASSERT_TRUE(node.HasMember("sql_index") && node["sql_index"].IsUint64());
                ASSERT_TRUE(node.HasMember("phase") && node["phase"].IsString());
                const std::string node_kind = node["node_kind"].GetString();
                if (node_kind == "batch") seen_batch = true;
                if (node_kind == "stream") seen_stream = true;
            }
            ASSERT_TRUE(seen_batch);
            ASSERT_TRUE(seen_stream);

            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);

        auto out_ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(out_df_name.c_str()));
        ASSERT_TRUE(out_ch != nullptr);
        DataFrame out;
        ASSERT_EQ(out_ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
    }
    std::puts("[PASS] T54");

    // T54.2: Hybrid Group DAG（batch -> stream_hub(split) -> stream selector）可省略 USING
    {
        std::string rsp;

        const std::string seed_df_name = "hub_seed_df_" + suffix;
        const std::string hub_name = "hub_split_" + suffix;
        const std::string out0_name = "hub_out0_" + suffix;
        const std::string out1_name = "hub_out1_" + suffix;
        const std::string out2_name = "hub_out2_" + suffix;
        const std::string out3_name = "hub_out3_" + suffix;

        auto make_add_hub_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("stream_hub");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("mode");
            w.String("split");
            w.Key("partition_count");
            w.Int(4);
            w.Key("partition_ring_mode");
            w.String("spsc");
            w.Key("partition_ring_size");
            w.Int(256);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_hub_req(hub_name), rsp), error::OK);
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src INTO dataframe." + seed_df_name),
                       rsp),
                  error::OK);

        const std::string sql1 =
            "SELECT * FROM dataframe." + seed_df_name + " INTO stream_hub." + hub_name;
        const std::string sql2 =
            "SELECT * FROM stream_hub." + hub_name + "[0] INTO dataframe." + out0_name;
        const std::string sql3 =
            "SELECT * FROM stream_hub." + hub_name + "[1] INTO dataframe." + out1_name;
        const std::string sql4 =
            "SELECT * FROM stream_hub." + hub_name + "[2] INTO dataframe." + out2_name;
        const std::string sql5 =
            "SELECT * FROM stream_hub." + hub_name + "[3] INTO dataframe." + out3_name;
        const std::string group_sql_text =
            sql1 + ";\n" + sql2 + ";\n" + sql3 + ";\n" + sql4 + ";\n" + sql5 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        bool done = false;
        for (int i = 0; i < 1200; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "failed" || st == "cancelled") {
                ASSERT_EQ(st, "stopped");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);

        int total_rows = 0;
        int non_empty_count = 0;
        const std::string out_names[] = {out0_name, out1_name, out2_name, out3_name};
        for (const auto& out_name : out_names) {
            auto out_ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(out_name.c_str()));
            ASSERT_TRUE(out_ch != nullptr);
            DataFrame out;
            ASSERT_EQ(out_ch->Read(&out), 0);
            total_rows += static_cast<int>(out.RowCount());
            if (out.RowCount() > 0) ++non_empty_count;
        }
        ASSERT_EQ(total_rows, 3);
        ASSERT_TRUE(non_empty_count >= 1);
    }
    std::puts("[PASS] T54.2");

    // T54.1: 非 root 同源分支自动构建 share_set（stream.mid -> n2/n3）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "non_root_share_src_" + suffix;
        const std::string mid_name = "non_root_share_mid_" + suffix;
        const std::string out1_name = "non_root_share_out1_" + suffix;
        const std::string out2_name = "non_root_share_out2_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(mid_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out1_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out2_name), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        auto* out1 = stream_factory->Get("ring", out1_name.c_str());
        auto* out2 = stream_factory->Get("ring", out2_name.c_str());
        ASSERT_TRUE(src != nullptr);
        ASSERT_TRUE(out1 != nullptr);
        ASSERT_TRUE(out2 != nullptr);

        while (true) {
            PollEvent ev = out1->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }
        while (true) {
            PollEvent ev = out2->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        const std::string sql1 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + mid_name;
        const std::string sql2 =
            "SELECT * FROM stream." + mid_name +
            " USING builtin.passthrough_stream INTO stream." + out1_name;
        const std::string sql3 =
            "SELECT * FROM stream." + mid_name +
            " USING builtin.passthrough_stream INTO stream." + out2_name;
        const std::string group_sql_text = sql1 + ";\n" + sql2 + ";\n" + sql3 + ";";

        rapidjson::StringBuffer req_buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
        w.StartObject();
        w.Key("execution_kind");
        w.String("group");
        w.Key("group_mode");
        w.String("dag");
        w.Key("sql_text");
        w.String(group_sql_text.c_str());
        w.EndObject();

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", req_buf.GetString(), rsp), error::OK);
        rapidjson::Document submit_doc;
        submit_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!submit_doc.HasParseError() && submit_doc.IsObject());
        ASSERT_TRUE(submit_doc.HasMember("share_set_count") && submit_doc["share_set_count"].IsUint());
        ASSERT_EQ(submit_doc["share_set_count"].GetUint(), 1u);
        const std::string group_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!group_task_id.empty());

        ASSERT_EQ(src->Put(MakeStreamBatch(3101), 9101), 0);
        ASSERT_EQ(src->Put(MakeStreamBatch(3102), 9102), 0);
        ASSERT_EQ(src->Put(MakeStreamBatch(3103), 9103), 0);
        src->CloseStream();

        bool status_observed = false;
        for (int i = 0; i < 1000; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(group_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("share_sets") && status_doc["share_sets"].IsArray());
            ASSERT_EQ(status_doc["share_sets"].Size(), 1u);
            const std::string st = ParseStatus(status_rsp);
            if (st == "running" || st == "preparing" || st == "stopping" ||
                st == "stopped" || st == "failed" || st == "cancelled") {
                status_observed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(status_observed);

        int rows1 = 0;
        int rows2 = 0;
        for (int i = 0; i < 400 && (rows1 < 3 || rows2 < 3); ++i) {
            PollEvent ev1 = out1->PollNext(10);
            if (ev1.kind == PollEventKind::kData && ev1.batch.data) {
                rows1 += static_cast<int>(ev1.batch.data->num_rows());
            }
            PollEvent ev2 = out2->PollNext(10);
            if (ev2.kind == PollEventKind::kData && ev2.batch.data) {
                rows2 += static_cast<int>(ev2.batch.data->num_rows());
            }
        }
        ASSERT_EQ(rows1, 3);
        ASSERT_EQ(rows2, 3);

        std::string stop_rsp;
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(group_task_id), stop_rsp), error::OK);
        ASSERT_EQ(ParseStatus(stop_rsp), "stopped");
    }
    std::puts("[PASS] T54.1");

    // T60/T61/T62: 同源跨任务共享 + late join + stop 隔离
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "late_join_src_" + suffix;
        const std::string out1_name = "late_join_out1_" + suffix;
        const std::string out2_name = "late_join_out2_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out1_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out2_name), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        auto* out1 = stream_factory->Get("ring", out1_name.c_str());
        auto* out2 = stream_factory->Get("ring", out2_name.c_str());
        ASSERT_TRUE(src != nullptr);
        ASSERT_TRUE(out1 != nullptr);
        ASSERT_TRUE(out2 != nullptr);

        auto drain_channel = [](IStreamChannel* ch) {
            if (!ch) return;
            while (true) {
                PollEvent ev = ch->PollNext(0);
                if (ev.kind != PollEventKind::kData) break;
            }
        };
        drain_channel(out1);
        drain_channel(out2);

        const std::string sql1 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out1_name;
        const std::string sql2 =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out2_name;

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(sql1), rsp), error::OK);
        const std::string task1 = ParseTaskId(rsp);
        ASSERT_TRUE(!task1.empty());

        bool task1_ready = false;
        for (int i = 0; i < 200; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task1), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "running" || st == "submitted") {
                task1_ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(task1_ready);

        ASSERT_EQ(src->Put(MakeStreamBatch(5001), 8001), 0);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(sql2), rsp), error::OK);
        const std::string task2 = ParseTaskId(rsp);
        ASSERT_TRUE(!task2.empty());

        bool task2_ready = false;
        for (int i = 0; i < 200; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task2), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "running" || st == "submitted") {
                task2_ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(task2_ready);

        {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task2), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("shared_hub_id") && status_doc["shared_hub_id"].IsString());
            ASSERT_TRUE(std::string(status_doc["shared_hub_id"].GetString()).size() > 0);
            ASSERT_TRUE(status_doc.HasMember("shared_source_keys") && status_doc["shared_source_keys"].IsArray());
            ASSERT_TRUE(status_doc["shared_source_keys"].Size() >= 1u);
            ASSERT_TRUE(status_doc.HasMember("subscriber_count") && status_doc["subscriber_count"].IsUint());
            ASSERT_TRUE(status_doc["subscriber_count"].GetUint() >= 2u);
            ASSERT_TRUE(status_doc.HasMember("subscriber_stats") && status_doc["subscriber_stats"].IsArray());
            ASSERT_TRUE(status_doc["subscriber_stats"].Size() >= 2u);
            for (const auto& item : status_doc["subscriber_stats"].GetArray()) {
                ASSERT_TRUE(item.IsObject());
                ASSERT_TRUE(item.HasMember("lag") && item["lag"].IsUint64());
            }
        }

        auto drain_values = [](IStreamChannel* ch, std::vector<int64_t>* out) {
            if (!ch || !out) return;
            while (true) {
                PollEvent ev = ch->PollNext(0);
                if (ev.kind != PollEventKind::kData || !ev.batch.data) break;
                auto col = ev.batch.data->column(0);
                auto arr = std::dynamic_pointer_cast<arrow::Int64Array>(col);
                if (!arr) continue;
                for (int64_t r = 0; r < arr->length(); ++r) {
                    if (arr->IsValid(r)) out->push_back(arr->Value(r));
                }
            }
        };
        auto contains_value = [](const std::vector<int64_t>& values, int64_t v) {
            return std::find(values.begin(), values.end(), v) != values.end();
        };

        std::vector<int64_t> vals1_acc;
        std::vector<int64_t> vals2_acc;

        ASSERT_EQ(src->Put(MakeStreamBatch(5002), 8002), 0);
        bool both_seen_5002 = false;
        for (int i = 0; i < 300; ++i) {
            drain_values(out1, &vals1_acc);
            drain_values(out2, &vals2_acc);
            if (contains_value(vals1_acc, 5002) && contains_value(vals2_acc, 5002)) {
                both_seen_5002 = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(both_seen_5002);

        std::string stop_rsp;
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(task1), stop_rsp), error::OK);
        bool task1_stopped = false;
        for (int i = 0; i < 300; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task1), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "cancelled" || st == "failed") {
                ASSERT_EQ(st, "stopped");
                task1_stopped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(task1_stopped);

        ASSERT_EQ(src->Put(MakeStreamBatch(5003), 8003), 0);
        src->CloseStream();

        bool task2_done = false;
        for (int i = 0; i < 600; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task2), status_rsp), error::OK);
            const std::string st = ParseStatus(status_rsp);
            if (st == "stopped" || st == "cancelled" || st == "failed") {
                ASSERT_EQ(st, "stopped");
                task2_done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(task2_done);

        auto collect_values = [](IStreamChannel* ch) {
            std::vector<int64_t> values;
            for (int i = 0; i < 400; ++i) {
                PollEvent ev = ch->PollNext(10);
                if (ev.kind == PollEventKind::kData && ev.batch.data) {
                    auto col = ev.batch.data->column(0);
                    auto arr = std::dynamic_pointer_cast<arrow::Int64Array>(col);
                    if (!arr) continue;
                    for (int64_t r = 0; r < arr->length(); ++r) {
                        if (arr->IsValid(r)) values.push_back(arr->Value(r));
                    }
                    continue;
                }
                if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
                    break;
                }
            }
            return values;
        };

        std::vector<int64_t> vals1 = std::move(vals1_acc);
        std::vector<int64_t> vals2 = std::move(vals2_acc);
        auto vals1_tail = collect_values(out1);
        auto vals2_tail = collect_values(out2);
        vals1.insert(vals1.end(), vals1_tail.begin(), vals1_tail.end());
        vals2.insert(vals2.end(), vals2_tail.begin(), vals2_tail.end());

        ASSERT_TRUE(contains_value(vals1, 5001));
        ASSERT_TRUE(contains_value(vals1, 5002));
        ASSERT_TRUE(contains_value(vals2, 5002));
        ASSERT_TRUE(contains_value(vals2, 5003));
        ASSERT_TRUE(!contains_value(vals2, 5001));
    }
    std::puts("[PASS] T60/T61/T62");

    // T68: 跨任务共享 source 慢消费者背压可观测（drop + lag）
    {
        std::string rsp;
        ASSERT_EQ(op_registry->Register("custom.slow_passthrough_stream", []() -> IOperator* {
            return new SlowPassthroughStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"slow_passthrough_stream",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e slow passthrough stream",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.slow_passthrough_stream"})", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "late_join_pressure_src_" + suffix;
        const std::string out_fast_name = "late_join_pressure_fast_" + suffix;
        const std::string out_slow_name = "late_join_pressure_slow_" + suffix;

        auto make_add_ring_req = [](const std::string& name, int ring_size) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(ring_size);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name, 16384), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out_fast_name, 16384), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out_slow_name, 16384), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        ASSERT_TRUE(src != nullptr);

        const std::string sql_fast =
            "SELECT * FROM ring." + src_name +
            " USING builtin.passthrough_stream INTO stream." + out_fast_name;
        const std::string sql_slow =
            "SELECT * FROM ring." + src_name +
            " USING custom.slow_passthrough_stream INTO stream." + out_slow_name;

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(sql_fast), rsp), error::OK);
        const std::string fast_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!fast_task_id.empty());

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(sql_slow), rsp), error::OK);
        const std::string slow_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!slow_task_id.empty());

        bool fast_ready = false;
        bool slow_ready = false;
        for (int i = 0; i < 300; ++i) {
            if (!fast_ready) {
                std::string status_rsp;
                ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(fast_task_id), status_rsp), error::OK);
                const std::string st = ParseStatus(status_rsp);
                if (st == "running" || st == "submitted") fast_ready = true;
            }
            if (!slow_ready) {
                std::string status_rsp;
                ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(slow_task_id), status_rsp), error::OK);
                const std::string st = ParseStatus(status_rsp);
                if (st == "running" || st == "submitted") slow_ready = true;
            }
            if (fast_ready && slow_ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(fast_ready);
        ASSERT_TRUE(slow_ready);

        constexpr int kInputBatches = 3200;
        int accepted_batches = 0;
        for (int i = 0; i < kInputBatches; ++i) {
            if (src->Put(MakeStreamBatch(700000 + i), 9000 + i) == 0) {
                ++accepted_batches;
            }
        }
        ASSERT_TRUE(accepted_batches > 0);

        bool observed_drop = false;
        bool observed_lag = false;
        for (int i = 0; i < 500; ++i) {
            std::string status_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(slow_task_id), status_rsp), error::OK);
            rapidjson::Document status_doc;
            status_doc.Parse(status_rsp.c_str());
            ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
            ASSERT_TRUE(status_doc.HasMember("subscriber_stats") && status_doc["subscriber_stats"].IsArray());
            for (const auto& item : status_doc["subscriber_stats"].GetArray()) {
                ASSERT_TRUE(item.IsObject());
                ASSERT_TRUE(item.HasMember("runtime_task_id") && item["runtime_task_id"].IsString());
                ASSERT_TRUE(item.HasMember("dropped_batches") && item["dropped_batches"].IsUint64());
                ASSERT_TRUE(item.HasMember("lag") && item["lag"].IsUint64());
                if (std::string(item["runtime_task_id"].GetString()) == slow_task_id) {
                    if (item["dropped_batches"].GetUint64() > 0) observed_drop = true;
                    if (item["lag"].GetUint64() > 0) observed_lag = true;
                }
            }
            if (observed_drop && observed_lag) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        src->CloseStream();

        bool fast_done = false;
        bool slow_done = false;
        for (int i = 0; i < 2000; ++i) {
            if (!fast_done) {
                std::string status_rsp;
                ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(fast_task_id), status_rsp), error::OK);
                const std::string st = ParseStatus(status_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    ASSERT_EQ(st, "stopped");
                    fast_done = true;
                }
            }
            if (!slow_done) {
                std::string status_rsp;
                ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(slow_task_id), status_rsp), error::OK);
                const std::string st = ParseStatus(status_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    ASSERT_EQ(st, "stopped");
                    slow_done = true;
                }
            }
            if (fast_done && slow_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(fast_done);
        ASSERT_TRUE(slow_done);
        ASSERT_TRUE(observed_drop);
        ASSERT_TRUE(observed_lag);
    }
    std::puts("[PASS] T68");

    // T66/T67: WHERE 同签名共享成功，异签名显式拒绝
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        const std::string src_name = "where_src_" + suffix;
        const std::string out1_name = "where_out1_" + suffix;
        const std::string out2_name = "where_out2_" + suffix;
        const std::string out3_name = "where_out3_" + suffix;

        auto make_add_ring_req = [](const std::string& name) {
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("type");
            w.String("ring");
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String("both");
            w.Key("options");
            w.StartObject();
            w.Key("ring_mode");
            w.String("spsc");
            w.Key("ring_size");
            w.Int(256);
            w.Key("overflow");
            w.String("drop");
            w.Key("finite");
            w.Bool(false);
            w.EndObject();
            w.EndObject();
            return std::string(buf.GetString());
        };

        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(src_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out1_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out2_name), rsp), error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add", make_add_ring_req(out3_name), rsp), error::OK);

        auto* src = stream_factory->Get("ring", src_name.c_str());
        ASSERT_TRUE(src != nullptr);

        const std::string where_sql_1 =
            "SELECT * FROM ring." + src_name +
            " WHERE v >= 0 USING builtin.passthrough_stream INTO stream." + out1_name;
        const std::string where_sql_2 =
            "SELECT * FROM ring." + src_name +
            " WHERE v >= 0 USING builtin.passthrough_stream INTO stream." + out2_name;
        const std::string where_sql_3 =
            "SELECT * FROM ring." + src_name +
            " WHERE v >= 1 USING builtin.passthrough_stream INTO stream." + out3_name;

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(where_sql_1), rsp), error::OK);
        const std::string task_same_1 = ParseTaskId(rsp);
        ASSERT_TRUE(!task_same_1.empty());

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(where_sql_2), rsp), error::OK);
        const std::string task_same_2 = ParseTaskId(rsp);
        ASSERT_TRUE(!task_same_2.empty());

        ASSERT_EQ(stream_exec("/scheduler/stream/execute", MakeStreamReq(where_sql_3), rsp), error::CONFLICT);
        rapidjson::Document mismatch_doc;
        mismatch_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!mismatch_doc.HasParseError() && mismatch_doc.IsObject());
        ASSERT_TRUE(mismatch_doc.HasMember("error_code") && mismatch_doc["error_code"].IsString());
        ASSERT_EQ(std::string(mismatch_doc["error_code"].GetString()), "SHARED_SOURCE_WHERE_MISMATCH");

        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(task_same_1), rsp), error::OK);
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(task_same_2), rsp), error::OK);
        src->CloseStream();
    }
    std::puts("[PASS] T66/T67");

    exec = fnRouterHandler();
    stream_exec = fnRouterHandler();
    stream_stop = fnRouterHandler();
    stream_status = fnRouterHandler();
    stream_list = fnRouterHandler();
    stream_add = fnRouterHandler();
    stream_remove = fnRouterHandler();
    stream_reset = fnRouterHandler();
    stream_definitions_query = fnRouterHandler();
    sql_classify = fnRouterHandler();
    activate = fnRouterHandler();
    deactivate = fnRouterHandler();
    upsert_batch = fnRouterHandler();
    loader->StopAll();
    loader->Unload();
    std::filesystem::remove(db_path);
    std::filesystem::remove(stream_cfg);
    std::filesystem::remove(stream_meta_db);
    std::filesystem::remove(pcap_ok);
    std::filesystem::remove(pcap_error);
    std::filesystem::remove_all(data_dir);
    std::filesystem::remove_all(operator_db_dir);

    std::puts("=== All Scheduler E2E tests passed ===");
    return 0;
}
