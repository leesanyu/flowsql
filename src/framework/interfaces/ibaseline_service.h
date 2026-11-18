// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <framework/interfaces/ibaseline_evaluation.h>

#include <memory>
#include <string_view>
#include <utility>

namespace flowsql {

// {f27073e4-98c7-4749-bd3d-83bd1b0d3a20}
const Guid IID_BASELINE_SERVICE = {
    0xf27073e4, 0x98c7, 0x4749, {0xbd, 0x3d, 0x83, 0xbd, 0x1b, 0x0d, 0x3a, 0x20}
};

/**
 * @brief Baseline 任务基接口，定义任务身份、状态查询、配置导出与关闭能力。
 *
 * 线程契约：
 * - 同一个任务实例内部不做并发同步，调用方或上游调度器必须保证同一任务实例的接口调用不重叠执行。
 * - 如果同一任务实例的相邻调用运行在不同物理线程上，上游调度器必须保证前一次调用结束先行发生于后一次调用开始。
 * - Id()、Name()、Kind() 只读取不可变身份信息，任务生命周期内允许跨线程读取。
 * - 不同任务实例之间可以并发调用；同一个任务实例不要求绑定固定物理线程。
 * - 除非后续实现明确声明更强保证，否则同一任务实例的重叠调用不属于本接口契约。
 */
interface IBaselineTask {
    virtual ~IBaselineTask() = default;

    /**
     * @brief 返回任务 ID。
     * @return 任务 ID 字符串指针。
     */
    virtual const char* Id() const = 0;
    /**
     * @brief 返回任务名称。
     * @return 任务名称字符串指针。
     */
    virtual const char* Name() const = 0;
    /**
     * @brief 返回任务类型。
     * @return BaselineTaskKind 枚举值。
     */
    virtual BaselineTaskKind Kind() const = 0;

    /**
     * @brief 导出创建任务时接收的原始配置。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容；不支持的格式返回 kUnsupportedFormat。
     */
    virtual BaselineSerializationResult ExportConfig(
        BaselineSerializationFormat format) const = 0;

    /**
     * @brief 查询任务级诊断快照。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与快照内容；快照用于观测和调试，不作为热路径状态传输协议。
     */
    virtual BaselineSerializationResult QueryTaskSnapshot(
        BaselineSerializationFormat format) const = 0;

    /**
     * @brief 查询指定序列的诊断快照。
     * @param series_key 序列标识。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与快照内容；序列状态不存在时返回 kNotTrained。
     */
    virtual BaselineSerializationResult QuerySeriesSnapshot(
        std::string_view series_key,
        BaselineSerializationFormat format) const = 0;

    /**
     * @brief 关闭任务并从服务注册表中注销。
     * @return kOk 表示关闭成功；关闭后的业务调用应返回非 kOk 状态且不得继续修改学习状态。
     */
    virtual BaselineStatus Close() = 0;
};

/**
 * @brief 数值型 Baseline 任务接口，继承 IBaselineTask 的同任务非并发调用契约。
 */
interface IBaselineValueTask : public IBaselineTask {
    /**
     * @brief 提交一条在线观测并推进 rolling 状态。
     * @param obs 数值型观测。
     * @param options rolling 提交选项。
     * @return 完整 rolling 更新结果与本次求值 readiness。
     */
    virtual BaselineEvaluationResult SubmitObservation(const ValueRollingObservation& obs,
                                                       const RollingSubmitOptions& options) = 0;
    /**
     * @brief 只读预测单个未来 bucket。
     * @param series_key 序列标识。
     * @param bucket_id 待预测 bucket，必须大于该序列最后观测 bucket。
     * @return rolling 预测结果。
     */
    virtual RollingPrediction PredictRolling(
        std::string_view series_key,
        int64_t bucket_id) const = 0;
    /**
     * @brief 只读预测连续 bucket 区间。
     * @param series_key 序列标识。
     * @param start_bucket_id 起始 bucket。
     * @param point_count 预测点数，必须大于 0。
     * @return rolling 预测序列，区间为 [start_bucket_id, start_bucket_id + point_count)。
     */
    virtual RollingPredictionSequence PredictRolling(
        std::string_view series_key,
        int64_t start_bucket_id,
        uint32_t point_count) const = 0;

    /**
     * @brief 训练或替换指定序列的 bootstrap artifact。
     * @param input 训练输入；当 force_replace_existing_artifact 为 false 且 artifact 已存在时拒绝替换。
     * @return 训练结果。
     */
    virtual BootstrapTrainResult Bootstrap(const ValueBootstrapInput& input) = 0;
    /**
     * @brief 使用已训练 bootstrap artifact 只读预测单个 bucket。
     * @param series_key 序列标识。
     * @param bucket_id 待预测 bucket。
     * @param options 预测选项。
     * @return bootstrap 预测结果。
     */
    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;
    /**
     * @brief 使用已训练 bootstrap artifact 只读预测连续 bucket 区间。
     * @param series_key 序列标识。
     * @param start_bucket_id 起始 bucket。
     * @param point_count 预测点数，必须大于 0。
     * @param options 预测选项。
     * @return bootstrap 预测序列，区间为 [start_bucket_id, start_bucket_id + point_count)。
     */
    virtual BootstrapPredictionSequence PredictBootstrap(
        std::string_view series_key,
        int64_t start_bucket_id,
        uint32_t point_count,
        const BootstrapPredictionOptions& options) const = 0;
    /**
     * @brief 导出当前任务持有的所有 bootstrap artifact。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    /**
     * @brief 加载 bootstrap artifact，并重建兼容的 rolling 预热状态。
     * @param content 序列化内容。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 加载状态。
     */
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    /**
     * @brief 导出由当前 artifact 派生的 bootstrap seed。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

/**
 * @brief 比例型 Baseline 任务接口，继承 IBaselineTask 的同任务非并发调用契约。
 */
interface IBaselineRatioTask : public IBaselineTask {
    /**
     * @brief 提交一条在线比例观测并推进 rolling 状态。
     * @param obs 比例型观测。
     * @param options rolling 提交选项。
     * @return 完整 rolling 更新结果与本次求值 readiness。
     */
    virtual BaselineEvaluationResult SubmitObservation(const RatioRollingObservation& obs,
                                                       const RollingSubmitOptions& options) = 0;
    /**
     * @brief 只读预测单个未来 bucket。
     * @param series_key 序列标识。
     * @param bucket_id 待预测 bucket，必须大于该序列最后观测 bucket。
     * @return rolling 预测结果。
     */
    virtual RollingPrediction PredictRolling(
        std::string_view series_key,
        int64_t bucket_id) const = 0;
    /**
     * @brief 只读预测连续 bucket 区间。
     * @param series_key 序列标识。
     * @param start_bucket_id 起始 bucket。
     * @param point_count 预测点数，必须大于 0。
     * @return rolling 预测序列，区间为 [start_bucket_id, start_bucket_id + point_count)。
     */
    virtual RollingPredictionSequence PredictRolling(
        std::string_view series_key,
        int64_t start_bucket_id,
        uint32_t point_count) const = 0;

    /**
     * @brief 训练或替换指定序列的 bootstrap artifact。
     * @param input 训练输入；当 force_replace_existing_artifact 为 false 且 artifact 已存在时拒绝替换。
     * @return 训练结果。
     */
    virtual BootstrapTrainResult Bootstrap(const RatioBootstrapInput& input) = 0;
    /**
     * @brief 使用已训练 bootstrap artifact 只读预测单个 bucket。
     * @param series_key 序列标识。
     * @param bucket_id 待预测 bucket。
     * @param options 预测选项。
     * @return bootstrap 预测结果。
     */
    virtual BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const = 0;
    /**
     * @brief 使用已训练 bootstrap artifact 只读预测连续 bucket 区间。
     * @param series_key 序列标识。
     * @param start_bucket_id 起始 bucket。
     * @param point_count 预测点数，必须大于 0。
     * @param options 预测选项。
     * @return bootstrap 预测序列，区间为 [start_bucket_id, start_bucket_id + point_count)。
     */
    virtual BootstrapPredictionSequence PredictBootstrap(
        std::string_view series_key,
        int64_t start_bucket_id,
        uint32_t point_count,
        const BootstrapPredictionOptions& options) const = 0;
    /**
     * @brief 导出当前任务持有的所有 bootstrap artifact。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    /**
     * @brief 加载 bootstrap artifact，并重建兼容的 rolling 预热状态。
     * @param content 序列化内容。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 加载状态。
     */
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    /**
     * @brief 导出由当前 artifact 派生的 bootstrap seed。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
};

/**
 * @brief 关系型 Baseline 任务接口，继承 IBaselineTask 的同任务非并发调用契约。
 */
interface IBaselineRelationTask : public IBaselineTask {
    /**
     * @brief 提交一条关系观测并推进 routed rolling、stream basis 与 relation fusion 状态。
     * @param obs 关系观测。
     * @param options 关系 rolling 提交选项。
     * @return 关系 rolling 更新结果。
     */
    virtual BaselineRelationEvaluationResult SubmitObservation(const RelationRollingObservation& obs,
                                                               const RelationRollingSubmitOptions& options) = 0;
    /**
     * @brief 只读预测一个 routed summary 的未来 bucket。
     * @param query routed summary 查询条件；basis-scoped summary 中 basis_version 为 0 表示使用当前 active basis。
     * @param bucket_id 待预测 bucket。
     * @return rolling 预测结果。
     */
    virtual RollingPrediction PredictRoutedSummary(
        const RelationRoutedSummaryQuery& query,
        int64_t bucket_id) const = 0;
    /**
     * @brief 查询一个 routed summary 的诊断快照。
     * @param query routed summary 查询条件。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与快照内容。
     */
    virtual BaselineSerializationResult QueryRoutedSummarySnapshot(
        const RelationRoutedSummaryQuery& query,
        BaselineSerializationFormat format) const = 0;

    /**
     * @brief 训练或替换关系型 bootstrap artifact。
     * @param input 训练输入；产物包括 relation basis 与 routed summary seed。
     * @return 训练结果。
     */
    virtual BootstrapTrainResult Bootstrap(const RelationBootstrapInput& input) = 0;
    /**
     * @brief 导出当前任务持有的所有关系型 bootstrap artifact。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const = 0;
    /**
     * @brief 加载关系型 bootstrap artifact，并重建关系运行时状态。
     * @param content 序列化内容。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 加载状态。
     */
    virtual BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) = 0;
    /**
     * @brief 导出由当前关系型 artifact 派生的 bootstrap seed。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const = 0;
    /**
     * @brief 查询当前可用的 relation basis artifact。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与序列化内容。
     */
    virtual BaselineSerializationResult QueryBootstrapBasis(
        BaselineSerializationFormat format) const = 0;
};

/**
 * @brief Baseline 服务接口，负责创建任务并提供服务级状态查询。
 */
interface IBaselineService {
    virtual ~IBaselineService() = default;

    /**
     * @brief 根据序列化配置创建数值型 Baseline 任务。
     * @param config_content 任务配置内容。
     * @param format 配置序列化格式；当前仅支持 JSON。
     * @return 创建状态与任务接口指针。
     */
    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>>
    CreateValueTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    /**
     * @brief 根据序列化配置创建比例型 Baseline 任务。
     * @param config_content 任务配置内容。
     * @param format 配置序列化格式；当前仅支持 JSON。
     * @return 创建状态与任务接口指针。
     */
    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>>
    CreateRatioTask(std::string_view config_content,
                    BaselineSerializationFormat format) = 0;

    /**
     * @brief 根据序列化配置创建关系型 Baseline 任务。
     * @param config_content 任务配置内容。
     * @param format 配置序列化格式；当前仅支持 JSON。
     * @return 创建状态与任务接口指针。
     */
    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>>
    CreateRelationTask(std::string_view config_content,
                       BaselineSerializationFormat format) = 0;

    /**
     * @brief 查询服务级快照。
     * @param format 序列化格式；当前仅支持 JSON。
     * @return 状态码与快照内容，包含任务注册表状态。
     */
    virtual BaselineSerializationResult QueryServiceSnapshot(
        BaselineSerializationFormat format) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_SERVICE_H_
