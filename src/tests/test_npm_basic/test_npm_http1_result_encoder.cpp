// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/http1/npm_http1_result_encoder.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <memory>

namespace npm = flowsql::npm;

namespace {

class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory, uint64_t bytes) override {
        if (bytes > limit - used) return npm::NpmBudgetError::kTrackedLimitExceeded;
        used += bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetError Release(npm::NpmBudgetCategory, uint64_t bytes) override {
        assert(bytes <= used);
        used -= bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetUsage Usage() const override { return {0, used, 0, 0}; }
    uint64_t used = 0;
    uint64_t limit = 1024 * 1024;
};

class FailingPool final : public arrow::MemoryPool {
 public:
    arrow::Status Allocate(int64_t, int64_t, uint8_t**) override {
        return arrow::Status::OutOfMemory("injected HTTP/1 encoding failure");
    }
    arrow::Status Reallocate(int64_t, int64_t, int64_t, uint8_t**) override {
        return arrow::Status::OutOfMemory("injected HTTP/1 encoding failure");
    }
    void Free(uint8_t*, int64_t, int64_t) override {}
    int64_t bytes_allocated() const override { return 0; }
    int64_t max_memory() const override { return 0; }
    int64_t total_bytes_allocated() const override { return 0; }
    int64_t num_allocations() const override { return 0; }
    std::string backend_name() const override { return "http1_test_failure"; }
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    explicit Emitter(std::shared_ptr<Budget> budget) : budget_(std::move(budget)) {}
    int Emit(std::string_view entity, const arrow::RecordBatch& rows) override {
        ++calls;
        assert(entity == "http1_transaction" && budget_->used > 0);
        auto descriptor = npm::NpmHttp1TransactionEntityDescriptorV1();
        assert(npm::ValidateNpmEntityRowsV1("http1", descriptor, rows).error == npm::NpmProtocolContractErrorV1::kNone);
        assert(rows.schema()->Equals(*descriptor.schema, true) && rows.num_rows() == 1);
        auto domain = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"));
        auto outcome = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"));
        auto latency = std::static_pointer_cast<arrow::Int64Array>(rows.GetColumnByName("latency_ns"));
        assert(domain->Value(0) == 44 && outcome->GetString(0) == "matched" && latency->Value(0) == 10);
        return error;
    }
    int error = 0;
    int calls = 0;

 private:
    std::shared_ptr<Budget> budget_;
};

void TestEncoder() {
    auto budget = std::make_shared<Budget>();
    Emitter emitter(budget);
    npm::NpmHttp1SessionIdentityV1 session;
    session.session_id = 7;
    session.observation_domain_id = 44;
    session.a_ip = "192.0.2.1";
    session.b_ip = "198.51.100.2";
    session.a_port = 12345;
    session.b_port = 80;
    npm::NpmHttp1TransactionResultV1 row;
    row.entity_instance_id = 9;
    row.session_id = 7;
    row.outcome = npm::NpmHttp1OutcomeV1::kMatched;
    row.request_direction = npm::NpmPacketDirection::kAToB;
    row.response_direction = npm::NpmPacketDirection::kBToA;
    row.request.emplace();
    row.request->method = "GET";
    row.request->target = "/a";
    row.request->complete_at_ns = 10;
    row.response.emplace();
    row.response->is_response = true;
    row.response->status_code = 200;
    row.response->complete_at_ns = 20;
    row.latency_ns = 10;
    row.observed_at_ns = 20;
    assert(npm::EmitNpmHttp1TransactionV1(row, session, budget, emitter) == 0);
    assert(budget->used == 0 && emitter.calls == 1);
    emitter.error = EIO;
    assert(npm::EmitNpmHttp1TransactionV1(row, session, budget, emitter) == EIO);
    assert(budget->used == 0 && emitter.calls == 2);
    budget->limit = 1;
    assert(npm::EmitNpmHttp1TransactionV1(row, session, budget, emitter) == ENOSPC);
    assert(budget->used == 0 && emitter.calls == 2);
    budget->limit = 1024 * 1024;
    FailingPool pool;
    assert(npm::EmitNpmHttp1TransactionV1(row, session, budget, emitter, &pool) == ENOMEM);
    assert(budget->used == 0 && emitter.calls == 2);
    row.request->target = std::string("\xff", 1);
    assert(npm::EmitNpmHttp1TransactionV1(row, session, budget, emitter) == EINVAL);
    assert(budget->used == 0 && emitter.calls == 2);
}

}  // namespace

int main() { TestEncoder(); }
