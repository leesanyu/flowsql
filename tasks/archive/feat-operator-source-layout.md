# Feature: C++ 算子源码目录归位

状态：`[x]` 已完成
优先级：P1
前置 Feature：`npm-basic-analysis`（已完成）

## 业务意图

建立 `src/operators/` 作为可独立加载的 C++ 业务算子源码根目录，将 `npm_basic` 从通用
`src/plugins/` 分类中整体迁入该目录。迁移只表达源码所有权，不改变动态加载、算子执行或部署行为。

主链路保持不变：Scheduler 按 `IID_BLOCK_TRANSFORM_OPERATOR_V1` 发现 `npm.basic` provider，创建任务并处理
packet block；构建仍生成 `libflowsql_npm_basic.so`，部署仍通过 `IPlugin` 加载该动态库。

## Non-Goals

- 不调整 `src/framework/builtin/`；内置算子继续由 `BuiltinRegistry` 注册。
- 不移动 Bridge、BinAddon 的算子代理，不调整 Python Worker 目录。
- 不移动算子接口、Scheduler 执行器、NPI、Baseline、Channel 或测试 Mock。
- 不改变 `npm.basic` 算法、Schema、ABI、SQL 名称、IID、生命周期、动态库名或部署配置。
- 不借目录迁移实施任何其他 NPM Feature 或代码重构。

## 目录与契约

```text
src/
├── operators/
│   ├── CMakeLists.txt
│   └── npm_basic/
├── framework/builtin/     # 保持不变
├── plugins/npi/           # 非算子能力插件，保持不变
└── plugins/baseline/      # 非算子能力插件，保持不变
```

- `src/operators/` 收纳可独立加载、向执行框架暴露算子能力的 C++ 业务实现。
- `npm_basic` 的源码、CMake 和 `plugin_register.cpp` 原子迁移，不拆分领域实现与 provider。
- `flowsql_npm_basic` target、`libflowsql_npm_basic.so`、`flowsql::npm` 命名空间、
  `IID_BLOCK_TRANSFORM_OPERATOR_V1` 和 IPlugin 批次生命周期保持不变。
- 头文件 include 路径随源码位置改为 `operators/npm_basic/...`；旧路径不得保留兼容转发层。

## 原子任务

- `[x]` T0 将 `npm_basic` 迁入 `src/operators/`，更新构建和测试引用，完成定向回归并清除旧路径。

## Feature 验收

- `cmake -B build src`
- 构建 `flowsql_npm_basic`、`test_npm_basic`、Scheduler E2E 和部署契约测试 target。
- `test_npm_basic`、Scheduler E2E 和原生/Docker 部署契约测试全部通过。
- 完整构建和全部 CTest 通过。
- `src/plugins/npm_basic` 不存在，`src` 中不存在 `plugins/npm_basic` 引用。
- `src/framework/builtin/` 内容和引用未被本 Feature 修改。
