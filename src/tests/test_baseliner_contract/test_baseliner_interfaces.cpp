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
static_assert(std::is_abstract_v<IBaselineCheckpointV1>);
static_assert(std::is_abstract_v<IBaselineCheckpointServiceV1>);
static_assert(std::is_abstract_v<IDatabaseAtomicSessionV1>);
static_assert(std::is_abstract_v<IDatabaseAtomicTargetV1>);
static_assert(std::is_abstract_v<IBlockTransformDatabaseInputTaskV1>);
static_assert(std::is_abstract_v<IBlockStreamInputProgressV1>);
static_assert(std::is_abstract_v<IBlockTransformInputProgressTaskV1>);
static_assert(std::has_virtual_destructor_v<IBaselineCheckpointV1>);
static_assert(std::has_virtual_destructor_v<IDatabaseAtomicSessionV1>);
static_assert(!std::is_base_of_v<IBaselineTask, IBaselineCheckpointV1>);
static_assert(!std::is_base_of_v<IDatabaseChannel, IDatabaseAtomicTargetV1>);
static_assert(!std::is_base_of_v<IBlockTransformTaskV2, IBlockTransformDatabaseInputTaskV1>);
using CheckpointBind = std::pair<BaselineStatus, std::shared_ptr<IBaselineCheckpointV1>> (
    IBaselineCheckpointServiceV1::*)(std::shared_ptr<IBaselineTask>, std::shared_ptr<IBaselineTaskStateControlV1>,
                                     const BaselineCheckpointBindingV1&);
static_assert(std::is_same_v<decltype(&IBaselineCheckpointServiceV1::Bind), CheckpointBind>);
using CreateInput = int (IBlockTransformDatabaseInputTaskV1::*)(const BlockDatabaseInputBindingV1&,
                                                                BlockDatabaseInputV1*);
static_assert(std::is_same_v<decltype(&IBlockTransformDatabaseInputTaskV1::CreateDatabaseInput), CreateInput>);
static_assert(std::is_same_v<decltype(&IDatabaseAtomicSessionV1::Cancel), void (IDatabaseAtomicSessionV1::*)()>);

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
    const Guid ids[] = {IID_BLOCK_TRANSFORM_DATABASE_INPUT_TASK_V1,
                        IID_BLOCK_STREAM_INPUT_PROGRESS_V1,
                        IID_BLOCK_TRANSFORM_INPUT_PROGRESS_TASK_V1,
                        IID_DATABASE_ATOMIC_TARGET_V1,
                        IID_BASELINE_CHECKPOINT_SERVICE_V1,
                        IID_DATABASE_CHANNEL,
                        IID_BLOCK_TRANSFORM_OPERATOR_V1,
                        IID_BLOCK_TRANSFORM_OPERATOR_V2,
                        IID_BASELINE_SERVICE,
                        IID_BASELINE_STATE_CONTROL_SERVICE_V1};
    for (size_t i = 0; i < std::size(ids); ++i)
        for (size_t j = i + 1; j < std::size(ids); ++j) assert(std::memcmp(&ids[i], &ids[j], sizeof(Guid)) != 0);
}
void TestInputLeaseAndVersions() {
    bool destroyed = false;
    auto owner = std::make_shared<LeaseProbe>(&destroyed);
    BlockDatabaseInputBindingV1 binding;
    assert(!ValidBlockDatabaseInputBindingV1(binding));
    binding.source = owner;
    binding.exact_source = "sqlite.probe";
    owner.reset();
    assert(!destroyed && ValidBlockDatabaseInputBindingV1(binding));
    --binding.struct_size;
    assert(!ValidBlockDatabaseInputBindingV1(binding));
    binding.struct_size = sizeof(binding);
    binding.contract_version = 2;
    assert(!ValidBlockDatabaseInputBindingV1(binding));
    binding.contract_version = 1;
    binding.operation_timeout_ms = 0;
    assert(!ValidBlockDatabaseInputBindingV1(binding));
    binding.operation_timeout_ms = 1;
    binding.max_cursor_bytes = 1024 * 1024 + 1;
    assert(!ValidBlockDatabaseInputBindingV1(binding));
    binding.source.reset();
    assert(destroyed);
    BlockDatabaseInputV1 result;
    assert(result.input == nullptr && !result.schema && result.struct_size == sizeof(result));
    BlockInputProgressV1 progress{1, {{"d", "epoch", "prefix", 4}}};
    auto owned = progress;
    progress.datasets[0].committed_position = "new-prefix";
    assert(owned.datasets[0].committed_position == "prefix");
}
void TestAtomicOutcomes() {
    DatabaseAtomicSessionOptionsV1 options;
    assert(ValidDatabaseAtomicSessionOptionsV1(options));
    ++options.struct_size;
    assert(!ValidDatabaseAtomicSessionOptionsV1(options));
    options.struct_size = sizeof(options);
    options.contract_version = 2;
    assert(!ValidDatabaseAtomicSessionOptionsV1(options));
    options.contract_version = 1;
    options.operation_timeout_ms = 0;
    assert(!ValidDatabaseAtomicSessionOptionsV1(options));
    options.operation_timeout_ms = 3600001;
    assert(!ValidDatabaseAtomicSessionOptionsV1(options));
    DatabaseCommitResultV1 result;
    assert(!ValidDatabaseCommitResultV1(result));
    result.status.code = DatabaseAtomicCodeV1::kCommitUnknown;
    assert(ValidDatabaseCommitResultV1(result));
    result.outcome = DatabaseCommitOutcomeV1::kCommitted;
    assert(!ValidDatabaseCommitResultV1(result));
    result.status.code = DatabaseAtomicCodeV1::kOk;
    assert(ValidDatabaseCommitResultV1(result));
    result.outcome = DatabaseCommitOutcomeV1::kRolledBack;
    assert(!ValidDatabaseCommitResultV1(result));
    result.status.code = DatabaseAtomicCodeV1::kCancelled;
    assert(ValidDatabaseCommitResultV1(result));
    result.status.code = DatabaseAtomicCodeV1::kCommitUnknown;
    assert(!ValidDatabaseCommitResultV1(result));
}
void TestCheckpointFraming() {
    BaselineCheckpointBindingV1 binding;
    assert(ValidBaselineCheckpointBindingV1(binding));
    ++binding.contract_version;
    assert(!ValidBaselineCheckpointBindingV1(binding));
    binding.contract_version = 1;
    binding.max_payload_bytes = 0;
    assert(!ValidBaselineCheckpointBindingV1(binding));
    binding.max_payload_bytes = kBaselineCheckpointMaxBytesV1 + 1;
    assert(!ValidBaselineCheckpointBindingV1(binding));
    BaselineCheckpointRestoreV1 restore;
    assert(!ValidBaselineCheckpointRestoreV1(restore, 2));
    restore.content = "{}";
    assert(ValidBaselineCheckpointRestoreV1(restore, 2));
    assert(!ValidBaselineCheckpointRestoreV1(restore, 1));
    --restore.struct_size;
    assert(!ValidBaselineCheckpointRestoreV1(restore, 2));
    restore.struct_size = sizeof(restore);
    ++restore.payload_version;
    assert(!ValidBaselineCheckpointRestoreV1(restore, 2));
    restore.payload_version = 1;
    ++restore.contract_version;
    assert(!ValidBaselineCheckpointRestoreV1(restore, 2));
    restore.contract_version = 1;
    restore.format = static_cast<BaselineSerializationFormat>(99);
    assert(!ValidBaselineCheckpointRestoreV1(restore, 2));
}
}  // namespace
int main() {
    TestIndependentIds();
    TestInputLeaseAndVersions();
    TestAtomicOutcomes();
    TestCheckpointFraming();
    std::cout << "versioned input, transaction and checkpoint interface contracts passed\n";
}
