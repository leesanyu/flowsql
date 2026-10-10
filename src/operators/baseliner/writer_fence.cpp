// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "writer_fence.h"
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <cstring>
#include <limits>
namespace flowsql::baseliner {
DatabaseParameterV1 TextParameter(const std::string& text) {
    DatabaseParameterV1 p;
    p.kind = DatabaseParameterKindV1::kString;
    p.data = text.data();
    p.size = text.size();
    return p;
}
DatabaseParameterV1 IntParameter(int64_t value) {
    DatabaseParameterV1 p;
    p.kind = DatabaseParameterKindV1::kInt64;
    p.int64_value = value;
    return p;
}
DatabaseAtomicStatusV1 ReadAtomicBatch(IDatabaseAtomicSessionV1& session, const std::string& sql,
                                       const std::vector<DatabaseParameterV1>& p,
                                       std::shared_ptr<arrow::RecordBatch>* out) {
    out->reset();
    IBatchReader* raw = nullptr;
    auto s = session.CreateReader(sql.c_str(), p.data(), p.size(), &raw);
    if (!s.ok()) return s;
    std::unique_ptr<IBatchReader, void (*)(IBatchReader*)> reader(raw, [](auto* r) {
        r->Close();
        r->Release();
    });
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (raw->GetSchema(&data, &size) != 0) return {DatabaseAtomicCodeV1::kError, "atomic schema read"};
    arrow::io::BufferReader schema_reader(arrow::Buffer::Wrap(data, size));
    arrow::ipc::DictionaryMemo memo;
    auto schema = arrow::ipc::ReadSchema(&schema_reader, &memo);
    if (!schema.ok()) return {DatabaseAtomicCodeV1::kError, schema.status().ToString()};
    int rc = raw->Next(&data, &size);
    if (rc == 1) {
        auto empty = arrow::RecordBatch::MakeEmpty(*schema);
        if (empty.ok()) {
            *out = *empty;
            return {};
        }
    }
    if (rc != 0) return {DatabaseAtomicCodeV1::kError, raw->GetLastError()};
    arrow::io::BufferReader batch_reader(arrow::Buffer::Wrap(data, size));
    auto batch = arrow::ipc::ReadRecordBatch(*schema, &memo, arrow::ipc::IpcReadOptions::Defaults(), &batch_reader);
    if (!batch.ok()) return {DatabaseAtomicCodeV1::kError, batch.status().ToString()};
    // The native reader buffers own this IPC. Copy decoded arrays before reader destruction.
    auto copied = arrow::ipc::SerializeRecordBatch(**batch, arrow::ipc::IpcWriteOptions::Defaults());
    if (!copied.ok()) return {DatabaseAtomicCodeV1::kError, copied.status().ToString()};
    auto buffer = arrow::AllocateBuffer((*copied)->size());
    if (!buffer.ok()) return {DatabaseAtomicCodeV1::kError, buffer.status().ToString()};
    memcpy((*buffer)->mutable_data(), (*copied)->data(), (*copied)->size());
    arrow::io::BufferReader owned(std::shared_ptr<arrow::Buffer>(std::move(*buffer)));
    auto decoded = arrow::ipc::ReadRecordBatch(*schema, &memo, arrow::ipc::IpcReadOptions::Defaults(), &owned);
    if (!decoded.ok()) return {DatabaseAtomicCodeV1::kError, decoded.status().ToString()};
    *out = *decoded;
    return {};
}
WriterFence::~WriterFence() { Release(); }
int WriterFence::Fail(std::string e) {
    error_ = std::move(e);
    failed_ = true;
    return -1;
}
int WriterFence::Acquire(std::shared_ptr<IDatabaseAtomicSessionV1> session, const std::string& backend,
                         std::string task, std::string owner, std::string fingerprint, int64_t lease_ms) {
    if (session_ || !session || task.empty() || owner.empty() || lease_ms <= 0) return Fail("invalid writer binding");
    session_ = std::move(session);
    task_ = std::move(task);
    owner_ = std::move(owner);
    lease_ms_ = lease_ms;
    auto& s = *session_;
    uint64_t n = 0;
    auto status = s.Begin();
    if (!status.ok()) return Fail(status.message);
    const auto key_type = backend == "mysql" ? "VARBINARY(1024)" : "VARCHAR(256)";
    const std::string create =
        std::string("CREATE TABLE IF NOT EXISTS baseline_tasks(task_key ") + key_type +
        " PRIMARY KEY,config_hash VARCHAR(128) NOT NULL,owner_run " + key_type +
        " NOT NULL,writer_epoch BIGINT NOT NULL,current_generation BIGINT NOT NULL,lease_deadline BIGINT NOT NULL)";
    status = s.ExecutePrepared(create.c_str(), nullptr, 0, &n);
    if (!status.ok()) {
        s.Rollback();
        return Fail(status.message);
    }
    auto committed = s.Commit();
    if (!committed.status.ok()) return Fail(committed.status.message);
    if (!(status = s.Begin()).ok()) return Fail(status.message);
    int64_t now = 0;
    if (!(status = s.ReadDatabaseTime(&now)).ok() || now > std::numeric_limits<int64_t>::max() - lease_ms_) {
        s.Rollback();
        return Fail("invalid target clock");
    }
    std::string insert = backend == "mysql" ? "INSERT IGNORE INTO" : "INSERT INTO";
    insert += " baseline_tasks VALUES(?,?,?,1,0,?)";
    if (backend != "mysql") insert += " ON CONFLICT(task_key) DO NOTHING";
    std::vector<DatabaseParameterV1> p = {TextParameter(task_), TextParameter(fingerprint), TextParameter(owner_),
                                          IntParameter(now + lease_ms_)};
    status = s.ExecutePrepared(insert.c_str(), p.data(), p.size(), &n);
    if (!status.ok()) {
        s.Rollback();
        return Fail(status.message);
    }
    if (n == 0) {
        p = {TextParameter(owner_), IntParameter(now + lease_ms_), TextParameter(task_), TextParameter(fingerprint),
             IntParameter(now)};
        status = s.ExecutePrepared(
            "UPDATE baseline_tasks SET owner_run=?,lease_deadline=?,writer_epoch=writer_epoch+1 WHERE task_key=? AND "
            "config_hash=? AND lease_deadline<=?",
            p.data(), p.size(), &n);
        if (!status.ok() || n != 1) {
            s.Rollback();
            return Fail("writer busy or incompatible configuration");
        }
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    status = ReadAtomicBatch(s, "SELECT writer_epoch,current_generation FROM baseline_tasks WHERE task_key=?",
                             {TextParameter(task_)}, &batch);
    if (!status.ok() || batch->num_rows() != 1) {
        s.Rollback();
        return Fail("writer state missing");
    }
    epoch_ = std::static_pointer_cast<arrow::Int64Array>(batch->column(0))->Value(0);
    generation_ = std::static_pointer_cast<arrow::Int64Array>(batch->column(1))->Value(0);
    committed = s.Commit();
    if (!committed.status.ok()) return Fail(committed.status.message);
    return 0;
}
int WriterFence::Change(bool publish) {
    if (!session_ || failed_ || staged_) return Fail("writer unavailable");
    auto& s = *session_;
    auto status = s.Begin();
    if (!status.ok()) return Fail(status.message);
    int64_t now = 0;
    if (!(status = s.ReadDatabaseTime(&now)).ok() || now > std::numeric_limits<int64_t>::max() - lease_ms_ ||
        generation_ == std::numeric_limits<int64_t>::max()) {
        s.Rollback();
        return Fail("writer clock/generation overflow");
    }
    auto p = std::vector<DatabaseParameterV1>{IntParameter(now + lease_ms_), TextParameter(task_),
                                              TextParameter(owner_),         IntParameter(epoch_),
                                              IntParameter(generation_),     IntParameter(now)};
    uint64_t n = 0;
    std::string sql = "UPDATE baseline_tasks SET lease_deadline=?";
    if (publish) sql += ",current_generation=current_generation+1";
    sql += " WHERE task_key=? AND owner_run=? AND writer_epoch=? AND current_generation=? AND lease_deadline>?";
    status = s.ExecutePrepared(sql.c_str(), p.data(), p.size(), &n);
    if (!status.ok() || n != 1) {
        s.Rollback();
        return Fail("stale/expired writer or generation conflict");
    }
    if (publish) {
        staged_ = true;
        return 0;
    }
    auto result = s.Commit();
    return result.status.ok() ? 0 : Fail(result.status.message);
}
int WriterFence::Renew() { return Change(false); }
int WriterFence::Stage() { return Change(true); }
int WriterFence::Confirm(const DatabaseCommitResultV1& result) {
    if (!staged_ || !ValidDatabaseCommitResultV1(result)) return Fail("invalid commit result");
    staged_ = false;
    if (!result.status.ok()) return Fail(result.status.message);
    ++generation_;
    return 0;
}
void WriterFence::Release() {
    if (!session_) return;
    if (staged_) session_->Rollback();
    if (!failed_ && !staged_) {
        if (session_->Begin().ok()) {
            auto p =
                std::vector<DatabaseParameterV1>{TextParameter(task_), TextParameter(owner_), IntParameter(epoch_)};
            uint64_t n = 0;
            auto s = session_->ExecutePrepared(
                "UPDATE baseline_tasks SET lease_deadline=0 WHERE task_key=? AND owner_run=? AND writer_epoch=?",
                p.data(), p.size(), &n);
            if (s.ok())
                session_->Commit();
            else
                session_->Rollback();
        }
    }
    session_->Close();
    session_.reset();
}
}  // namespace flowsql::baseliner
