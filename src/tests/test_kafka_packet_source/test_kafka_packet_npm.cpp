// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_packet_reader.h>
#include <framework/core/packet_family_codec.h>
#include <framework/interfaces/iblock_stream_channel_descriptor.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iflow_labeling.h>
#include <plugins/npi/packet_decoder.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <map>
#include <set>
#include "packet_test_runtime.h"
using namespace packet_test;
namespace {
class Labels final : public IConfigChannelRegistryV1 {
 public:
    int Resolve(const char* reference, ConfigChannelSnapshot* output, std::string*) override {
        if (!reference || std::string(reference) != "config.packet-test@1" || !output) return EINVAL;
        *output = {};
        output->channel_name = "packet-test";
        output->revision = 1;
        output->format = "yaml";
        output->schema_id = "flowsql.io/flow-labeling/v1alpha1";
        output->sha256_hex = "frozen-packet-test-source";
        output->content = std::make_shared<const std::string>(R"YAML(api_version: flowsql.io/flow-labeling/v1alpha1
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
      name: packet-fixture
  rules:
    - id: all-ports
      label_id: 1001
      direction: bidirectional
      matches:
        - field: destination_port
          min: 1
          max: 65535
)YAML");
        output->content_bytes = output->content->size();
        return 0;
    }
};
PacketSourceConfig Config() {
    return ParsePacketSourceConfig(Parse(
        R"({"encoding":"flowsql_packet_v1","stream_id":"probe","observation_domain_id":"7","max_packets_per_batch":4096,"max_bytes_per_batch":16777216,"max_message_bytes":16777216,"inputs":[{"source_id":0,"queue_id":0,"source_name":"a"},{"source_id":1,"queue_id":0,"source_name":"b"}]})"));
}
std::vector<PacketEnvelope> Sequence() {
    PacketEnvelope e;
    e.stream_epoch[0] = 1;
    e.observation_domain_id = 7;
    e.capture_generation = 77;
    e.poll_sequence = 1;
    uint64_t seq = 0;
    // Read the independent PCAP fixture format, without using the Kafka codec or layer cache.
    for (const auto* file :
         {"Dns.pcap", "http-packets.pcap", "https-packets.pcap", "icmp-packets.pcap", "ipv6ssdp-packets.pcap"}) {
        std::ifstream input(std::string(PACKET_DATA_DIR) + "/" + file, std::ios::binary);
        Check(bool(input), "pcap_missing");
        unsigned char global[24];
        input.read(reinterpret_cast<char*>(global), 24);
        const bool little = global[0] == 0xd4 || global[0] == 0x4d;
        auto u32 = [&](const unsigned char* p) {
            uint32_t n = 0;
            for (int i = 0; i < 4; ++i) n |= uint32_t(p[i]) << (8 * (little ? i : 3 - i));
            return n;
        };
        Check(u32(global + 20) == 1, "pcap_ethernet");
        unsigned char header[16];
        while (input.read(reinterpret_cast<char*>(header), 16)) {
            const auto size = u32(header + 8), wire = u32(header + 12);
            Check(size <= 65536 && wire >= size, "pcap_size");
            std::string raw(size, '\0');
            input.read(raw.data(), size);
            Check(bool(input), "pcap_packet");
            ++seq;
            e.packets.push_back({{100000000000LL + static_cast<int64_t>(seq) * 100000, size, wire, 1,
                                  static_cast<uint32_t>(seq % 2), seq},
                                 std::move(raw)});
            const auto original = e.packets.back();
            const auto& bytes = original.bytes;
            if (bytes.size() >= 34 && static_cast<uint8_t>(bytes[12]) == 8 && bytes[13] == 0 &&
                (static_cast<uint8_t>(bytes[14]) >> 4) == 4) {
                const size_t ip_header = (static_cast<uint8_t>(bytes[14]) & 15) * 4;
                if (ip_header >= 20 && bytes.size() >= 14 + ip_header) {
                    std::string v6 = bytes.substr(0, 14) + std::string(40, '\0');
                    v6[12] = '\x86';
                    v6[13] = '\xdd';
                    v6[14] = '\x60';
                    const uint16_t payload = (static_cast<uint8_t>(bytes[16]) << 8) | static_cast<uint8_t>(bytes[17]);
                    const auto length = payload >= ip_header ? payload - ip_header : 0;
                    v6[18] = static_cast<char>(length >> 8);
                    v6[19] = static_cast<char>(length);
                    v6[20] = bytes[23];
                    v6[21] = 64;
                    v6.replace(34, 4, bytes.substr(26, 4));
                    v6.replace(50, 4, bytes.substr(30, 4));
                    v6 += bytes.substr(14 + ip_header);
                    if (bytes[23] == 1 && v6.size() > 54) {
                        v6[20] = 58;
                        if (v6[54] == 8)
                            v6[54] = '\x80';
                        else if (v6[54] == 0)
                            v6[54] = '\x81';
                    }
                    auto meta = original.meta;
                    meta.captured_len = v6.size();
                    meta.wire_len = wire + 40 - ip_header;
                    meta.sequence = ++seq;
                    meta.timestamp_ns = 100000000000LL + static_cast<int64_t>(seq) * 100000;
                    e.packets.push_back({meta, std::move(v6)});
                }
            }
        }
    }
    // Independent, known SYN origins cover protocol fixtures that begin midstream in the PCAPs.
    for (bool six : {false, true}) {
        for (bool tls : {false, true}) {
            std::string body = "GET /packet HTTP/1.1\r\nHost: example.com\r\n\r\n";
            if (tls) {
                const unsigned char hello[] = {22, 3, 3, 0, 45, 1, 0, 0, 41, 3, 3};
                body.assign(reinterpret_cast<const char*>(hello), sizeof(hello));
                body.append(32, 'c');
                body.append("\0\0\2\x13\1\1\0", 7);
            }
            const size_t tcp = six ? 54 : 34;
            std::string raw(tcp + 20, '\0');
            raw[0] = 1;
            raw[6] = 2;
            raw[12] = six ? '\x86' : '\x08';
            raw[13] = six ? '\xdd' : 0;
            raw[14] = six ? '\x60' : '\x45';
            if (six) {
                raw[20] = 6;
                raw[21] = 64;
                raw[22] = raw[38] = '\x20';
                raw[37] = 1;
                raw[53] = 2;
            } else {
                raw[22] = 64;
                raw[23] = 6;
                raw[26] = raw[30] = 10;
                raw[29] = 1;
                raw[33] = 2;
            }
            raw[tcp] = '\xc0';
            raw[tcp + 1] = tls ? 2 : 1;
            raw[tcp + 2] = tls ? 1 : 0;
            raw[tcp + 3] = tls ? '\xbb' : 80;
            raw[tcp + 7] = 1;
            raw[tcp + 12] = '\x50';
            raw[tcp + 13] = 2;
            raw += body;
            const auto length = raw.size() - (six ? 54 : 14);
            raw[six ? 18 : 16] = static_cast<char>(length >> 8);
            raw[six ? 19 : 17] = static_cast<char>(length);
            ++seq;
            e.packets.push_back({{100000000000LL + static_cast<int64_t>(seq) * 100000,
                                  static_cast<uint32_t>(raw.size()), static_cast<uint32_t>(raw.size()), 1, 0, seq},
                                 std::move(raw)});
        }
    }
    Check(e.packets.size() > 20 && e.packets.size() < 4096, "pcap_count");
    for (uint32_t id = 0; id < 2; ++id) {
        CaptureProgressV1 f;
        f.source_id = id;
        f.fact_sequence = 1;
        f.packet_observed = true;
        for (const auto& p : e.packets)
            if (p.meta.source_id == id) f.capture_time_ns = p.meta.timestamp_ns;
        f.backlog = CaptureBacklogV1::kEmpty;
        e.progress.push_back(f);
    }
    std::vector<PacketEnvelope> polls{e};
    // Remote backlog/unknown and backwards time suspend the common candidate; explicit idle resumes it.
    for (const auto [time, backlog] : {std::pair<int64_t, CaptureBacklogV1>{120, CaptureBacklogV1::kEmpty},
                                       {135, CaptureBacklogV1::kPresent},
                                       {150, CaptureBacklogV1::kEmpty},
                                       {140, CaptureBacklogV1::kEmpty},
                                       {180, CaptureBacklogV1::kEmpty},
                                       {200, CaptureBacklogV1::kUnknown},
                                       {240, CaptureBacklogV1::kEmpty}}) {
        auto idle = e;
        idle.packets.clear();
        idle.poll_sequence = polls.size() + 1;
        for (auto& f : idle.progress) {
            f.fact_sequence = idle.poll_sequence;
            f.packet_observed = false;
            f.source_idle_confirmed = true;
            f.capture_time_ns = (time - 3 * f.source_id) * 1000000000;
            f.backlog = backlog;
        }
        polls.push_back(std::move(idle));
    }
    return polls;
}
struct State {
    std::atomic<size_t> released{0};
    std::atomic<size_t> permitted{1};
};
class Local final : public ICaptureBlockStreamReaderV2, public IBlockStreamSchemaV1 {
 public:
    Local(std::string name, std::vector<PacketEnvelope> polls, IProtocol* protocol, std::shared_ptr<State> state)
        : name_(std::move(name)), polls_(std::move(polls)), protocol_(protocol), state_(std::move(state)) {}
    const char* Category() override { return "kafka"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override { return packet::PacketFamilySchema(); }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    bool IsFinished() const override { return cancelled_; }
    void Cancel() override { cancelled_ = true; }
    int DescribeSources(CaptureSourceSetV2* sources) const override {
        sources->inputs.clear();
        for (uint32_t i = 0; i < 2; ++i) {
            CaptureQueueIdentityV1 id;
            id.source_name = i ? "b" : "a";
            id.source_id = i;
            id.generation = 900;
            id.observation_domain_id = 7;
            id.link_type = 1;
            sources->inputs.push_back(id);
        }
        sources->limits.max_packets_per_batch = 4096;
        sources->limits.max_bytes_per_batch = 16777216;
        sources->limits.max_wait_ms = 100;
        sources->limits.max_outstanding_batches = 1;
        return 0;
    }
    int ReadInputCounters(uint32_t, CaptureCountersV1*) const override { return ENOTSUP; }
    BlockPollEvent PollBlock(int timeout) override { return PollCapture(timeout).block; }
    CapturePollEventV2 PollCapture(int) override {
        CapturePollEventV2 event;
        if (cancelled_) {
            event.block.kind = BlockPollEvent::kCancelled;
            return event;
        }
        if (index_ == polls_.size() || index_ >= state_->permitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return event;
        }
        const auto& poll = polls_[index_++];
        packet::PacketFamilyBatch rows;
        protocol::NpiPacketLayerDecoder decoder(protocol_);
        auto owner = std::make_shared<PacketEnvelope>(poll);
        for (const auto& p : owner->packets) {
            packet::PacketView view{p.meta, {reinterpret_cast<const uint8_t*>(p.bytes.data()), p.bytes.size()}};
            packet::PacketFamilyRecord row;
            row.meta = p.meta;
            row.raw_data = {owner, view.bytes.data, p.meta.captured_len};
            Check(decoder.Decode(view, {7, packet::EndpointScope::kInnermost}, rows, &row) == 0, "local_decode");
            rows.rows.push_back(row);
        }
        std::shared_ptr<arrow::RecordBatch> batch;
        Check(packet::EncodePacketFamilyBatch(rows, &batch) == packet::PacketBatchError::kNone, "local_batch");
        event.block = {BlockPollEvent::kData, batch, 0};
        event.progress = poll.progress;
        for (auto& f : event.progress) f.generation = 900;
        return event;
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override {
        ++state_->released;
        return 0;
    }

 private:
    std::string name_;
    std::vector<PacketEnvelope> polls_;
    IProtocol* protocol_;
    std::shared_ptr<State> state_;
    size_t index_ = 0;
    std::atomic<bool> cancelled_{false};
};
class Session final : public IKafkaClientSessionV1, public IKafkaConsumerSessionV1 {
 public:
    KafkaBindingSnapshotV1 snapshot;
    std::vector<KafkaMessageV1> messages;
    size_t index = 0;
    std::atomic<bool> cancelled{false};
    std::shared_ptr<State> state;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return ENOTSUP; }
    void Cancel() override { cancelled = true; }
    int QueryInterface(const Guid&, void** p) override {
        *p = static_cast<IKafkaConsumerSessionV1*>(this);
        return 0;
    }
    int StartConsumer(const char*, uint32_t) override { return 0; }
    int PollMessage(uint32_t, KafkaMessageV1* m) override {
        if (cancelled) return ECANCELED;
        if (index == messages.size() || index >= state->permitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return EAGAIN;
        }
        *m = messages[index++];
        return 0;
    }
    int CommitOffsets(const std::vector<KafkaOffsetV1>&, uint32_t) override {
        ++state->released;
        return 0;
    }
};
class Factory final : public IBlockStreamFactory,
                      public IBlockStreamReaderFactoryV1,
                      public IBlockStreamChannelDescriptorV1,
                      public IKafkaChannelCatalogV1 {
 public:
    Local local{"local", {}, nullptr, std::make_shared<State>()},
        remote{"remote", {}, nullptr, std::make_shared<State>()};
    std::shared_ptr<State> state = std::make_shared<State>();
    int DescribeChannel(const char*, KafkaChannelDescriptionV1* d) override {
        *d = {};
        d->role = "source";
        d->source_adapter_available = true;
        return 0;
    }
    IBlockStreamChannel* Get(const char* type, const char* name) override {
        if (!type || std::string(type) != "kafka" || !name) return nullptr;
        return std::string(name) == "local" ? &local : std::string(name) == "remote" ? &remote : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> f) override {
        f("kafka", "local", &local);
        f("kafka", "remote", &remote);
    }
    void DescribeChannelTypes(std::function<void(const BlockStreamChannelTypeDescriptorV1&)> f) const override {
        BlockStreamChannelTypeDescriptorV1 d;
        d.channel_type = "kafka";
        d.allowed_roles = {"source"};
        f(d);
    }
    int CreateReader(const BlockStreamReaderConfigV1& c, IBlockStreamChannel** output) override {
        *output = nullptr;
        if (!Get(c.source_category, c.source_name)) return ENOTSUP;
        auto* protocol = static_cast<IProtocol*>(PluginLoader::Single()->First(IID_PROTOCOL));
        state->released = 0;
        state->permitted = 1;
        auto polls = Sequence();
        if (std::string(c.source_name) == "local")
            *output = new Local("local", polls, protocol, state);
        else {
            auto session = std::make_shared<Session>();
            session->state = state;
            session->snapshot.topic = "packets";
            for (const auto& p : polls) {
                auto m = EncodePacket(Config(), p).ValueOrDie();
                m.topic = "packets";
                m.offset = session->messages.size();
                session->messages.push_back(std::move(m));
            }
            *output = new PacketReader("remote", Config(), session, protocol);
        }
        return (*output)->Open();
    }
    void ReleaseReader(IBlockStreamChannel* reader) override { delete reader; }
};
std::multiset<std::string> Rows(Runtime& runtime, const std::string& entity, const std::string& run) {
    std::multiset<std::string> rows;
    auto batches = runtime.Query("SELECT * FROM npm_" + entity + "_history_v1 WHERE __npm_run_id='" + run + "'");
    for (const auto& batch : batches)
        for (int64_t r = 0; r < batch->num_rows(); ++r) {
            std::string row;
            for (int c = 0; c < batch->num_columns(); ++c) {
                const auto& name = batch->schema()->field(c)->name();
                if (name.rfind("__npm_", 0) == 0 || name.find("session_id") != std::string::npos ||
                    name == "entity_instance_id" || name == "observed_at")
                    continue;
                row += name + "=" + batch->column(c)->GetScalar(r).ValueOrDie()->ToString() + "|";
            }
            rows.insert(row);
        }
    return rows;
}
}  // namespace
int main() {
    try {
        Factory factory;
        Labels labels;
        auto* loader = PluginLoader::Single();
        loader->Regist(IID_CONFIG_CHANNEL_REGISTRY_V1, &labels);
        loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&factory));
        loader->Regist(IID_BLOCK_STREAM_READER_FACTORY_V1, static_cast<IBlockStreamReaderFactoryV1*>(&factory));
        loader->Regist(IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR_V1, static_cast<IBlockStreamChannelDescriptorV1*>(&factory));
        loader->Regist(IID_KAFKA_CHANNEL_CATALOG_V1, static_cast<IKafkaChannelCatalogV1*>(&factory));
        Runtime runtime(flowsql::test::WorkspacePath("packet-npm"));
        for (const auto& [setting, diagnostic] :
             {std::pair<std::string, std::string>{"run_mode='offline'", "run_mode conflicts"},
              {"source_domains='0:8;1:8'", "source domain conflicts"}}) {
            const auto id = "packet-conflict-" + diagnostic;
            runtime.Submit(id, "SELECT * FROM kafka.remote USING npm.basic WITH features='basic'," + setting +
                                   " INTO sqlite.results");
            const auto failed = runtime.Terminal(id);
            Check(Text(failed, "status") == "failed" &&
                      Text(failed, "error_message").find(diagnostic) != std::string::npos,
                  "source_contract_conflict_accepted");
        }
        int next = 0;
        auto execute = [&](const char* source, int seconds, bool filter, bool cancel) {
            const auto id = "packet-npm-" + std::to_string(++next);
            const auto sql = "SELECT * FROM kafka." + std::string(source) + (filter ? " WHERE wire_len < 0" : "") +
                             " USING npm.basic WITH features='basic,session,dns,http1,tls,icmp,labeling',"
                             "parameters='{\"schema_version\":1,\"core\":{\"labeling\":\"config.packet-test@1\","
                             "\"out_of_order_tolerance_ns\":0,\"tcp_idle_timeout_ns\":600000000000,"
                             "\"udp_idle_timeout_ns\":600000000000,\"output_interval_ns\":" +
                             std::to_string(int64_t(seconds) * 1000000000) +
                             "},"
                             "\"dns\":{\"primary_label_ids\":[1001]},\"http1\":{\"primary_label_ids\":[1001]},"
                             "\"tls\":{\"primary_label_ids\":[1001]}}' INTO sqlite.results";
            runtime.Submit(id, sql);
            const auto poll_count = Sequence().size();
            int64_t prior_rows = 0;
            for (size_t poll = 1; poll <= poll_count; ++poll) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (factory.state->released < poll && std::chrono::steady_clock::now() < deadline) {
                    const auto status = runtime.Status(id);
                    if (Text(status, "status") == "failed") {
                        std::cerr << Encode(status) << '\n';
                        throw std::runtime_error("npm_failed");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                Check(factory.state->released == poll, "source_not_drained");
                const auto counts = runtime.Query("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1");
                const auto count = std::stoll(counts.front()->column(0)->GetScalar(0).ValueOrDie()->ToString());
                if (poll == 3 || poll == 5 || poll == 7)
                    Check(count == prior_rows, "blocked_remote_time_closed_period");
                prior_rows = count;
                if (poll == 2 || poll == 4 || poll == 6 || poll == 8) {
                    const auto end = runtime.Query(
                        "SELECT MAX(period_end_ns) FROM npm_basic_ipv4_history_v1 WHERE period_complete=1 AND "
                        "__npm_run_id=(SELECT run_id FROM npm_result_runs ORDER BY started_at_ns DESC LIMIT 1)");
                    const auto value = end.front()->column(0)->GetScalar(0).ValueOrDie();
                    const int64_t safe_seconds[] = {0, 117, 0, 147, 0, 177, 0, 237};
                    if (value->is_valid)
                        Check(std::stoll(value->ToString()) <= safe_seconds[poll - 1] * 1000000000,
                              "period_exceeded_slowest_input");
                }
                factory.state->permitted = poll + 1;
            }
            runtime.Status(id, cancel ? "cancel" : "stop");
            auto final = runtime.Terminal(id);
            Check(Text(final, "status") == (cancel ? "cancelled" : "stopped"), "terminal_kind");
            Check(final.HasMember("managed_result"), "managed_result_missing");
            const auto run = Text(final["managed_result"], "run_id");
            const auto states = runtime.Query("SELECT status FROM npm_result_runs WHERE run_id='" + run + "'");
            Check(states.front()->column(0)->GetScalar(0).ValueOrDie()->ToString() ==
                      (cancel ? "incomplete" : "completed"),
                  "managed_terminal_state");
            if (!filter && !cancel) {
                const auto tail = runtime.Query("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" +
                                                run + "' AND is_final=1 AND period_complete=0");
                Check(std::stoll(tail.front()->column(0)->GetScalar(0).ValueOrDie()->ToString()) > 0,
                      "stop_missing_incomplete_tail");
            }
            return run;
        };
        for (int seconds : {15, 30, 60}) {
            factory.state->released = 0;
            const auto local = execute("local", seconds, false, false);
            factory.state->released = 0;
            const auto remote = execute("remote", seconds, false, false);
            Check(local != remote, "run_identity_reused");
            for (const auto* entity :
                 {"basic_ipv4", "basic_ipv6", "session_ipv4", "session_ipv6", "dns_transaction_ipv4",
                  "dns_transaction_ipv6", "http1_transaction_ipv4", "http1_transaction_ipv6", "tls_handshake_ipv4",
                  "tls_handshake_ipv6", "icmp_event_ipv4", "icmp_event_ipv6"}) {
                const auto expected = Rows(runtime, entity, local), actual = Rows(runtime, entity, remote);
                if (actual != expected)
                    std::cerr << "mismatch entity=" << entity << " period=" << seconds << " local=" << expected.size()
                              << " remote=" << actual.size() << '\n';
                Check(actual == expected, "npm_packet_local_mismatch");
                if (actual.empty()) std::cerr << "empty entity=" << entity << '\n';
                Check(!actual.empty(), "missing_entity_family");
            }
        }
        factory.state->released = 0;
        const auto filtered = execute("remote", 15, true, false);
        Check(Rows(runtime, "basic_ipv4", filtered).empty() && Rows(runtime, "basic_ipv6", filtered).empty(),
              "filtered_batch_created_session");
        factory.state->released = 0;
        execute("remote", 15, false, true);
        std::cout << "PASS production Scheduler/NPM managed SQLite, packet/local twelve-entity comparison, two "
                     "inputs/IP families, 15/30/60s, backlog/regression/idle, filter and stop/cancel\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
