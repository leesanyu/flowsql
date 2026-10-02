// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_result_encoder.h>

#include <arrow/api.h>

#include <cassert>
#include <cerrno>
#include <memory>

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
    uint64_t used = 0;
    uint64_t limit = 1024 * 1024;
};

class FailingPool final : public arrow::MemoryPool {
 public:
    arrow::Status Allocate(int64_t, int64_t, uint8_t**) override { return arrow::Status::OutOfMemory("tls test"); }
    arrow::Status Reallocate(int64_t, int64_t, int64_t, uint8_t**) override {
        return arrow::Status::OutOfMemory("tls test");
    }
    void Free(uint8_t*, int64_t, int64_t) override {}
    int64_t bytes_allocated() const override { return 0; }
    int64_t max_memory() const override { return 0; }
    int64_t total_bytes_allocated() const override { return 0; }
    int64_t num_allocations() const override { return 0; }
    std::string backend_name() const override { return "tls_test_failure"; }
};

class Emitter final : public npm::INpmResultEmitterV1 {
 public:
    explicit Emitter(std::shared_ptr<Budget> budget) : budget_(std::move(budget)) {}
    int Emit(std::string_view entity, const arrow::RecordBatch& rows) override {
        ++calls;
        assert(entity == "tls_handshake" && budget_->used > 0);
        const auto descriptor = npm::NpmTlsHandshakeEntityDescriptorV1();
        assert(npm::ValidateNpmEntityRowsV1("tls", descriptor, rows).error == npm::NpmProtocolContractErrorV1::kNone);
        assert(rows.schema()->Equals(*descriptor.schema, true) && rows.num_rows() == 1);
        auto domain = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"));
        auto outcome = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"));
        auto reason = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("incomplete_reason"));
        auto alpn = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("client_alpn_protocols"));
        auto version = std::static_pointer_cast<arrow::UInt16Array>(rows.GetColumnByName("selected_version"));
        auto selected_alpn = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("selected_alpn"));
        assert(domain->Value(0) == 44 && outcome->GetString(0) == "server_hello_observed");
        assert(reason->GetString(0) == expected_reason);
        assert(alpn->GetString(0) == "[\"h2\",\"http/1.1\"]");
        assert(version->Value(0) == expected_version);
        if (expected_selected_alpn) {
            assert(selected_alpn->GetString(0) == *expected_selected_alpn);
        } else {
            assert(selected_alpn->IsNull(0));
        }
        return error;
    }
    int calls = 0;
    int error = 0;
    uint16_t expected_version = 0x0304;
    std::string expected_reason = "encrypted_after_server_hello";
    std::optional<std::string> expected_selected_alpn;

 private:
    std::shared_ptr<Budget> budget_;
};

void TestEncoder() {
    auto budget = std::make_shared<Budget>();
    Emitter emitter(budget);
    npm::NpmTlsSessionIdentityV1 session{7, 44, "192.0.2.1", "198.51.100.2", 12345, 443};
    npm::NpmTlsHandshakeResultV1 row;
    row.handshake.entity_instance_id = 9;
    row.handshake.session_id = 7;
    row.handshake.phase = npm::NpmTlsHandshakePhaseV1::kClosed;
    row.handshake.first_client_hello.emplace();
    row.handshake.first_client_hello->offered_versions = {0x0304};
    row.handshake.first_client_hello->offered_alpn = {"h2", "http/1.1"};
    row.handshake.first_client_hello->sni = "example.com";
    row.handshake.first_client_hello->complete_at_ns = 10;
    row.handshake.final_server_hello.emplace();
    row.handshake.final_server_hello->selected_version = 0x0304;
    row.handshake.final_server_hello->cipher_suite = 0x1301;
    row.handshake.final_server_hello->complete_at_ns = 20;
    row.outcome = npm::NpmTlsOutcomeV1::kServerHelloObserved;
    row.incomplete_reason = npm::NpmTlsIncompleteReasonV1::kEncryptedAfterServerHello;
    row.observed_at_ns = 20;
    row.server_hello_latency_ns = 10;
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter) == 0);
    assert(budget->used == 0 && emitter.calls == 1);
    emitter.error = EIO;
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter) == EIO);
    assert(budget->used == 0 && emitter.calls == 2);
    budget->limit = 1;
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter) == ENOSPC);
    assert(budget->used == 0 && emitter.calls == 2);
    budget->limit = 1024 * 1024;
    FailingPool pool;
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter, &pool) == ENOMEM);
    assert(budget->used == 0 && emitter.calls == 2);
    row.handshake.final_server_hello->selected_version = 0x0303;
    row.handshake.final_server_hello->selected_alpn = std::string("h\0\xff\"", 4);
    row.incomplete_reason = npm::NpmTlsIncompleteReasonV1::kSessionEndAfterServerHello;
    emitter.error = 0;
    emitter.expected_version = 0x0303;
    emitter.expected_reason = "session_end_after_server_hello";
    emitter.expected_selected_alpn = "\"h\\u0000\\u00ff\\\"\"";
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter) == 0);
    assert(budget->used == 0 && emitter.calls == 3);
    row.handshake.entity_instance_id = 0;
    assert(npm::EmitNpmTlsHandshakeV1(row, session, budget, emitter) == EINVAL);
}

}  // namespace

int main() { TestEncoder(); }
