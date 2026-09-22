# 即时工作台

事项：`npm.basic` 参数共享配置命名空间从 `parameters.framework` 改为 `parameters.core`
关联 Feature：已完成的 `npm-basic-parameters` 与 `npm-flow-labeling-refinement` 的用户请求契约修正；不新增 Feature Task。
当前 Atomic Slice：先以单元/E2E 断言冻结 `core` 唯一命名与旧 `framework` 明确失败语义，再同步解析器、内部拥有型
类型、任务接线、错误路径及现行文档，完成定向和全量回归后停止。
状态：已完成

## 业务意图

- 让 `npm.basic` 的参数命名与源码职责一致：基础流程位于 `core/`，对应任务共享配置统一写在
  `parameters.core`，避免 `framework` 被误解为 FlowSQL 全局框架配置。
- 让旧拼写产生明确、可诊断的配置错误，避免被前向兼容的未知模块规则静默忽略并退回默认配置。

## Non-Goals

- 不改变 `core` 内字段名、默认值、范围、legacy `WITH` 参数、模块参数、条件消费、Config Channel Resolve、
  matcher、会话或结果语义。
- 不保留 `framework` 兼容别名，不自动迁移运行中的外部任务，不提升 `schema_version`。
- 不改 `src/framework/`、`framework.*` C++ 框架 include、插件 ABI、目录布局或历史 sprint 文档。
- 不修改无关既有差异，不 push；用户已在验收阶段明确要求提交当前已完成改动。

## 冻结接口与测试契约

- V1 JSON 信封的唯一共享配置键是可省略 object `core`；内部拥有型类型同步为 `NpmCoreParametersV1`，成员名为
  `core`，避免配置名与实现名再次分叉。
- `core` 继续严格校验共享字段；labeling 未启用/不可用时仍忽略其 `labeling` 与
  `labeling_memory_mib` 业务值，启用且可用时严格校验并只 Resolve 一次。
- 根级 `framework` 不作为未知未来模块接受，统一返回 `kUnknownConsumedField`，路径 `/framework`；同时出现
  `core` 与 `framework` 也按 `/framework` 失败。所有共享字段诊断路径改为 `/core/...`。
- 测试先锚定：`core` 正常解析和任务/E2E 传递、旧 `framework` 明确拒绝、重复/类型/范围/labeling 错误路径
  均使用 `/core`，失败仍保持输出原子性。

## 允许修改文件

- `tasks/active_task.md`
- `src/operators/npm_basic/config/npm_parameters.h`
- `src/operators/npm_basic/config/npm_parameters.cpp`
- `src/operators/npm_basic/config/npm_basic_task_config.cpp`
- `src/operators/npm_basic/npm_basic_operator.cpp`
- `src/tests/test_npm_basic/test_npm_basic.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`
- `src/tests/test_config_channel/test_config_channel_e2e.cpp`
- `docs/flow-labeling.md`
- `tasks/archive/feat-npm-basic-parameters.md`
- `tasks/archive/feat-npm-flow-labeling-refinement.md`
- `tasks/product_backlog.md`

## 验收命令

```bash
cmake --build build --target test_npm_basic test_scheduler_e2e test_config_channel_e2e -j$(nproc)
ctest --test-dir build -R '^(test_npm_basic|test_scheduler_e2e|test_config_channel_e2e)$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
rg -n 'parameters\.framework|framework\.labeling|NpmFrameworkParametersV1|parsed_parameters\.framework|/framework' \
  src/operators/npm_basic src/tests/test_npm_basic src/tests/test_scheduler_e2e docs/flow-labeling.md \
  tasks/archive/feat-npm-basic-parameters.md tasks/archive/feat-npm-flow-labeling-refinement.md tasks/product_backlog.md
clang-format-18 --dry-run --Werror \
  src/operators/npm_basic/config/npm_parameters.h src/operators/npm_basic/config/npm_parameters.cpp \
  src/operators/npm_basic/config/npm_basic_task_config.cpp src/operators/npm_basic/npm_basic_operator.cpp \
  src/tests/test_npm_basic/test_npm_basic.cpp
git diff --check -- src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp \
  src/tests/test_config_channel/test_config_channel_e2e.cpp
git diff --check
git diff --name-only
git status --short --untracked-files=all
```

## 时间盒与停止条件

- 时间盒：30 分钟；未完成则沿用本切片到可验证或有明确错误证据的检查点，不创建第三层任务。
- 停止条件：`core` 成为唯一共享配置命名空间，旧 `framework` 明确失败，现行文档与诊断路径一致，定向及完整
  回归通过，完成证据写入工作台后立即停止。
- 两个既有 E2E 源文件已有大量与本切片无关的全文件 clang-format 差异；本轮仅替换配置键、局部变量名与诊断
  文本，保持原有局部排版并检查 Diff/空白，不对整文件做无关机械改写。

## 完成证据

- `core` 已成为 V1 参数信封的唯一共享配置入口；拥有型类型、任务接线与共享字段诊断均同步为 `core`。
  旧 `framework` 输入由生产解析器明确返回 `kUnknownConsumedField` 与 `/framework`，对应单元测试覆盖单独出现及
  与 `core` 同时出现两种情况。搜索结果中的其余旧命名仅位于拒绝断言和归档历史证据，不构成有效配置入口。
- `cmake --build build --target test_npm_basic test_scheduler_e2e test_config_channel_e2e -j8` 通过；定向 CTest
  3/3 通过（25.77 秒）。
- `cmake --build build -j8` 通过；完整 CTest 16/16、0 失败（64.27 秒）。
- 当前切片及 Feature 改动 C++ 均通过 `clang-format-18 --dry-run --Werror`；按冻结边界不对两个已有大量全文件
  格式差异的 E2E 源文件做无关机械重排，其本轮局部改动已通过 Diff/空白检查。所有新增或修改 C++ 文件版权头
  符合约定，`git diff --check` 通过。
- 两个已完成 Feature 的归档证据、Backlog 完成状态和现行 `docs/flow-labeling.md` 已同步为 `parameters.core`；
  本次未改变字段、默认值、范围、条件消费、matcher、会话、结果或 legacy `WITH` 语义，未 push。
