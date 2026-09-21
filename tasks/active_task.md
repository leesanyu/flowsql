# 即时工作台

事项：流量标签化 T3.3 规则容量画像与完整回归
关联 Feature Task：`tasks/archive/feat-flow-labeling.md` T3.3；T3.1/T3.2/T3.3 与父任务 T3 已完成。
当前 Atomic Slice：使用现有真实插件 benchmark 完成 1K/10K/50K 逻辑规则的构建时间、RSS、物理规则数和分类吞吐
画像，并复核 50K 在 64 MiB 及可调预算档位下的资源诊断与完成边界；实测确认进程级 EAL 默认 64 MiB 堆使
128 MiB 任务档位无法落地，本切片同时修复该 P1 阻塞。
状态：已完成

## 业务意图

- 为 T3 提供可复核的 1K/10K/50K 真实 matcher 资源画像，明确 50K 是否能在各 matcher 预算档位完成构建，
  并验证失败时的结构化资源诊断。
- 保持 T3.1/T3.2 已交付的 32-field 无损布局、任务级预算参数和两倍总跟踪预算准入不变。

## Non-Goals

- 不修改 Flow Labeling 公共 ABI、FlowLabelingSet YAML、DPDK 规则布局、matcher 编译/分类逻辑或配置模板。
- 不建立节点级多任务总 matcher 上限，不改变 `max_logical_rules/max_compiled_rules`，不引入多 context。
- 不把当前机器的时间/RSS/吞吐观测冒充跨机器硬保证；不在 benchmark 中增加新的容量优化实现。
- 不修改 T3.1/T3.2 已完成代码，不自动进入 Feature 之外的容量产品承诺。
- 不整理或覆盖工作区内其他既有未提交差异，不 commit/push。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-flow-labeling.md`
- `tasks/archive/feat-flow-labeling.md`
- `tasks/product_backlog.md`
- `src/plugins/flow_labeling/flow_labeling_plugin.cpp`
- `src/tests/test_flow_labeling/benchmark_flow_labeling.cpp`
- `src/tests/test_flow_labeling/test_flow_labeling.cpp`

## 验收命令

```bash
cmake -B build src -DFLOWSQL_FLOW_LABELING=ON
cmake --build build --target test_flow_labeling benchmark_flow_labeling -j$(nproc)
ctest --test-dir build -R '^test_flow_labeling$' --output-on-failure
build/output/benchmark_flow_labeling build/output/libflowsql_flow_labeling.so
clang-format --dry-run --Werror src/tests/test_flow_labeling/benchmark_flow_labeling.cpp \
  src/tests/test_flow_labeling/test_flow_labeling.cpp src/plugins/flow_labeling/flow_labeling_plugin.cpp
git diff --check
git diff --name-only
git status --short
```

## 时间盒与停止条件

- 时间盒：30 分钟；若未完成，沿用 T3.3 到可复核的 benchmark 检查点，不创建第三层编号或扩大允许文件。
- 停止条件：真实插件 E2E 定向测试通过；benchmark 对 1K/10K/50K 记录 physical rules、配置大小、构建时间、
  构建 RSS、峰值 RSS 和分类吞吐；至少记录 50K 在 64 MiB 及可行更高档位的成功/失败诊断；完整回归通过后
  更新 T3 完成证据并停止，不进行新的容量优化实现。

## 已确认问题与最小修改方案

- 复现：50K/64 MiB 在稳态预算检查返回 `kBudgetExceeded`，符合预期；50K/128 MiB 通过该检查后，DPDK 输出
  `ACL: allocation of 25218672 bytes ... failed`，并在 `/spec/engine/max_runtime_bytes` 返回构建失败。进程有 20 GiB
  可用内存且无 cgroup OOM，根因是 `InitializeEal()` 未指定 `-m`，无巨页模式采用 DPDK 默认 64 MiB 预留。
- 最小修改：仅在私有 EAL 启动参数中显式预留 512 MiB，覆盖单任务最高 256 MiB matcher 档位及 DPDK 构建期工作区；
  不改变公共 ABI、任务预算准入、节点级调度或 matcher 规则表达。风险是启用插件的进程虚拟/实际内存预留增加，
  由真实 50K build/classify、生命周期测试和全量回归验证。

## 完成证据

- 容量 benchmark 使用方向非对称规则形成真实 2× 物理展开；最终矩阵验证 1K/10K 在 64 MiB 成功、50K/64
  返回结构化预算诊断、50K/128 实际构建 100K 物理规则并完成正反向、未命中和批量吞吐分类。
- 发现并修复任务 128/256 MiB 档位受 EAL 默认 64 MiB 堆阻塞的问题：私有 EAL 显式预留 512 MiB，未改变公共
  ABI、任务预算或 matcher 语义。六档保守代表点全部实测成功：8→1K、16→5K、32→10K、64→20K、128→50K、
  256→50K；建议值不作为任意规则组合硬上限。
- 最终全量构建成功；完整 CTest 16/16、0 失败，总耗时 53.30 秒。DPDK 23.11.4 动态依赖隔离复核、clang-format
  dry-run 和 `git diff --check` 通过。T3/T3.3 已勾选，Feature 与 Backlog 已完成并归档；未 commit/push。
