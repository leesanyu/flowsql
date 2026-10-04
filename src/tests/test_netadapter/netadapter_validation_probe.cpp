// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

// Test-only operator capability: the production analyzer and SQLite still execute every operation.
#include <common/iplugin.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/icapture_backend.h>
#include <framework/interfaces/idatabase_channel.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <dlfcn.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace flowsql;
namespace {
using Clock = std::chrono::steady_clock;
int64_t MonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
struct Injection {
    std::string profile;
    Clock::time_point started = Clock::now();
    bool processing_started = false;
    uint64_t analysis_sleeps = 0, output_sleeps = 0;
    double Elapsed() const { return std::chrono::duration<double>(Clock::now() - started).count(); }
    void Analysis() {
        if (!processing_started) {
            processing_started = true;
            started = Clock::now();
        }
        const double elapsed = Elapsed();
        if ((profile == "slow_analysis" && elapsed >= 5 && elapsed < 35) ||
            (profile == "overload_recovery" && elapsed >= 5 && elapsed < 10)) {
            ++analysis_sleeps;
            std::this_thread::sleep_for(std::chrono::milliseconds(profile == "slow_analysis" ? 20 : 50));
        }
    }
    void Output() {
        if (profile == "slow_output" && processing_started && Elapsed() < 35) {
            ++output_sleeps;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
class DatabaseProbe final : public IDatabaseChannel, public IDatabasePreparedCommandV1 {
 public:
    DatabaseProbe(IDatabaseChannel* database, Injection* injection) : database_(database), injection_(injection) {
        prepared_ = dynamic_cast<IDatabasePreparedCommandV1*>(database);
        assert(prepared_);
    }
    const char* Category() override { return database_->Category(); }
    const char* Name() override { return database_->Name(); }
    const char* Type() override { return database_->Type(); }
    const char* Schema() override { return database_->Schema(); }
    int Open() override { return database_->Open(); }
    int Close() override { return database_->Close(); }
    bool IsOpened() const override { return database_->IsOpened(); }
    int Flush() override { return database_->Flush(); }
    int CreateReader(const char* sql, IBatchReader** reader) override { return database_->CreateReader(sql, reader); }
    int CreateWriter(const char* table, IBatchWriter** writer) override {
        return database_->CreateWriter(table, writer);
    }
    int CreateArrowReader(const char* sql, IArrowReader** reader) override {
        return database_->CreateArrowReader(sql, reader);
    }
    int CreateArrowWriter(const char* table, IArrowWriter** writer) override {
        return database_->CreateArrowWriter(table, writer);
    }
    int ExecuteQueryArrow(const char* sql, std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) override {
        return database_->ExecuteQueryArrow(sql, batches);
    }
    int WriteArrowBatches(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override {
        return database_->WriteArrowBatches(table, batches);
    }
    int ExecuteSql(const char* sql) override { return database_->ExecuteSql(sql); }
    const char* GetLastError() override { return database_->GetLastError(); }
    bool IsConnected() override { return database_->IsConnected(); }
    int ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t count) override {
        injection_->Output();
        return prepared_->ExecutePrepared(sql, parameters, count);
    }
    int ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters, size_t per_execution,
                             size_t executions) override {
        injection_->Output();
        return prepared_->ExecutePreparedBatch(sql, parameters, per_execution, executions);
    }

 private:
    IDatabaseChannel* database_;
    IDatabasePreparedCommandV1* prepared_;
    Injection* injection_;
};
class TaskProbe final : public IBlockTransformTaskV2,
                        public IBlockTransformCaptureFactTaskV2,
                        public IBlockTransformManagedSinkTaskV1,
                        public IBlockTransformInputSourceTaskV1 {
 public:
    TaskProbe(IBlockTransformOperatorV2* provider, IBlockTransformTaskV2* task) : provider_(provider), task_(task) {
        const char* profile = std::getenv("FLOWSQL_NETADAPTER_TEST_PROFILE");
        injection_.profile = profile ? profile : "dual";
    }
    ~TaskProbe() override { provider_->ReleaseTask(task_); }
    int Open(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::Schema>* output) override {
        return task_->Open(std::move(schema), output);
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t ts,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        injection_.Analysis();
        return task_->ProcessBlock(input, ts, outputs);
    }
    int Flush(std::vector<BlockTransformOutputV1>* outputs) override { return task_->Flush(outputs); }
    void Cancel() override { task_->Cancel(); }
    std::string LastError() const override { return task_->LastError(); }
    int BindInputSource(const char* source) override {
        return dynamic_cast<IBlockTransformInputSourceTaskV1*>(task_)->BindInputSource(source);
    }
    int BindManagedSink(const BlockTransformManagedSinkBindingV1& binding) override {
        database_ = std::make_unique<DatabaseProbe>(dynamic_cast<IDatabaseChannel*>(binding.sink_channel), &injection_);
        auto forwarded = binding;
        forwarded.sink_channel = database_.get();
        return dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task_)->BindManagedSink(forwarded);
    }
    int BindCaptureSources(const CaptureSourceSetV2& sources) override {
        input_last_service_.resize(sources.inputs.size());
        input_service_max_ms_.resize(sources.inputs.size());
        return dynamic_cast<IBlockTransformCaptureFactTaskV2*>(task_)->BindCaptureSources(sources);
    }
    int AcceptCaptureFacts(const std::vector<CaptureProgressV1>& facts) override {
        const int result = dynamic_cast<IBlockTransformCaptureFactTaskV2*>(task_)->AcceptCaptureFacts(facts);
        if (!result && !facts.empty()) {
            const auto now = MonotonicNs();
            if (!pending_since_ns_) pending_since_ns_ = now;
            for (const auto& fact : facts) {
                auto& last = input_last_service_.at(fact.source_id);
                if (last)
                    input_service_max_ms_.at(fact.source_id) =
                        std::max(input_service_max_ms_.at(fact.source_id), double(now - last) / 1e6);
                last = now;
            }
        }
        return result;
    }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) override {
        const int result = task_->GetTimeDriveState(state);
        if (!result && state->armed) deadline_ns_ = state->deadline_ns;
        return result;
    }
    int OnTime(const BlockTransformTimeEventV1& event, std::vector<BlockTransformOutputV1>* outputs) override {
        const auto deadline = deadline_ns_ > 0 ? deadline_ns_ : pending_since_ns_;
        if (deadline > 0) lateness_ms_.push_back(double(std::max<int64_t>(0, event.monotonic_now_ns - deadline)) / 1e6);
        if (last_time_ns_ > 0) service_gap_ms_.push_back(double(event.monotonic_now_ns - last_time_ns_) / 1e6);
        last_time_ns_ = event.monotonic_now_ns;
        pending_since_ns_ = 0;
        return task_->OnTime(event, outputs);
    }
    std::string ManagedSinkResultJson() const override {
        auto json = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task_)->ManagedSinkResultJson();
        rapidjson::Document document;
        document.Parse(json.c_str());
        if (!document.IsObject()) return json;
        auto& allocator = document.GetAllocator();
        rapidjson::Value probe(rapidjson::kObjectType);
        probe.AddMember("analysis_sleeps", injection_.analysis_sleeps, allocator);
        probe.AddMember("output_sleeps", injection_.output_sleeps, allocator);
        probe.AddMember("analysis_delay_ms", injection_.profile == "slow_analysis" ? 20 : 50, allocator);
        probe.AddMember("output_delay_ms", 10, allocator);
        const auto distribution = [&](const char* name, std::vector<double> values) {
            rapidjson::Value value(rapidjson::kObjectType);
            value.AddMember("count", uint64_t(values.size()), allocator);
            if (!values.empty()) {
                std::sort(values.begin(), values.end());
                value.AddMember("p50", values[values.size() / 2], allocator);
                value.AddMember("p99", values[std::min(values.size() - 1, values.size() * 99 / 100)], allocator);
                value.AddMember("max", values.back(), allocator);
            }
            probe.AddMember(rapidjson::Value(name, allocator), value, allocator);
        };
        distribution("maintenance_lateness_ms", lateness_ms_);
        distribution("maintenance_service_gap_ms", service_gap_ms_);
        rapidjson::Value input_services(rapidjson::kArrayType);
        for (auto value : input_service_max_ms_) input_services.PushBack(value, allocator);
        probe.AddMember("per_input_service_max_ms", input_services, allocator);
        document.AddMember("validation_probe", probe, allocator);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
        return buffer.GetString();
    }

 private:
    IBlockTransformOperatorV2* provider_;
    IBlockTransformTaskV2* task_;
    Injection injection_;
    std::unique_ptr<DatabaseProbe> database_;
    int64_t pending_since_ns_ = 0, deadline_ns_ = 0, last_time_ns_ = 0;
    std::vector<double> lateness_ms_, service_gap_ms_;
    std::vector<int64_t> input_last_service_;
    std::vector<double> input_service_max_ms_;
};
class OperatorProbe final : public IBlockTransformOperatorV2 {
 public:
    explicit OperatorProbe(IQuerier* querier) {
        library_ = dlopen(FLOWSQL_REAL_NPM_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
        assert(library_);
        auto create =
            reinterpret_cast<void* (*)(int, IQuerier*)>(dlsym(library_, "flowsql_create_operator_capability"));
        destroy_ = reinterpret_cast<void (*)(int, void*)>(dlsym(library_, "flowsql_destroy_operator_capability"));
        assert(create && destroy_);
        delegate_ = static_cast<IBlockTransformOperatorV2*>(create(1, querier));
        assert(delegate_);
    }
    ~OperatorProbe() override {
        destroy_(1, delegate_);
        dlclose(library_);
    }
    std::string Category() const override { return "npm"; }
    std::string Name() const override { return "basic"; }
    std::string Description() const override { return "Test probe around production npm.basic"; }
    int CreateTask(const BlockTransformTaskConfigV2& config, IBlockTransformTaskV2** output) override {
        IBlockTransformTaskV2* task = nullptr;
        const int result = delegate_->CreateTask(config, &task);
        if (!result) *output = new TaskProbe(delegate_, task);
        return result;
    }
    void ReleaseTask(IBlockTransformTaskV2* task) override { delete task; }

 private:
    void* library_ = nullptr;
    void (*destroy_)(int, void*) = nullptr;
    IBlockTransformOperatorV2* delegate_ = nullptr;
};

class FailingInput final : public ICaptureBackendSessionV1 {
 public:
    explicit FailingInput(std::shared_ptr<ICaptureBackendSessionV1> delegate) : delegate_(std::move(delegate)) {}
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return delegate_->Inputs(); }
    int TryRead(uint32_t input, CapturePacketViewV1* packet) override {
        if (input == 1 && Clock::now() - started_ >= std::chrono::seconds(3)) {
            std::cerr << "validation injected second-input EIO\n";
            return EIO;
        }
        return delegate_->TryRead(input, packet);
    }
    void ReleasePacket(uint32_t input) override { delegate_->ReleasePacket(input); }
    CaptureBacklogV1 Backlog(uint32_t input) const override { return delegate_->Backlog(input); }
    int64_t IdleTimeNs(uint32_t input) const override { return delegate_->IdleTimeNs(input); }
    int ReadCounters(uint32_t input, CaptureCountersV1* counters) override {
        return delegate_->ReadCounters(input, counters);
    }
    void Cancel() override { delegate_->Cancel(); }

 private:
    std::shared_ptr<ICaptureBackendSessionV1> delegate_;
    Clock::time_point started_ = Clock::now();
};
class InputFailureProvider final : public IPlugin, public ICaptureBackendProviderV1 {
 public:
    int Option(const char*) override {
        const char* profile = std::getenv("FLOWSQL_NETADAPTER_TEST_PROFILE");
        const char* backend = std::getenv("FLOWSQL_NETADAPTER_TEST_BACKEND");
        backend_ = profile && std::string(profile) == "input_failure" && backend ? backend : "unused_test_provider";
        return 0;
    }
    int Load(IQuerier* querier) override {
        querier_ = querier;
        return 0;
    }
    int Unload() override {
        querier_ = nullptr;
        return 0;
    }
    const char* Backend() const override { return backend_.c_str(); }
    int Open(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* output,
             std::string* error) override {
        ICaptureBackendProviderV1* actual = nullptr;
        querier_->Traverse(IID_CAPTURE_BACKEND_PROVIDER_V1, [&](void* value) {
            auto* provider = static_cast<ICaptureBackendProviderV1*>(value);
            if (provider != this && config.backend == provider->Backend()) {
                actual = provider;
                return -1;
            }
            return 0;
        });
        assert(actual);
        std::shared_ptr<ICaptureBackendSessionV1> session;
        const int result = actual->Open(config, &session, error);
        if (!result) {
            std::cerr << "validation wrapped real " << config.backend << " inputs=" << session->Inputs().size() << "\n";
            *output = std::make_shared<FailingInput>(std::move(session));
        }
        return result;
    }

 private:
    IQuerier* querier_ = nullptr;
    std::string backend_;
};
}  // namespace

BEGIN_PLUGIN_REGIST(InputFailureProvider)
____INTERFACE(IID_PLUGIN, IPlugin)
____INTERFACE(IID_CAPTURE_BACKEND_PROVIDER_V1, ICaptureBackendProviderV1)
END_PLUGIN_REGIST()
extern "C" int flowsql_abi_version() { return kCppOperatorPluginAbiVersionV2; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" int flowsql_describe_operator(int index, CppOperatorDescriptorV2* descriptor) {
    if (index || !descriptor || descriptor->struct_size < kCppOperatorDescriptorV2Size) return EINVAL;
    *descriptor = {};
    descriptor->struct_size = kCppOperatorDescriptorV2Size;
    descriptor->category = "npm";
    descriptor->name = "basic";
    descriptor->description = "Test probe around production npm.basic";
    descriptor->contract_iid = IID_BLOCK_TRANSFORM_OPERATOR_V2;
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, IQuerier* querier) {
    return !index && querier ? static_cast<IBlockTransformOperatorV2*>(new OperatorProbe(querier)) : nullptr;
}
extern "C" void flowsql_destroy_operator_capability(int index, void* capability) {
    if (!index) delete static_cast<IBlockTransformOperatorV2*>(capability);
}
