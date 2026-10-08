# 即时工作台

关联 Feature：[Baseline 检视修复](archive/feat-baseline-review-fixes.md)，T1～T5 已验收完成。
当前 Atomic Slice：baseline 提交前规范与范围验收；状态：已完成，WIP=0。
用户已明确要求“提交代码”，本轮授权本地 Git 提交。

## 业务意图与 Non-Goals

将已验收的 B01～B11 修复、回归、文档、完成记录及后续算子规格登记保存为可追溯的本地提交。
不实施封装算子、不改算法或接口、不推送；Backlog 的 AF_XDP 和 DPDK 采集通道原有描述改动保留在工作区。

## 允许修改与暂存范围

- src/tests/test_baseline/test_baseline_relation_fusion.cpp：仅将旧版权块替换为统一两行注释，代码保持。
- tasks/active_task.md：本轮范围、验证和提交准备记录。
- 暂存清单冻结为 /tmp/baseline-commit/scope-paths.json 的 43 个已核实路径：baseline 插件/测试、独立状态控制头、README、工作台、归档规格、后续算子规格及 Backlog。
- tasks/product_backlog.md 仅暂存 baseline-review-fixes 和 baseline-operator 两项新增记录；用独立 staged 文本和 patch 保留另两项用户改动，工作文件不覆盖。
- /tmp/baseline-commit/**：范围、hash、提交消息、局部补充验收及暂存审计。

每次 patch 后 git diff --name-only，按 before_hashes.json 检查本轮文件内容修改仅允许上述测试注释和工作台。

## 验收命令与步骤

1. 已确认所有提交源码与 T5 全量构建及 61/61 CTest 的版本 hashes 一致；git diff --check 通过。
2. 版权补齐后 clang-format-18 --dry-run --Werror --lines=1:2 检查，构建 test_baseline_relation_fusion 并运行对应 CTest。
3. 精确 git add 暂存 42 个完整文件，再 git apply --cached 只加入 Backlog 两项记录。
4. git diff --cached --check 与 staged 路径/hash/Backlog 文本断言；审核 staged diff 和统计。
5. 验收/暂存准备结束置 WIP=0，执行用户授权 git commit；核对新提交内容、暂存区清空及剩余 Backlog 改动。

## 时间盒与停止条件

2026-10-08 06:55～07:15 UTC，20 分钟；提交失败自主核查，只有明确不可恢复阻塞才中止。
本地提交成功、内容核对通过即停止，不推送。源码未发生语义变化，不重复已通过的全量验收。

## 检查点

43 个路径范围已冻结；现有暂存区为空，源码与 T5 验收 hash 全部吻合。唯一遗漏的版权声明位于 fusion 回归测试，补齐后局部验证。
提交准备已完成：37 个 C++ 文件版权声明符合规范；fusion 仅头部注释变化，格式检查和目标构建通过，对应 CTest 1/1、8 PASS。其余源码与 T5 全量构建及 61/61 CTest 的版本保持。
43 个暂存路径及完整文件内容已逐一核对，Backlog 只暂存两个 baseline 记录，另两项采集通道描述保留未暂存；staged diff --check 通过。用户已授权执行本地提交，实际提交结果以 Git 记录为准；不推送。
