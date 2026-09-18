# 即时工作台

事项：`config-channel` T4 端到端验收与 Feature 收口
关联 Feature Task：`tasks/archive/feat-config-channel.md` T4；T1～T4 已完成。
当前 Atomic Slice（第二个）：真实 HTTP 控制链路与双进程重启验收，随后完整回归并归档 Feature。
状态：已完成

## 业务意图与接口契约

- 以可执行测试证明并发发布只产生一个下一 revision、全部不可变历史在重启后逐项一致、消费者持有的精确快照
  不随新发布或 Provider 生命周期变化。
- 原生单进程、Guardian 与 Docker Scheduler 都必须加载 Config Provider，使用被部署持久卷覆盖的 SQLite 路径，
  并在 Scheduler 启动前完成接口注册；完整 CTest 与前端回归共同证明现有通道和页面无回归。
- 全部验收通过后勾选 T4，将规格移入 `tasks/archive/`，并把 Backlog Feature 标记完成。

## Non-Goals

- 不实现 Application Scope、NPM 参数消费或任何具体业务 Schema；不新增逐包 Resolve、HTTP 或 SQLite 访问。
- 不新增配置删除、重命名、`latest`、热更新、密钥管理或跨集群复制。
- 不顺手修改现有 NPM、Baseline 或其他工作树内容；不 commit/push。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-config-channel.md`
- `tasks/archive/feat-config-channel.md`
- `tasks/product_backlog.md`
- `README.md`
- `src/tests/test_config_channel/test_config_channel.cpp`
- `src/tests/test_framework/test_native_deploy_config.cpp`
- `src/tests/test_framework/test_docker_deploy_config.cpp`
- `src/tests/test_framework/CMakeLists.txt`
- `src/services/web/web_plugin.cpp`
- `src/tests/test_config_channel/test_config_channel_e2e.cpp`
- `src/tests/test_config_channel/CMakeLists.txt`

其余已有工作树修改保持原样；每次 patch 后核对 `git diff --name-only` 和新增文件状态。

## 测试锚点与验收命令

- `cmake --build build --target test_config_channel_e2e -j$(nproc)`
- `ctest --test-dir build -R '^test_config_channel_e2e$' --output-on-failure`
- `node --test src/frontend/src/api/*.test.js src/frontend/src/utils/*.test.js`
- `npm run build --prefix src/frontend`
- `cmake --build build -j$(nproc)`
- `ctest --test-dir build --output-on-failure`
- 本 Feature 文件 `git diff --check`、版权头、部署文件与归档/Backlog 状态检查。

## 时间盒与停止条件

30 分钟。T4 全部断言、完整构建/CTest、前端回归及归档通过后标记 Feature 完成并停止；不进入后续 Feature。
到期仅报告：已完成、进行中且检查点通过、当前错误待修复、被明确问题阻塞。

## 第一切片检查点

- 重启前后 5 个 revision 的身份、格式、Schema、摘要、长度、时间与内容逐项一致，current 与历史列表恢复。
- Config/原生/Docker 定向 CTest 3/3；前端测试文件 4/4、生产构建和完整 CMake 构建通过。
- 完整 CTest 首轮 13/14：既有 T47 在 Web Load 后直接调用路由，gateway 选项此前只在 Start 下发；
  修复 Load 同步运行边界后，Scheduler/Web 契约定向 CTest 2/2 通过。尚不勾选 T4。

## 第二切片检查点与完成证据

- 删除 E2E 草稿死代码并补充独立 HTTP 客户端状态采集；`test_config_channel_e2e` 构建及定向 CTest 1/1 通过。
- 真实 HTTP 链路覆盖并发 200/409、幂等、失败不耗版本、JSON/YAML/XML、512 KiB 原文与 1 MiB 控制请求边界、
  精确引用错误、分页元数据和基于旧版恢复；seed/recover 独立进程逐字比较重启前后 list/history/resolve 响应。
- 任务在打开阶段只 Resolve 一次并持有 `shared_ptr<const string>`；后续发布、Provider 卸载和 1000 次事件处理均未改变
  旧快照，也未再次 Resolve。
- `cmake --build build -j$(nproc)` 通过；完整 `ctest --test-dir build --output-on-failure` 15/15 通过；前端测试 4/4
  通过，`npm run build --prefix src/frontend` 通过。原生/Docker Config 部署契约和 Scheduler/Web 定向回归通过。

## 完成出口

T1～T4 已完成，规格已归档至 `tasks/archive/feat-config-channel.md`，Backlog 已标记 `[x]`；本工作台停止，未
提交或推送代码。
