# 即时工作台

Feature：`npm-basic-operator-plugin-lifecycle`（NPM 基础分析算子插件生命周期）

原子任务：T4.4 隔离环境真实插件生命周期 E2E

状态：已完成

## 业务意图

- 让 Scheduler E2E 不再静态加载 `libflowsql_npm_basic.so`，而是经 Catalog/BinAddon API 上传并激活真实
  V2 构建产物，再执行既有 PCAP → `npm.basic` → 22 列 DataFrame 主链路。
- 用同一隔离 API 链验证坏记录删除重传、激活详情、租约占用时 409、释放后去激活，以及激活/去激活状态的
  重启恢复差异。
- 插件详情以 operator-level 诊断项报告 `npm.basic` 的 `contract=block_transform_v1`，保持插件、功能算子、
  数据契约三层语义。

## Non-Goals

- 不修改 `npm.basic` 算法、V2 ABI、Scheduler 执行语义、部署配置或前端。
- 不新增插件级单一 `contract`；同一 V2 插件可导出不同契约，详情必须逐算子表达。
- 不直接编辑 SQLite，不操作用户当前运行实例或其真实 `broken` 数据；E2E 使用临时目录和同一 API 语义。
- 不增加 `T4.4.1` 等更深规格编号，不实施其他 Feature，不 commit/push。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-basic-operator-plugin-lifecycle.md`
- `tasks/archive/feat-npm-basic-operator-plugin-lifecycle.md`（Feature 全绿后的归档目标）
- `tasks/product_backlog.md`（Feature 全绿后的状态与链接收尾）
- `src/services/binaddon/binaddon_host_plugin.cpp`
- `src/tests/test_scheduler_e2e/CMakeLists.txt`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`

其余累计差异只保留，不扩改。若验收揭示公共 ABI 或其他生产模块存在新的 P0/P1 阻塞，记录证据并停止。

## 契约与测试锚点

- E2E 启动时 `npm.basic` 不可用，且不消费或删除 PCAP 源；坏 `.so` 经上传和激活进入 `broken`，再经删除
  API 清理后才允许用同一文件名上传真实构建产物。
- 激活响应和详情报告 V2、`operator_count=1`、`operators=["npm.basic"]`；详情的 `operator_details`
  包含 `{name:"npm.basic", contract:"block_transform_v1"}`。
- 激活后离线 SQL 产生一行固定 22 列结果；持有动态 capability lease 时去激活返回 409，释放后成功。
- 重新激活后执行 BinAddon `Stop/Start` 可恢复 `npm.basic`；去激活后再 `Stop/Start` 不恢复。
- 去激活后 SQL 再次明确不可用；删除插件后隔离目录不残留插件状态或文件。

## 验收命令

```bash
cmake -B build src
cmake --build build --target flowsql_binaddon flowsql_npm_basic test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^test_scheduler_e2e$' --output-on-failure
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

## 时间盒

30 分钟。

## 停止条件

- 隔离 API 生命周期、真实 SQL、详情契约和恢复语义全部通过后，勾选 T4.4；随后执行 Feature 级完整构建与
  全部 CTest，只有全绿才完成 T4/Feature 和归档。
- 若定向 E2E 未通过，只修复当前允许文件内的问题；需要扩大边界时记录阻塞证据并停止。

## 完成证据

- 定向目标构建通过；`test_scheduler_e2e` 通过，覆盖坏插件删除重传、真实 V2 激活、SQL、租约 409、
  去激活和重启恢复。
- Feature 定向验收 5/5 通过；完整构建通过；具备 loopback 权限的完整 CTest 13/13 通过。
- `git diff --check` 通过；未启动后续 Feature，未 commit/push。
