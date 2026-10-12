# 即时工作台

WIP=0；npm-ip-family-pipeline / npm-ip-family-throughput 提交收口，Atomic Slice 1：已完成。

- 用户授权：提交代码；完成当前分族实现与新增优化规格的本地提交。
- 关联：npm-ip-family-pipeline T1～T5 已验收并归档；npm-ip-family-throughput 规格已完成，T1～T5 未启动。
- 业务意图：将已验收的分族链路与后续优化方案分别固化为可审查的版本，确保提交内容对应实际验收对象。
- Non-Goals：不实施优化、不修改生产代码或测试、不重新扩展验收范围、不推送、不整理其他任务。
- 当前切片：核对各阶段范围和哈希、精确暂存、审查差异、创建两个本地提交并核对最终工作区。
- 允许修改文件：tasks/active_task.md；tasks/product_backlog.md 仅拆分两个提交的状态与新增行，最终恢复本轮起点内容。
- 允许暂存文件：/tmp/npm-ip-family-submit/pipeline-paths.json 与 planning-paths.json 的逐项清单；共覆盖起点 160 个路径。
- 提交划分：先提交分族实现、测试、成本报告和归档；再提交优化分析、规格、Backlog 新行及工作台收口。
- 起点：/tmp/npm-ip-family-submit/start.json；HEAD=e577023668aaa6be184c843efc06ec8fb29b303d，暂存区为空。
- 验收命令：git diff --check；git diff --cached --check / --name-status；逐文件 SHA256 对照；JUnit 覆盖核验；git show --stat / --format=fuller；git status --short。
- 时间盒：30 分钟，自 start.json 的 started_at 起算。
- 停止条件：两个提交及提交树/最终哈希核对通过后 WIP=0 并停止；若出现范围或哈希漂移，停在有证据的检查点。

## 已核对的验收基础

- 160 个当前改动路径均可追溯到上述两项交付，未发现额外无关改动。
- 当前 1,017 项源码/文档哈希和 T5 的 45 项产物哈希与验收记录一致；本轮未改源码，未重跑历史测试。
- 分族 Feature：标准全量构建通过；完整 CTest 90/90 唯一测试；ASan/UBSan/LSan 24 个唯一测试及三远程后端通过。
- 三万会话 16 场景通过；59 个严格编译单元与 142 个 C++ 格式/版权检查通过；真实四后端与原生驱动验收通过。
- 优化 Feature：仅分析与规格完成，所有实现任务未勾选；性能指标为后续验收目标。

## 提交收口证据

- 分族实现本地提交：0e339be094ff3963fa8cfd895fae487546a4afe7；158 个路径的提交树与逐项暂存清单及内容哈希一致。
- 完整 CTest 的最终 JUnit 81/81 和 10/10 去重覆盖当前全部 90 个登记测试，无失败或跳过；Sanitizer 24 项及大容量重验记录核对通过。
- 本轮重新执行 142 个 C++ 的 clang-format/版权检查和 git diff --check；已有验收对象哈希未变，本轮没有重跑实现测试。
- 新优化规格与分析按四文档清单独立提交；Backlog 最终内容恢复起点哈希，T1～T5 及全部子任务仍待实施。
- 提交范围、JUnit 覆盖、格式、暂存内容与最终提交/工作区审计：/tmp/npm-ip-family-submit/。
- 本轮只修改工作台与 Backlog 的提交拆分内容；源码、测试、成本报告及两份优化文档保持已验收哈希；不推送。
- 当前切片完成后停止；未启动 npm-ip-family-throughput T1。
