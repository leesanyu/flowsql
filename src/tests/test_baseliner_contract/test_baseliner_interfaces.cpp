// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/ibaseline_checkpoint.h>
#include <framework/interfaces/iblock_transform_database_input.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/idatabase_atomic_target.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <iterator>
#include <type_traits>

using namespace flowsql;
static_assert(std::is_abstract_v<IBaselineCheckpoint>);
static_assert(std::is_abstract_v<IBaselineCheckpointService>);
static_assert(std::is_abstract_v<IDatabaseAtomicSession>);
static_assert(std::is_abstract_v<IDatabaseAtomicTarget>);
static_assert(std::is_abstract_v<IBlockTransformDatabaseInputTask>);
static_assert(std::is_abstract_v<IBlockStreamInputProgress>);
static_assert(std::is_abstract_v<IBlockTransformInputProgressTask>);
static_assert(std::has_virtual_destructor_v<IBaselineCheckpoint>);
static_assert(std::has_virtual_destructor_v<IDatabaseAtomicSession>);
static_assert(!std::is_base_of_v<IBaselineTask, IBaselineCheckpoint>);
static_assert(!std::is_base_of_v<IDatabaseChannel, IDatabaseAtomicTarget>);
static_assert(!std::is_base_of_v<IBlockTransformTask, IBlockTransformDatabaseInputTask>);
using CheckpointBind = std::pair<BaselineStatus, std::shared_ptr<IBaselineCheckpoint>> (IBaselineCheckpointService::*)(
    std::shared_ptr<IBaselineTask>, std::shared_ptr<IBaselineTaskStateControl>, const BaselineCheckpointBinding&);
static_assert(std::is_same_v<decltype(&IBaselineCheckpointService::Bind), CheckpointBind>);
using CreateInput = int (IBlockTransformDatabaseInputTask::*)(const BlockDatabaseInputBinding&, BlockDatabaseInput*);
static_assert(std::is_same_v<decltype(&IBlockTransformDatabaseInputTask::CreateDatabaseInput), CreateInput>);
static_assert(std::is_same_v<decltype(&IDatabaseAtomicSession::Cancel), void (IDatabaseAtomicSession::*)()>);

namespace {
// Lifecycle probe only: no database/transaction behavior is claimed by this fixture.
class LeaseProbe final : public IDatabaseChannel {
 public:
    explicit LeaseProbe(bool* destroyed) : destroyed_(destroyed) {}
    ~LeaseProbe() override { *destroyed_ = true; }
    const char* Category() override { return "sqlite"; }
    const char* Name() override { return "probe"; }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return ""; }
    int Open() override { return -1; }
    int Close() override { return -1; }
    bool IsOpened() const override { return false; }
    int Flush() override { return -1; }
    int CreateReader(const char*, IBatchReader**) override { return -1; }
    int CreateWriter(const char*, IBatchWriter**) override { return -1; }
    int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
    int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
    int ExecuteQueryArrow(const char*, std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }
    int WriteArrowBatches(const char*, const std::vector<std::shared_ptr<arrow::RecordBatch>>&) override { return -1; }
    int ExecuteSql(const char*) override { return -1; }
    const char* GetLastError() override { return "not a backend"; }
    bool IsConnected() override { return false; }

 private:
    bool* destroyed_;
};
void TestIndependentIds() {
    const Guid ids[] = {IID_BLOCK_TRANSFORM_DATABASE_INPUT_TASK,
                        IID_BLOCK_STREAM_INPUT_PROGRESS,
                        IID_BLOCK_TRANSFORM_INPUT_PROGRESS_TASK,
                        IID_DATABASE_ATOMIC_TARGET,
                        IID_BASELINE_CHECKPOINT_SERVICE,
                        IID_DATABASE_CHANNEL,
                        IID_BLOCK_TRANSFORM_OPERATOR,
                        IID_BASELINE_SERVICE,
                        IID_BASELINE_STATE_CONTROL_SERVICE};
    for (size_t i = 0; i < std::size(ids); ++i)
        for (size_t j = i + 1; j < std::size(ids); ++j) assert(std::memcmp(&ids[i], &ids[j], sizeof(Guid)) != 0);
}
void TestInputLeaseAndVersions() {
    bool destroyed = false;
    auto owner = std::make_shared<LeaseProbe>(&destroyed);
    BlockDatabaseInputBinding binding;
    assert(!ValidBlockDatabaseInputBinding(binding));
    binding.source = owner;
    binding.exact_source = "sqlite.probe";
    owner.reset();
    assert(!destroyed && ValidBlockDatabaseInputBinding(binding));
    --binding.struct_size;
    assert(!ValidBlockDatabaseInputBinding(binding));
    binding.struct_size = sizeof(binding);
    binding.contract_version = 2;
    assert(!ValidBlockDatabaseInputBinding(binding));
    binding.contract_version = 1;
    binding.operation_timeout_ms = 0;
    assert(!ValidBlockDatabaseInputBinding(binding));
    binding.operation_timeout_ms = 1;
    binding.max_cursor_bytes = 1024 * 1024 + 1;
    assert(!ValidBlockDatabaseInputBinding(binding));
    binding.source.reset();
    assert(destroyed);
    BlockDatabaseInput result;
    assert(result.input == nullptr && !result.schema && result.struct_size == sizeof(result));
    BlockInputProgress progress{1, {{"d", "epoch", "prefix", 4}}};
    auto owned = progress;
    progress.datasets[0].committed_position = "new-prefix";
    assert(owned.datasets[0].committed_position == "prefix");
}
void TestAtomicOutcomes() {
    DatabaseAtomicSessionOptions options;
    assert(ValidDatabaseAtomicSessionOptions(options));
    ++options.struct_size;
    assert(!ValidDatabaseAtomicSessionOptions(options));
    options.struct_size = sizeof(options);
    options.contract_version = 2;
    assert(!ValidDatabaseAtomicSessionOptions(options));
    options.contract_version = 1;
    options.operation_timeout_ms = 0;
    assert(!ValidDatabaseAtomicSessionOptions(options));
    options.operation_timeout_ms = 3600001;
    assert(!ValidDatabaseAtomicSessionOptions(options));
    DatabaseCommitResult result;
    assert(!ValidDatabaseCommitResult(result));
    result.status.code = DatabaseAtomicCode::kCommitUnknown;
    assert(ValidDatabaseCommitResult(result));
    result.outcome = DatabaseCommitOutcome::kCommitted;
    assert(!ValidDatabaseCommitResult(result));
    result.status.code = DatabaseAtomicCode::kOk;
    assert(ValidDatabaseCommitResult(result));
    result.outcome = DatabaseCommitOutcome::kRolledBack;
    assert(!ValidDatabaseCommitResult(result));
    result.status.code = DatabaseAtomicCode::kCancelled;
    assert(ValidDatabaseCommitResult(result));
    result.status.code = DatabaseAtomicCode::kCommitUnknown;
    assert(!ValidDatabaseCommitResult(result));
}
void TestCheckpointFraming() {
    BaselineCheckpointBinding binding;
    assert(ValidBaselineCheckpointBinding(binding));
    ++binding.contract_version;
    assert(!ValidBaselineCheckpointBinding(binding));
    binding.contract_version = 1;
    binding.max_payload_bytes = 0;
    assert(!ValidBaselineCheckpointBinding(binding));
    binding.max_payload_bytes = kBaselineCheckpointMaxBytes + 1;
    assert(!ValidBaselineCheckpointBinding(binding));
    BaselineCheckpointRestore restore;
    assert(!ValidBaselineCheckpointRestore(restore, 2));
    restore.content = "{}";
    assert(ValidBaselineCheckpointRestore(restore, 2));
    assert(!ValidBaselineCheckpointRestore(restore, 1));
    --restore.struct_size;
    assert(!ValidBaselineCheckpointRestore(restore, 2));
    restore.struct_size = sizeof(restore);
    ++restore.payload_version;
    assert(!ValidBaselineCheckpointRestore(restore, 2));
    restore.payload_version = 1;
    ++restore.contract_version;
    assert(!ValidBaselineCheckpointRestore(restore, 2));
    restore.contract_version = 1;
    restore.format = static_cast<BaselineSerializationFormat>(99);
    assert(!ValidBaselineCheckpointRestore(restore, 2));
}
}  // namespace
int main() {
    TestIndependentIds();
    TestInputLeaseAndVersions();
    TestAtomicOutcomes();
    TestCheckpointFraming();
    std::cout << "versioned input, transaction and checkpoint interface contracts passed\n";
}
