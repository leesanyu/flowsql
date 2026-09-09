# Active Task

Feature：`npm-offline-filter`
原子任务：T1.3 source reader ABI、配置深拷贝与独占实例契约夹具
状态：已完成

## 业务意图

- 用可实例化的 legacy fixture 证明新增 reader factory 没有向既有 `IBlockStreamFactory` 或
  `IBlockStreamChannel` 追加虚函数，新能力只通过独立 IID 暴露。
- 用最小 `IBlockStreamReaderFactoryV1` fixture 锚定 provider 在 `CreateReader()` 返回前深拷贝全部临时字符串。
- 连续创建两个携带不同 task/plan 的 reader，证明实例地址、配置和可变生命周期状态相互隔离，并由同一
  provider 分别释放。
- 锚定 owner 成功返回非空 reader、非 owner/非法配置返回错误且输出 reader 为空的边界。

## Non-Goals

- 不修改 `IFilterDomainResolverV1`、`IBlockStreamReaderFactoryV1`、`IBlockStreamFactory`、
  `IBlockStreamChannel` 或 packet plan 公共头文件。
- 不实现 Scheduler reader session、IID 遍历、pcapfile provider、canonical plan 解析或数据面过滤。
- 不增加共享 channel 过滤状态，不实现并发调度、EOF/错误/取消的生产生命周期；这些属于 T3/T4。
- 不调用 `npi::Identify()`，不增加协议/应用过滤。
- 不读取或修改 `tasks/sprints/**`，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-offline-filter.md`
- `src/tests/test_framework/main.cpp`

既有累计 diff（T0/T1.1/T1.2 已产生，本切片只保留、不修改；其中规格和 framework 测试按上方列表继续演进）：

- `tasks/product_backlog.md`
- `src/framework/CMakeLists.txt`
- `src/framework/core/filter_binding.h/.cpp`
- `src/framework/core/filter_expression.h/.cpp`
- `src/framework/core/packet_filter_plan.h`
- `src/framework/core/sql_parser.h/.cpp`
- `src/framework/interfaces/ifilter_domain_resolver.h`
- `src/framework/interfaces/iblock_stream_reader.h`
- `src/services/scheduler/scheduler_stream_executor.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`

## 验收命令

- 测试实例化只实现旧方法的 `IBlockStreamFactory` fixture，并冻结旧 `Get/List` 方法签名及新旧接口无继承关系。
- 测试在 caller 字符串失效/改变后，reader 仍持有原 task/category/name/plan；两个 reader 地址、配置和
  cancel/open 状态相互隔离。
- 测试成功、非 owner、非法版本、空输出参数及 provider 分别释放两个 reader 的返回值/所有权边界。
- `cmake --build build --target test_framework -j$(nproc)`
- `ctest --test-dir build -R '^test_framework$' --output-on-failure`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：25 分钟。
- ABI/deep-copy/exclusive-reader fixture 通过，规格勾选 T1.3 和父任务 T1，且本切片无越界修改后，将任务标为
  已完成并立即停止，不自动执行 T2。
- 若契约夹具必须修改公共接口或运行时代码，状态记为“当前错误待修复”，不扩大本切片。

## 验收结果

- legacy `IBlockStreamFactory` fixture 仅实现既有 `Get/List` 即可实例化；旧 factory/channel 头文件相对
  `HEAD` 零 diff，新 reader factory 保持独立接口与 IID。
- caller 字符串被修改或离开作用域后，两个 reader 仍分别持有原始 task/category/name/plan；reader 地址、
  open/cancel 状态互不共享。
- owner、非 owner、非法版本、空输出参数与分别释放两个 reader 的契约断言通过。
- `cmake --build build --target test_framework -j8`：通过；仅有工作区 clock-skew 警告，无编译错误。
- `ctest --test-dir build -R '^test_framework$' --output-on-failure`：通过（1/1）。
- `git diff --check` 和本切片尾随空白检查：通过。
- 当前环境未安装 `clang-format`，工程无 format/lint target；已按 120 列限制人工复核本切片代码。
- T1.3 与父任务 T1 完成即停；未进入 T2，未修改公共接口或生产运行时代码。
