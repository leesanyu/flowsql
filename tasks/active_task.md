# Active Task

Feature：C++ 算子源码目录归位（`operator-source-layout`）
原子任务：T0 将 `npm_basic` 迁入统一算子目录
状态：已完成

## 业务意图

- 建立 `src/operators/` 作为可独立加载的 C++ 业务算子源码根目录。
- 将 `npm_basic` 整体迁至 `src/operators/npm_basic/`，消除它与 NPI、Baseline 等非算子插件混放的问题。
- 只改变源码所有权和构建引用，不改变运行时行为或部署契约。

## Non-Goals

- 不移动或重构 `src/framework/builtin/` 中的内置算子。
- 不移动 Bridge、BinAddon 的算子代理，不调整 Python Worker 目录。
- 不修改 `npm.basic` 算法、ABI、SQL 名称、IID、插件生命周期或部署配置。
- 不实施任何待办 NPM Feature，不 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/product_backlog.md`
- `tasks/specs/feat-operator-source-layout.md`
- `tasks/archive/feat-operator-source-layout.md`
- `src/CMakeLists.txt`
- `src/operators/CMakeLists.txt`
- `src/operators/npm_basic/**`
- `src/plugins/npm_basic/**`（仅整体迁出并删除旧路径）
- `src/tests/test_npm_basic/CMakeLists.txt`
- `src/tests/test_npm_basic/test_npm_basic.cpp`

其余累计差异只保留、不扩改。

## 冻结契约

- `src/operators/` 只承接可独立加载的 C++ 业务算子；本次仅迁移 `npm_basic`。
- `src/framework/builtin/`、服务适配器、算子接口、NPI 和 Baseline 的源码位置保持不变。
- CMake target `flowsql_npm_basic`、产物 `libflowsql_npm_basic.so`、`flowsql::npm` 命名空间、
  `IID_BLOCK_TRANSFORM_OPERATOR_V1` 和 `IPlugin` 批次生命周期保持不变。
- 部署文件不改；现有 Scheduler 和部署契约继续按同一动态库名加载。
- 迁移保留所有已修改及未跟踪的 `npm_basic` 文件内容，不做功能性改写。

## 验收标准与命令

- `cmake -B build src`
- `cmake --build build --target flowsql_npm_basic test_npm_basic test_scheduler_e2e test_native_deploy_config test_docker_deploy_config -j$(nproc)`
- `ctest --test-dir build -R '^(test_npm_basic|test_scheduler_e2e|test_native_deploy_config|test_docker_deploy_config)$' --output-on-failure`
- `cmake --build build -j$(nproc)`
- `ctest --test-dir build --output-on-failure`
- `rg -n 'plugins/npm_basic' src` 无残留。
- `test ! -e src/plugins/npm_basic && test -d src/operators/npm_basic`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。
- 迁移、定向测试、完整构建和全部 CTest 通过后，勾选 T0、完成并归档本 Feature，然后立即停止。
- 若失败需要修改允许清单外的生产代码，只记录证据并停止，不扩大任务边界。

## 执行前基线

- `npm-basic-analysis` 已完成并归档；当前累计实现尚未提交，迁移必须完整保留这些差异。
- `npm_basic` 当前位于 `src/plugins/npm_basic/`，是生产代码中唯一实现
  `IBlockTransformOperatorV1` 的独立加载算子 provider。
- `src/framework/builtin/` 中的内置算子继续由 `BuiltinRegistry` 注册，本任务不调整其目录。

## 完成证据

- 已建立 `src/operators/`，`npm_basic` 的 30 个源码/构建文件已完整迁至
  `src/operators/npm_basic/`；`src/plugins/npm_basic` 已不存在。
- CMake、测试源文件和 include 均使用新路径；`src` 中无 `plugins/npm_basic` 或旧头文件保护宏残留。
- `src/framework/builtin/` 的 Git Diff 为空；target、动态库名、IID、命名空间和部署配置保持不变。
- CMake 配置与定向 target 构建通过；定向 CTest 4/4 通过，总耗时 22.20 秒。
- 完整构建通过；在允许 loopback socket 的环境运行完整 CTest 13/13 通过，总耗时 50.70 秒。
- 受限沙箱中的两项首次失败均为创建 loopback socket 失败；沙箱外复跑通过，未修改产品代码。
- `git diff --check`、旧路径检查、动态库产物检查和目录边界检查通过。
