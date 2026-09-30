// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/dns/npm_dns_result_encoder.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

namespace npm = flowsql::npm;

namespace {

class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == npm::NpmBudgetCategory::kModuleState);
        if (bytes > limit - used) return npm::NpmBudgetError::kTrackedLimitExceeded;
        used += bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetError Release(npm::NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == npm::NpmBudgetCategory::kModuleState && bytes <= used);
        used -= bytes;
        return npm::NpmBudgetError::kNone;
    }
    npm::NpmBudgetUsage Usage() const override { return {0, used, 0, 0}; }
    uint64_t limit = 65536;
    uint64_t used = 0;
};

class FailingPool final : public arrow::MemoryPool {
 public:
    arrow::Status Allocate(int64_t, int64_t, uint8_t**) override {
        return arrow::Status::OutOfMemory("injected DNS encoding allocation failure");
    }
    arrow::Status Reallocate(int64_t, int64_t, int64_t, uint8_t**) override {
        return arrow::Status::OutOfMemory("injected DNS encoding allocation failure");
    }
    void Free(uint8_t*, int64_t, int64_t) override {}
    int64_t bytes_allocated() const override { return 0; }
    int64_t max_memory() const override { return 0; }
    int64_t total_bytes_allocated() const override { return 0; }
    int64_t num_allocations() const override { return 0; }
    std::string backend_name() const override { return "dns_test_failure"; }
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    explicit Emitter(std::shared_ptr<Budget> budget) : budget_(std::move(budget)) {}
    int Emit(std::string_view entity, const arrow::RecordBatch& rows) override {
        ++calls;
        assert(entity == "dns_transaction" && budget_->used > 0);
        const auto descriptor = npm::NpmDnsTransactionEntityDescriptorV1();
        assert(npm::ValidateNpmEntityRowsV1("dns", descriptor, rows).error == npm::NpmProtocolContractErrorV1::kNone);
        assert(rows.num_rows() == 1 && rows.schema()->Equals(*descriptor.schema, true));
        auto id = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("entity_instance_id"));
        auto domain = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"));
        auto outcome = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"));
        auto response = std::static_pointer_cast<arrow::Int64Array>(rows.GetColumnByName("response_at_ns"));
        auto reason = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("incomplete_reason"));
        assert(id->Value(0) == 9 && domain->Value(0) == 44 && outcome->GetString(0) == "query_only");
        assert(response->IsNull(0) && reason->GetString(0) == "session_end");
        return error;
    }
    int error = 0;
    int calls = 0;

 private:
    std::shared_ptr<Budget> budget_;
};

void TestEncodeAndFailures() {
    auto budget = std::make_shared<Budget>();
    Emitter emitter(budget);
    npm::NpmDnsSessionIdentityV1 session;
    session.session_id = 7;
    session.observation_domain_id = 44;
    session.transport_protocol = 17;
    session.a_ip = "192.0.2.1";
    session.b_ip = "198.51.100.2";
    session.a_port = 53000;
    session.b_port = 53;
    npm::NpmDnsUdpResultV1 row;
    row.entity_instance_id = 9;
    row.session_id = 7;
    row.dns_id = 12;
    row.outcome = "query_only";
    row.incomplete_reason = "session_end";
    row.query_direction = npm::NpmPacketDirection::kAToB;
    row.qname = "a.example";
    row.qtype = 1;
    row.qclass = 1;
    row.query_at_ns = 100;
    row.observed_at_ns = 200;
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter) == 0);
    assert(emitter.calls == 1 && budget->used == 0);
    emitter.error = EIO;
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter) == EIO);
    assert(emitter.calls == 2 && budget->used == 0);
    budget->limit = 1;
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter) == ENOSPC);
    assert(emitter.calls == 2 && budget->used == 0);
    budget->limit = 65536;
    FailingPool pool;
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter, &pool) == ENOMEM);
    assert(emitter.calls == 2 && budget->used == 0);
    row.entity_instance_id = 0;
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter) == EINVAL);
    assert(emitter.calls == 2 && budget->used == 0);
    row.entity_instance_id = 9;
    row.qname = std::string(1025, 'x');
    assert(npm::EmitNpmDnsTransactionV1(row, session, budget, emitter) == EINVAL);
    assert(emitter.calls == 2 && budget->used == 0);
}

}  // namespace

int main() {
    TestEncodeAndFailures();
    std::puts("NPM DNS result encoder tests passed");
    return 0;
}
