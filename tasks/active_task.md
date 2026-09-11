# Active Task

Feature：PCAP 文件通道持久化（`pcapfile-channel-persistence`）
原子任务：T3.3 执行全量回归、Feature Diff 审查与归档收口
状态：已完成

## 业务意图

- 用标准 CMake 入口重新配置并全量构建，执行完整 CTest 和前端生产构建，确认 T0～T3.2 的累积实现满足
  Feature DoD 且没有破坏既有功能。
- 只审查本 Feature 的完整未提交 Diff；全部验收通过后完成 T3/T3.3、backlog 状态和规格归档，形成可提交状态。

## Non-Goals

- 不新增功能、测试场景、接口、配置项、依赖或文档章节。
- 不修改已经通过原子验收的生产代码、测试、配置和 README；若全量回归发现 P0/P1 阻塞缺陷，先记录证据并
  重新切分允许修改文件，不在收口任务中顺手修复。
- 不处理与本 Feature 无关的历史失败、代码风味或低优先级建议。
- 不读取或修改 `tasks/sprints/**`，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-pcapfile-channel-persistence.md`
- `tasks/archive/feat-pcapfile-channel-persistence.md`

Feature 的生产代码、CMake、部署配置、README 和测试 diff 仅做只读审查，不修改。

## 验收标准与命令

- `npm run build`（工作目录 `src/frontend`）通过，且不产生需提交的前端源代码或构建资产 diff。
- `cmake -B build src`
- `cmake --build build -j$(nproc)`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`
- 完整 Diff 仅包含本 Feature 的持久层、插件生命周期、部署路径、README、测试和任务文档；无越界改动、死代码
  或未经测试支撑的抽象。
- 所有验收通过后，T3/T3.3 和 Feature 状态标记完成，backlog 改为完成并链接归档规格，规格移入 `tasks/archive/`。
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。
- 全量构建、完整 CTest、前端构建和 Diff 审查全部通过后完成归档并立即停止，不执行 commit/push。
- 若回归失败，状态只能记录为“当前错误待修复”或“被明确问题阻塞”；不得扩大文件范围或归档未通过的 Feature。

## 完成证据

- `npm run build`（`src/frontend`）通过，Vite 完成 1522 个模块的生产构建；仅有既有的大 chunk 提示，未产生新的
  受版本控制前端差异。
- `cmake -B build src` 使用标准入口配置成功；`cmake --build build -j$(nproc)` 全量构建到 100%，所有 target
  成功生成。挂载文件系统报告小于 0.2 秒的 `Clock skew` 提示，但没有编译错误或缺失 target。
- `ctest --test-dir build --output-on-failure` 在允许本地 loopback socket 的执行环境中运行：12/12 通过，0 失败，
  总耗时 46.43 秒。
- Feature 完整 Diff 已审查：SQLite 动态值均使用绑定参数，持久写入/恢复/删除顺序符合规格，原生、Guardian 和
  Docker 路径一致，真实 Web E2E 覆盖重载恢复、重复 SQL、脱敏和删除后不恢复；无新增公共 ABI 或越界实现。
- `git diff --check` 通过，新增 C++ 行均不超过 120 列；环境未提供 `clang-format` 可执行文件。
- T3/T3.3 和 Feature 状态已完成，backlog 已链接归档规格，规格已移入 `tasks/archive/`；未读取或修改
  `tasks/sprints/**`，未执行 commit 或 push。
