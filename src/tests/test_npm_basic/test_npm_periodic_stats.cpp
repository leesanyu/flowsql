// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <arrow/api.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/core/npm_protocol_context.h>
#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/modules/basic/npm_basic_periodic_stats.h>
#include <atomic>
#include <cassert>
#include <thread>
using namespace flowsql;
using namespace flowsql::npm;
class ContextDictionary final : public flowsql::protocol::IDictionary {
 public:
    int32_t Count() const override { return 2; }

    const flowsql::protocol::Entry* Query(int32_t number) const override {
        if (number == main_.number) return &main_;
        if (number == sub_.number) return &sub_;
        return &unknown_;
    }

    int32_t Traverse(std::function<int32_t(const flowsql::protocol::Entry*)> traverser) const override {
        if (!traverser) return 0;
        if (traverser(&main_) != 0) return 1;
        traverser(&sub_);
        return 2;
    }

 private:
    const flowsql::protocol::Entry main_{7, 7, "MAIN", "MAIN", "MAIN"};
    const flowsql::protocol::Entry sub_{8, 7, "SUB", "SUB", "SUB"};
    const flowsql::protocol::Entry unknown_{0, 0, "UNKNOWN", "UNKNOWN", "UNKNOWN"};
};

class ContextProtocol final : public flowsql::IProtocol {
 public:
    explicit ContextProtocol(flowsql::protocol::IDictionary* dictionary) : dictionary_(dictionary) {}

    void Concurrency(int32_t) override {}

    flowsql::protocol::Protocol Identify(int32_t pipeno, const uint8_t*, int32_t,
                                         const flowsql::protocol::Layers*) override {
        if (identify_entered != nullptr) identify_entered->store(true, std::memory_order_release);
        while (release_identify != nullptr && !release_identify->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        identify_pipelines.push_back(pipeno);
        return {7, 8};
    }

    int32_t Layer(int32_t, const uint8_t*, int32_t, flowsql::protocol::Layers*) override {
        ++layer_calls;
        return 0;
    }

    flowsql::protocol::IDictionary* Dictionary() override { return dictionary_; }

    int32_t layer_calls = 0;
    std::vector<int32_t> identify_pipelines;
    std::atomic<bool>* identify_entered = nullptr;
    std::atomic<bool>* release_identify = nullptr;

 private:
    flowsql::protocol::IDictionary* dictionary_ = nullptr;
};

class ContextPool final : public flowsql::IProtocolPipelinePoolV1 {
 public:
    explicit ContextPool(flowsql::IProtocol* protocol,
                         flowsql::ProtocolPipelinePoolError acquire_result = flowsql::ProtocolPipelinePoolError::kNone)
        : protocol_(protocol), acquire_result_(acquire_result) {}

    flowsql::ProtocolPipelinePoolError Acquire(int32_t* pipeno) override {
        ++acquire_calls;
        if (pipeno == nullptr) return flowsql::ProtocolPipelinePoolError::kNullOutput;
        if (acquire_result_ != flowsql::ProtocolPipelinePoolError::kNone) return acquire_result_;
        if (leased_) return flowsql::ProtocolPipelinePoolError::kExhausted;
        leased_ = true;
        *pipeno = 3;
        return flowsql::ProtocolPipelinePoolError::kNone;
    }

    flowsql::ProtocolPipelinePoolError Release(int32_t pipeno) override {
        ++release_calls;
        if (pipeno != 3) return flowsql::ProtocolPipelinePoolError::kInvalidPipeline;
        if (!leased_) return flowsql::ProtocolPipelinePoolError::kNotLeased;
        leased_ = false;
        return flowsql::ProtocolPipelinePoolError::kNone;
    }

    int32_t Capacity() const override { return 4; }
    flowsql::IProtocol* Protocol() override { return protocol_; }

    int32_t acquire_calls = 0;
    int32_t release_calls = 0;

 private:
    flowsql::IProtocol* protocol_ = nullptr;
    flowsql::ProtocolPipelinePoolError acquire_result_;
    bool leased_ = false;
};

class Querier final : public IQuerier {
 public:
    IProtocolPipelinePoolV1* pool;
    explicit Querier(IProtocolPipelinePoolV1* p) : pool(p) {}
    void* First(const Guid& id) override {
        return !(id < IID_PROTOCOL_PIPELINE_POOL_V1) && !(IID_PROTOCOL_PIPELINE_POOL_V1 < id) ? pool : nullptr;
    }
    int Traverse(const Guid& id, fntraverse fn) override {
        auto p = First(id);
        return p && fn ? fn(p) : 0;
    }
};
class Writer final : public INpmResultWriter {
 public:
    std::vector<NpmBasicResult> rows;
    int WriteBasic(const NpmBasicResult& row) override {
        assert(ValidateNpmBasicResult(row) == NpmBasicResultError::kNone);
        rows.push_back(row);
        return 0;
    }
    int WriteSession(const NpmSessionResult&) override { return EIO; }
};
int main() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    Querier querier(&pool);
    std::unique_ptr<NpmProtocolContext> context;
    assert(NpmProtocolContext::Create(&querier, &context) == NpmProtocolContextError::kNone);
    NpmBasicResultProjector projector(*context);
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.output_interval_ns = 10000000000LL;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionKey key;
    key.input_namespace = "pcapfile.test";
    key.ip_family = packet::AddressFamily::kIPv4;
    key.transport_protocol = 17;
    key.a.ip = packet::IPv4Address(htonl(0xc0000201));
    key.b.ip = packet::IPv4Address(htonl(0xc0000202));
    NpmSessionView view;
    view.key = &key;
    view.session_id = 1;
    view.protocol_status = NpmProtocolStatus::kUnknown;
    Writer writer;
    {
        NpmBasicPeriodicStats stats(config, budget, projector);
        const auto feed = [&](int64_t time, NpmPacketDirection direction, uint32_t bytes) {
            NpmPacketView packet;
            packet.packet.meta.timestamp_ns = time;
            packet.packet.meta.wire_len = bytes;
            packet.direction = direction;
            view.last_ns = std::max(view.last_ns, time);
            assert(stats.OnPacket(packet, view, writer) == 0);
        };
        feed(1000000000LL, NpmPacketDirection::kAToB, 100);
        feed(10000000000LL, NpmPacketDirection::kBToA, 200);
        assert(stats.OnTime(10000000000LL, 1, writer) == 0 && writer.rows.size() == 1);
        assert(writer.rows[0].wire_bytes_total == 100 && writer.rows[0].period->interval_wire_bytes_total == 100);
        feed(11000000000LL, NpmPacketDirection::kAToB, 50);
        assert(stats.OnTime(30000000000LL, 2, writer) == 0 && writer.rows.size() == 3);
        assert(writer.rows[1].wire_bytes_total == 350 && writer.rows[1].period->interval_wire_bytes_total == 250);
        assert(writer.rows[2].period->interval_wire_bytes_total == 0);
        assert(stats.OnSessionEnd(view, NpmSessionEndReason::kEof, 3, writer) == 0);
        assert(stats.OnFinish(3, writer) == 0 && writer.rows.size() == 4);
        assert(writer.rows.back().is_final && writer.rows.back().revision == 4);
        uint64_t total = 0;
        for (const auto& row : writer.rows) total += row.period->interval_wire_bytes_total;
        assert(total == writer.rows.back().wire_bytes_total && total == 350);
    }
    assert(budget->Usage().module_state_bytes == 0);
    for (auto reason : {NpmSessionEndReason::kClosed, NpmSessionEndReason::kIdleTimeout,
                        NpmSessionEndReason::kTupleReuse, NpmSessionEndReason::kEof}) {
        Writer result;
        view.session_id++;
        view.first_ns = 1000000000LL;
        view.last_ns = 23000000000LL;
        NpmBasicPeriodicStats stats(config, budget, projector);
        NpmPacketView packet;
        packet.packet.meta.wire_len = 120;
        packet.packet.meta.timestamp_ns = 23000000000LL;
        assert(stats.OnPacket(packet, view, result) == 0);
        packet.direction = NpmPacketDirection::kBToA;
        packet.packet.meta.timestamp_ns = 1000000000LL;
        assert(stats.OnPacket(packet, view, result) == 0);
        assert(stats.OnTime(20000000000LL, 0, result) == 0);
        assert(result.rows.size() == 2 && result.rows[0].wire_bytes_total == 120);
        assert(result.rows[0].first_ns == 1000000000LL && result.rows[0].last_ns == 1000000000LL);
        assert(result.rows[1].period->interval_wire_bytes_total == 0);
        assert(stats.OnSessionEnd(view, reason, 24000000000LL, result) == 0);
        assert(stats.OnFinish(0, result) == 0);
        assert(result.rows.back().is_final && result.rows.back().end_reason == reason);
        assert(result.rows.back().wire_bytes_total == 240);
        assert(result.rows.back().period->period_end_ns - result.rows.back().period->period_start_ns ==
               config.output_interval_ns);
        uint64_t total = 0;
        for (const auto& row : result.rows) total += row.period->interval_wire_bytes_total;
        assert(total == 240);
        NpmPacketView late;
        late.packet.meta.timestamp_ns = 1000000000LL;
        assert(stats.OnPacket(late, view, result) != 0);
        assert(stats.LastError().find("closed_boundary_ns") != std::string::npos);
    }
    assert(budget->Usage().module_state_bytes == 0);
    {
        Writer empty;
        NpmBasicPeriodicStats stats(config, budget, projector);
        assert(stats.OnTime(10000000000LL, 0, empty) == 0 && empty.rows.empty());
        assert(stats.OnFinish(0, empty) == 0 && empty.rows.empty());
    }
    {
        auto limited = config;
        limited.max_tracked_bytes = 1;
        auto constrained = std::make_shared<NpmTaskBudget>(limited);
        Writer result;
        NpmPacketView packet;
        packet.packet.meta.timestamp_ns = 1;
        NpmBasicPeriodicStats stats(config, constrained, projector);
        assert(stats.OnPacket(packet, view, result) != 0);
        assert(stats.LastError().find("budget") != std::string::npos);
        assert(constrained->Usage().module_state_bytes == 0);
    }
}
