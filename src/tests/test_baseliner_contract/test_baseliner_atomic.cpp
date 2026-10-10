// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/idatabase_atomic_target.h>
#include <operators/baseliner/writer_fence.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace flowsql;
class LostAck final : public IDatabaseAtomicSessionV1 {
 public:
    explicit LostAck(std::shared_ptr<IDatabaseAtomicSessionV1> session) : session_(std::move(session)) {}
    bool armed = false;
    DatabaseAtomicStatusV1 Begin() override { return session_->Begin(); }
    DatabaseAtomicStatusV1 ExecutePrepared(const char* sql, const DatabaseParameterV1* p, size_t n,
                                           uint64_t* affected) override {
        return session_->ExecutePrepared(sql, p, n, affected);
    }
    DatabaseAtomicStatusV1 CreateReader(const char* sql, const DatabaseParameterV1* p, size_t n,
                                        IBatchReader** r) override {
        return session_->CreateReader(sql, p, n, r);
    }
    DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t* t) override { return session_->ReadDatabaseTime(t); }
    DatabaseCommitResultV1 Commit() override {
        auto result = session_->Commit();
        if (armed && result.status.ok()) {
            armed = false;
            session_->Close();
            return {{DatabaseAtomicCodeV1::kCommitUnknown, "lost acknowledgement"}, DatabaseCommitOutcomeV1::kUnknown};
        }
        return result;
    }
    DatabaseAtomicStatusV1 Rollback() override { return session_->Rollback(); }
    void Cancel() override { session_->Cancel(); }
    void Close() override { session_->Close(); }

 private:
    std::shared_ptr<IDatabaseAtomicSessionV1> session_;
};
void Test(std::shared_ptr<IDatabaseChannel> channel, const std::string& backend) {
    assert(channel);
    auto* target = dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get());
    assert(target);
    std::shared_ptr<IDatabaseAtomicSessionV1> a, b;
    assert(target->AcquireAtomicSession({}, &a).ok());
    DatabaseAtomicSessionOptionsV1 short_timeout;
    short_timeout.operation_timeout_ms = 50;
    assert(target->AcquireAtomicSession(short_timeout, &b).ok() && a != b);
    auto invalid = short_timeout;
    invalid.contract_version = 2;
    std::shared_ptr<IDatabaseAtomicSessionV1> rejected = a;
    assert(!target->AcquireAtomicSession(invalid, &rejected).ok() && !rejected);
    uint64_t n = 0;
    {
        auto status = a->Begin();
        if (!status.ok()) std::cerr << backend << ": " << status.message << std::endl;
        assert(status.ok());
    }
    assert(a->ExecutePrepared("CREATE TABLE IF NOT EXISTS atomic_test(id BIGINT PRIMARY KEY,v TEXT)", nullptr, 0, &n)
               .ok());
    assert(a->Commit().outcome == DatabaseCommitOutcomeV1::kCommitted);
    assert(a->Begin().ok());
    assert(a->ExecutePrepared("DELETE FROM atomic_test", nullptr, 0, &n).ok());
    assert(a->ExecutePrepared("INSERT INTO atomic_test VALUES(1,'uncommitted')", nullptr, 0, &n).ok() && n == 1);
    assert(b->Begin().ok());
    IBatchReader* reader = nullptr;
    assert(b->CreateReader("SELECT id FROM atomic_test", nullptr, 0, &reader).ok());
    const uint8_t* data;
    size_t size;
    assert(reader->Next(&data, &size) == 1);
    reader->Close();
    reader->Release();
    assert(b->Rollback().ok());
    assert(b->Begin().ok());
    auto busy = b->ExecutePrepared("INSERT INTO atomic_test VALUES(1,'busy')", nullptr, 0, &n);
    if (busy.ok() || busy.code != DatabaseAtomicCodeV1::kTimeout)
        std::cerr << backend << ": busy code=" << static_cast<int>(busy.code) << " " << busy.message << std::endl;
    assert(!busy.ok() && busy.code == DatabaseAtomicCodeV1::kTimeout && n == 0);
    if (backend == "sqlite")
        assert(b->Rollback().ok());
    else
        b->Close();
    assert(a->Rollback().ok());
    int64_t t1, t2;
    assert(a->ReadDatabaseTime(&t1).ok() && t1 > 0);
    assert(a->ReadDatabaseTime(&t2).ok() && t2 >= t1);
    assert(a->Begin().ok());
    assert(a->ExecutePrepared("INSERT INTO atomic_test VALUES(2,'x')", nullptr, 0, &n).ok() && n == 1);
    assert(a->ExecutePrepared("UPDATE atomic_test SET v='y' WHERE id=3", nullptr, 0, &n).ok() && n == 0);
    assert(!a->ExecutePrepared("INSERT INTO atomic_test VALUES(2,'duplicate')", nullptr, 0, &n).ok() && n == 0);
    assert(a->Rollback().ok());
    if (backend == "sqlite") {
        assert(a->Begin().ok());
        assert(
            a->ExecutePrepared("CREATE TABLE IF NOT EXISTS atomic_parent(id BIGINT PRIMARY KEY)", nullptr, 0, &n).ok());
        assert(a->ExecutePrepared("CREATE TABLE IF NOT EXISTS atomic_child(id BIGINT REFERENCES atomic_parent(id) "
                                  "DEFERRABLE INITIALLY DEFERRED)",
                                  nullptr, 0, &n)
                   .ok());
        assert(a->Commit().status.ok());
        assert(a->Begin().ok());
        assert(a->ExecutePrepared("INSERT INTO atomic_child VALUES(777)", nullptr, 0, &n).ok());
        auto failed_commit = a->Commit();
        assert(failed_commit.outcome == DatabaseCommitOutcomeV1::kRolledBack && !failed_commit.status.ok());
    }
    assert(a->Begin().ok());
    DatabaseParameterV1 p;
    p.kind = DatabaseParameterKindV1::kUInt64;
    p.uint64_value = UINT64_MAX;
    assert(a->ExecutePrepared("INSERT INTO atomic_test VALUES(42,?)", &p, 1, &n).ok());
    std::shared_ptr<arrow::RecordBatch> exact;
    assert(flowsql::baseliner::ReadAtomicBatch(*a, "SELECT v FROM atomic_test WHERE id=42", {}, &exact).ok());
    assert(exact->column(0)->GetScalar(0).ValueOrDie()->ToString() == "18446744073709551615");
    assert(a->Rollback().ok());
    a->Cancel();
    assert(!a->Begin().ok());
    a->Close();
    a->Close();
    b->Close();
    a.reset();
    {
        using namespace flowsql::baseliner;
        auto fresh = [&] {
            std::shared_ptr<IDatabaseAtomicSessionV1> s;
            assert(target->AcquireAtomicSession({}, &s).ok());
            return s;
        };
        WriterFence first, conflict;
        const auto key = "writer-" + std::to_string(t1);
        assert(first.Acquire(fresh(), backend, key, "first", "hash", 10000) == 0);
        assert(conflict.Acquire(fresh(), backend, key, "conflict", "hash", 10000) != 0);
        assert(first.Renew() == 0 && first.Stage() == 0);
        assert(first.Confirm(first.Session().Commit()) == 0 && first.Generation() == 1);
        // Expiration is based on target time; force a past database deadline to exercise takeover.
        auto expiry = fresh();
        uint64_t changed;
        assert(expiry->Begin().ok());
        auto p = TextParameter(key);
        assert(expiry->ExecutePrepared("UPDATE baseline_tasks SET lease_deadline=0 WHERE task_key=?", &p, 1, &changed)
                   .ok());
        assert(expiry->Commit().status.ok());
        WriterFence next;
        assert(next.Acquire(fresh(), backend, key, "next", "hash", 10000) == 0 && next.Epoch() == 2 &&
               next.Generation() == 1);
        assert(first.Stage() != 0);
        assert(next.Stage() == 0 && next.Confirm(next.Session().Commit()) == 0);
        // Rollback the staged generation: durable current must remain 2.
        assert(next.Stage() == 0 && next.Session().Rollback().ok());
        DatabaseCommitResultV1 rollback{{DatabaseAtomicCodeV1::kError, "injected rollback"},
                                        DatabaseCommitOutcomeV1::kRolledBack};
        assert(next.Confirm(rollback) != 0);
    }

    {
        using namespace flowsql::baseliner;
        auto fresh = [&] {
            std::shared_ptr<IDatabaseAtomicSessionV1> s;
            assert(target->AcquireAtomicSession({}, &s).ok());
            return s;
        };
        auto key = "lost-ack-" + std::to_string(t1);
        auto wrapped = std::make_shared<LostAck>(fresh());
        WriterFence writer;
        assert(writer.Acquire(wrapped, backend, key, "lost", "hash", 10000) == 0);
        wrapped->armed = true;
        assert(writer.Stage() == 0);
        auto unknown = wrapped->Commit();
        assert(unknown.outcome == DatabaseCommitOutcomeV1::kUnknown && ValidDatabaseCommitResultV1(unknown));
        assert(writer.Confirm(unknown) != 0 && writer.Generation() == 0 && writer.Stage() != 0);
        auto inspection = fresh();
        std::shared_ptr<arrow::RecordBatch> state;
        assert(ReadAtomicBatch(*inspection, "SELECT current_generation FROM baseline_tasks WHERE task_key=?",
                               {TextParameter(key)}, &state)
                   .ok());
        assert(std::static_pointer_cast<arrow::Int64Array>(state->column(0))->Value(0) == 1);
        assert(inspection->Begin().ok());
        auto p = TextParameter(key);
        uint64_t n;
        assert(
            inspection->ExecutePrepared("UPDATE baseline_tasks SET lease_deadline=0 WHERE task_key=?", &p, 1, &n).ok());
        assert(inspection->Commit().status.ok());
        WriterFence restart;
        assert(restart.Acquire(fresh(), backend, key, "restart", "hash", 10000) == 0 && restart.Generation() == 1);
        // Initialization/acquisition race has exactly one owner before either consumes data.
        auto race_key = "race-" + std::to_string(t1);
        WriterFence left, right;
        auto l = fresh(), r = fresh();
        int lr = -1, rr = -1;
        std::thread lt([&] { lr = left.Acquire(l, backend, race_key, "left", "hash", 10000); });
        std::thread rt([&] { rr = right.Acquire(r, backend, race_key, "right", "hash", 10000); });
        lt.join();
        rt.join();
        assert((lr == 0) != (rr == 0));
        if (backend != "sqlite") {
            auto interrupted = fresh();
            assert(interrupted->Begin().ok());
            interrupted->Cancel();
            auto outcome = interrupted->Commit();
            assert(outcome.outcome == DatabaseCommitOutcomeV1::kUnknown && ValidDatabaseCommitResultV1(outcome));
            assert(!interrupted->Rollback().ok());
        }
    }
    b.reset();
    channel.reset();
    std::cout << "atomic " << backend << " passed\n";
}

int main() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream options;
    options << input.rdbuf();
    database::DatabasePlugin plugin;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = plugin.Option(options.str().c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(rc == 0);
    assert(plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    for (auto backend : {"sqlite", "mysql", "postgres"})
        Test(plugin.AcquireChannel(backend, ("t2" + std::string(backend)).c_str()), backend);
    auto clickhouse = plugin.AcquireChannel("clickhouse", "t2clickhouse");
    assert(clickhouse);
    std::shared_ptr<IDatabaseAtomicSessionV1> session;
    assert(!dynamic_cast<IDatabaseAtomicTargetV1*>(clickhouse.get())->AcquireAtomicSession({}, &session).ok() &&
           !session);
    clickhouse.reset();
    plugin.Stop();
    plugin.Unload();
}
