// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <operators/npm_basic/output/npm_basic_result_collector.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace {
bool count_new = false;
size_t new_calls = 0;
size_t fail_new_at = 0;
using namespace flowsql::npm;
using flowsql::packet::AddressFamily;

class Budget final : public INpmTaskBudget {
 public:
    uint64_t limit = UINT64_MAX;
    uint64_t peak = 0;
    size_t reserves = 0;
    size_t reject_at = 0;
    NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == NpmBudgetCategory::kPendingOutput);
        ++reserves;
        if ((reject_at && reserves == reject_at) || bytes > limit - usage_.pending_output_bytes)
            return NpmBudgetError::kPendingOutputLimitExceeded;
        usage_.pending_output_bytes += bytes;
        peak = std::max(peak, usage_.pending_output_bytes);
        return NpmBudgetError::kNone;
    }
    NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) override {
        assert(category == NpmBudgetCategory::kPendingOutput && bytes <= usage_.pending_output_bytes);
        usage_.pending_output_bytes -= bytes;
        return NpmBudgetError::kNone;
    }
    NpmBudgetUsage Usage() const override { return usage_; }

 private:
    NpmBudgetUsage usage_;
};

struct Trace {
    std::map<std::string, std::vector<uint64_t>> ids;
    std::map<std::string, std::vector<std::string>> protocols;
    std::map<std::string, std::vector<int64_t>> batches;
    int attempts = 0;
    int fail_at = 0;
    int finishes = 0;
    int finish_error = 0;
    int cancels = 0;
    std::function<void()> consume;
};
class Consumer final : public INpmResultConsumerV1 {
 public:
    explicit Consumer(Trace& trace) : trace_(trace) {}
    int Consume(const NpmResultContextV1&, const NpmEntityDescriptorV1& entity,
                const arrow::RecordBatch& rows) override {
        ++trace_.attempts;
        if (trace_.consume) trace_.consume();
        if (trace_.attempts == trace_.fail_at) return EIO;
        trace_.batches[entity.entity_id].push_back(rows.num_rows());
        const auto ids = std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("session_id"));
        const auto protocol = std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("protocol"));
        for (int64_t row = 0; row < rows.num_rows(); ++row) {
            trace_.ids[entity.entity_id].push_back(ids->Value(row));
            trace_.protocols[entity.entity_id].push_back(protocol->IsNull(row) ? "" : protocol->GetString(row));
        }
        return 0;
    }
    int Finish() override {
        ++trace_.finishes;
        return trace_.finish_error;
    }
    void Cancel() noexcept override { ++trace_.cancels; }

 private:
    Trace& trace_;
};

template <typename Result>
Result Row(uint64_t id, size_t protocol_bytes = 0) {
    Result row;
    row.session_id = id;
    row.observation_domain_id = 77;
    row.revision = id;
    row.observed_at = 1000 + id;
    row.transport_protocol = 17;
    row.first_ns = 10;
    row.last_ns = 10;
    row.protocol_status = NpmProtocolStatus::kUnknown;
    if (protocol_bytes) {
        row.protocol_status = NpmProtocolStatus::kIdentified;
        row.protocol_id = 7;
        row.protocol = std::string(protocol_bytes, 'p');
    }
    return row;
}

struct Fixture {
    Trace trace;
    std::shared_ptr<Budget> budget = std::make_shared<Budget>();
    std::shared_ptr<NpmResultRouter> router;
    std::unique_ptr<NpmBasicResultCollector> collector;
    Fixture(std::string observing, bool consuming) {
        NpmBasicFeatureConfig features;
        features.basic_enabled = true;
        features.session_enabled = true;
        features.observing_entity = observing;
        std::vector<NpmEntityDescriptorV1> entities;
        for (auto family : {AddressFamily::kIPv4, AddressFamily::kIPv6}) {
            entities.push_back(NpmBasicEntityDescriptorV1(family));
            entities.push_back(NpmSessionEntityDescriptorV1(family));
        }
        std::unique_ptr<INpmResultConsumerV1> consumer;
        if (consuming) consumer = std::make_unique<Consumer>(trace);
        router = std::make_shared<NpmResultRouter>(std::move(entities), std::move(observing),
                                                   NpmResultContextV1{"task", "run"}, budget, std::move(consumer),
                                                   [](const std::function<int()>& callback) { return callback(); });
        collector = std::make_unique<NpmBasicResultCollector>(features, budget, router);
    }
    void Four(uint64_t id, size_t bytes = 0) {
        assert(collector->WriteBasic(Row<NpmBasicResult4>(id, bytes)) == 0);
        assert(collector->WriteBasic(Row<NpmBasicResult6>(id, bytes)) == 0);
        assert(collector->WriteSession(Row<NpmSessionResult4>(id, bytes)) == 0);
        assert(collector->WriteSession(Row<NpmSessionResult6>(id, bytes)) == 0);
    }
    std::shared_ptr<arrow::RecordBatch> Drain() {
        assert(collector->FlushAccepted() == 0);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(router->Drain(&output) == 0);
        return output;
    }
};

void TestBatchLimitsAndOutlets() {
    for (const bool observing : {false, true}) {
        for (const bool consuming : {false, true}) {
            Fixture f(observing ? "basic_ipv4" : "", consuming);
            static_assert(std::is_const_v<std::remove_reference_t<decltype(f.router->DeliveryPlan())>>);
            for (const auto& entry : f.router->DeliveryPlan().entities) {
                assert(entry.observing == (observing && entry.descriptor.entity_id == "basic_ipv4"));
                assert(entry.consuming == consuming);
                assert(entry.RequiresEncoding() == (entry.observing || consuming));
            }
            for (uint64_t id = 1; id <= 512; ++id) f.Four(id);
            assert(f.trace.attempts == 0);
            assert(f.collector->pending_typed_rows() == (consuming ? 2048 : observing ? 512 : 0));
            assert(f.router->pending_results() == 0);
            assert(f.budget->Usage().pending_output_bytes == f.collector->typed_storage_bytes());
            f.Four(513);  // Before accepting the next row the 512-row entities are delivered.
            assert(f.trace.attempts == (consuming ? 4 : 0));
            assert(f.collector->pending_typed_rows() == (consuming ? 4 : observing ? 1 : 0));
            auto output = f.Drain();
            assert(output->num_rows() == (observing ? 513 : 0));
            assert(f.collector->pending_results() == 0 && f.collector->typed_storage_bytes() == 0);
            if (consuming) {
                for (const auto& [entity, ids] : f.trace.ids) {
                    assert(ids.size() == 513);
                    assert((f.trace.batches[entity] == std::vector<int64_t>{512, 1}));
                    for (uint64_t row = 0; row < ids.size(); ++row) assert(ids[row] == row + 1);
                }
                assert(f.trace.ids.size() == 4 && f.trace.attempts == 8);
            }
            if (!observing && !consuming) assert(f.budget->reserves == 0);
            assert(f.collector->Finish() == 0 && f.collector->Finish() == 0);
            assert(f.trace.finishes == (consuming ? 1 : 0));
            output.reset();
            assert(f.budget->Usage().pending_output_bytes == 0);
        }
    }
}

void TestValidationAndOwnedStrings() {
    Fixture none("", false);
    auto invalid = Row<NpmBasicResult4>(1, 1);
    invalid.protocol = std::string(1, static_cast<char>(0xff));
    assert(none.collector->WriteBasic(invalid) == EINVAL);
    auto invalid6 = Row<NpmBasicResult6>(1, 1);
    invalid6.protocol = invalid.protocol;
    assert(none.collector->WriteBasic(invalid6) == EINVAL);
    auto session = Row<NpmSessionResult4>(1, 1);
    session.protocol = invalid.protocol;
    assert(none.collector->WriteSession(session) == EINVAL);
    auto session6 = Row<NpmSessionResult6>(1, 1);
    session6.protocol = invalid.protocol;
    assert(none.collector->WriteSession(session6) == EINVAL);
    session.protocol.reset();
    session.revision = 0;
    assert(none.collector->WriteSession(session) == EINVAL);
    none.Four(1);
    assert(none.budget->reserves == 0 && none.collector->pending_results() == 0);

    Fixture both("basic_ipv4", true);
    auto row = Row<NpmBasicResult4>(1, 256);
    assert(both.collector->WriteBasic(row) == 0);
    const auto bytes = both.collector->typed_storage_bytes();
    assert(bytes > sizeof(row) + row.protocol->size());
    row.protocol->assign(256, 'x');
    row.protocol.reset();
    both.trace.consume = [&] {
        assert(both.budget->Usage().pending_output_bytes ==
               both.collector->typed_storage_bytes() + static_cast<uint64_t>(both.router->pool()->bytes_allocated()));
        const auto overlap = both.budget->Usage().pending_output_bytes;
        assert(overlap > bytes);
        assert(both.budget->Reserve(NpmBudgetCategory::kPendingOutput, 4096) == NpmBudgetError::kNone);
        assert(both.budget->Usage().pending_output_bytes == overlap + 4096);
        assert(both.budget->Release(NpmBudgetCategory::kPendingOutput, 4096) == NpmBudgetError::kNone);
    };
    auto output = both.Drain();
    assert(both.trace.protocols["basic_ipv4"] == std::vector<std::string>{std::string(256, 'p')});
    assert(std::static_pointer_cast<arrow::StringArray>(output->GetColumnByName("protocol"))->GetString(0) ==
           std::string(256, 'p'));
    output.reset();
    assert(both.budget->Usage().pending_output_bytes == 0);
}

void TestGlobalSoftLimitAndLargeRow() {
    Fixture f("", true);
    size_t accepted = 0;
    for (uint64_t id = 1; id <= 9; ++id) {
        f.Four(id, 100000);
        accepted += 4;
        assert(f.collector->typed_storage_bytes() <= kNpmTypedStorageSoftLimit);
    }
    assert(f.trace.attempts > 0 && f.collector->pending_typed_rows() > 0);
    for (const auto& [entity, batches] : f.trace.batches)
        for (auto rows : batches) assert(rows < 512);
    auto output = f.Drain();
    assert(output->num_rows() == 0);
    size_t delivered = 0;
    for (const auto& [entity, ids] : f.trace.ids) {
        delivered += ids.size();
        assert(ids.size() == 9);
        for (size_t i = 0; i < ids.size(); ++i) assert(ids[i] == i + 1);
    }
    assert(delivered == accepted && f.budget->Usage().pending_output_bytes == 0);

    assert(f.collector->WriteBasic(Row<NpmBasicResult4>(10, kNpmTypedStorageSoftLimit + 1)) == 0);
    assert(f.collector->pending_typed_rows() == 0 && f.collector->typed_storage_bytes() == 0);
    assert(f.trace.ids["basic_ipv4"].back() == 10 && f.trace.batches["basic_ipv4"].back() == 1);
    assert(f.budget->Usage().pending_output_bytes == 0);
}

void TestFailuresDoNotRetry() {
    for (const int fail_at : {1, 2}) {
        Fixture f("basic_ipv4", true);
        f.trace.fail_at = fail_at;
        f.Four(1);
        assert(f.collector->FlushAccepted() == EIO);
        assert(f.trace.attempts == fail_at && f.trace.ids.size() == static_cast<size_t>(fail_at - 1));
        const auto error = f.router->LastError();
        assert(error.find("consumer Consume failed") != std::string::npos);
        assert(f.collector->pending_results() == 0 && f.budget->Usage().pending_output_bytes == 0);
        assert(f.collector->WriteBasic(Row<NpmBasicResult4>(2)) == EIO);
        assert(f.collector->FlushAccepted() == EIO && f.collector->Finish() == EIO);
        assert(f.trace.attempts == fail_at && f.trace.finishes == 0 && f.router->LastError() == error);
        f.router->Cancel();
        assert(f.collector->FlushAccepted() == EIO && f.router->LastError() == error);
    }
    Fixture f("", true);
    f.trace.finish_error = EIO;
    f.Four(1);
    assert(f.collector->Finish() == EIO);
    assert(f.trace.attempts == 4 && f.trace.finishes == 1);
    assert(f.collector->Finish() == EIO && f.trace.finishes == 1);
    assert(f.budget->Usage().pending_output_bytes == 0);

    Fixture cancelled("basic_ipv4", true);
    cancelled.Four(1);
    cancelled.router->Cancel();
    assert(cancelled.collector->FlushAccepted() == ECANCELED);
    assert(cancelled.trace.attempts == 0 && cancelled.trace.finishes == 0);
    assert(cancelled.collector->pending_results() == 0 && cancelled.budget->Usage().pending_output_bytes == 0);
}

void TestEveryReservationFailure() {
    Fixture complete("basic_ipv4", true);
    complete.Four(1, 32);
    complete.Four(2, 256);
    auto output = complete.Drain();
    const size_t reserves = complete.budget->reserves;
    output.reset();
    assert(complete.budget->Usage().pending_output_bytes == 0);
    for (size_t failure = 1; failure <= reserves; ++failure) {
        Fixture f("basic_ipv4", true);
        f.budget->reject_at = failure;
        int rc = 0;
        for (const uint64_t id : {1, 2}) {
            const auto bytes = id == 1 ? 32 : 256;
            if (!rc) rc = f.collector->WriteBasic(Row<NpmBasicResult4>(id, bytes));
            if (!rc) rc = f.collector->WriteBasic(Row<NpmBasicResult6>(id, bytes));
            if (!rc) rc = f.collector->WriteSession(Row<NpmSessionResult4>(id, bytes));
            if (!rc) rc = f.collector->WriteSession(Row<NpmSessionResult6>(id, bytes));
        }
        if (!rc) rc = f.collector->FlushAccepted();
        std::shared_ptr<arrow::RecordBatch> result;
        if (!rc) {
            rc = f.router->Drain(&result);
            if (rc) f.collector->DiscardAccepted();
        }
        assert(rc == ENOMEM && !result);
        f.router->Discard();
        assert(f.collector->pending_results() == 0 && f.budget->Usage().pending_output_bytes == 0);
        assert(f.collector->FlushAccepted() == ENOMEM && f.collector->Finish() == ENOMEM);
    }
    Fixture hard("", true);
    hard.budget->limit = 1;
    assert(hard.collector->WriteBasic(Row<NpmBasicResult4>(1)) == ENOMEM);
    assert(hard.budget->Usage().pending_output_bytes == 0);
}

void TestTypedAllocationFailures() {
    Fixture measured("", true);
    auto row = Row<NpmBasicResult4>(1, 256);
    new_calls = 0;
    count_new = true;
    for (int i = 0; i < 3; ++i) assert(measured.collector->WriteBasic(row) == 0);
    count_new = false;
    const size_t calls = new_calls;
    assert(calls >= 7);
    measured.collector->DiscardAccepted();
    assert(measured.budget->Usage().pending_output_bytes == 0);
    for (size_t point = 1; point <= calls; ++point) {
        Fixture f("", true);
        new_calls = 0;
        fail_new_at = point;
        count_new = true;
        int rc = 0;
        for (int i = 0; i < 3 && !rc; ++i) rc = f.collector->WriteBasic(row);
        count_new = false;
        fail_new_at = 0;
        assert(rc == ENOMEM && f.trace.attempts == 0);
        assert(f.collector->pending_results() == 0 && f.budget->Usage().pending_output_bytes == 0);
        assert(f.collector->FlushAccepted() == ENOMEM && f.collector->Finish() == ENOMEM);
    }
}

void TestForegroundOwnershipAllocationFailureIsTerminal() {
    Fixture measured("basic_ipv4", true);
    measured.Four(1, 256);
    assert(measured.collector->FlushAccepted() == 0);
    std::shared_ptr<arrow::RecordBatch> output;
    new_calls = 0;
    count_new = true;
    assert(measured.router->Drain(&output) == 0);
    count_new = false;
    const size_t calls = new_calls;
    output.reset();
    assert(calls > 0 && measured.budget->Usage().pending_output_bytes == 0);
    for (size_t point = 1; point <= calls; ++point) {
        Fixture f("basic_ipv4", true);
        f.Four(1, 256);
        assert(f.collector->FlushAccepted() == 0 && f.trace.attempts == 4);
        const auto original = arrow::RecordBatch::MakeEmpty(arrow::schema({})).ValueOrDie();
        output = original;
        new_calls = 0;
        fail_new_at = point;
        count_new = true;
        const int rc = f.router->Drain(&output);
        count_new = false;
        fail_new_at = 0;
        assert(rc == ENOMEM && output == original);
        assert(f.collector->FlushAccepted() == ENOMEM && f.collector->Finish() == ENOMEM);
        assert(f.trace.attempts == 4 && f.trace.finishes == 0);
        assert(f.collector->pending_results() == 0 && f.budget->Usage().pending_output_bytes == 0);
    }
}

void TestRetainedArrayAndBuffer() {
    auto f = std::make_unique<Fixture>("basic_ipv4", true);
    f->Four(1, 256);
    auto rows = f->Drain();
    auto ids = std::static_pointer_cast<arrow::UInt64Array>(rows->GetColumnByName("session_id"));
    auto protocol = rows->GetColumnByName("protocol");
    auto buffer = protocol->data()->buffers.back();
    auto budget = f->budget;
    assert(f->collector->Finish() == 0);
    rows.reset();
    protocol.reset();
    f.reset();
    assert(ids->Value(0) == 1 && buffer->size() == 256 && buffer->data()[255] == 'p');
    assert(budget->Usage().pending_output_bytes > 0);
    ids.reset();
    assert(budget->Usage().pending_output_bytes > 0);
    buffer.reset();
    assert(budget->Usage().pending_output_bytes == 0);
}
}  // namespace

void* operator new(size_t size) {
    if (count_new && ++new_calls == fail_new_at) throw std::bad_alloc();
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { std::free(ptr); }

int main() {
    TestBatchLimitsAndOutlets();
    TestValidationAndOwnedStrings();
    TestGlobalSoftLimitAndLargeRow();
    TestFailuresDoNotRetry();
    TestEveryReservationFailure();
    TestTypedAllocationFailures();
    TestForegroundOwnershipAllocationFailureIsTerminal();
    TestRetainedArrayAndBuffer();
    std::puts("[PASS] bounded family result delivery, outlets, budgets, terminal failures and retained buffers");
}
