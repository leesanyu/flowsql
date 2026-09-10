# Active Task

Feature：`npm-offline-filter`
原子任务：T2.3b resolver IID 注册与 Scheduler 当前 stage 遍历/fallback
状态：已完成

## 业务意图

- 由 pcapfile plugin 通过 `IID_FILTER_DOMAIN_RESOLVER_V1` 暴露 T2.3a 已实现的领域 resolver，不让
  Scheduler 依赖具体 plugin 类或 `.so` 名称。
- Scheduler 在 source stage 和每个 transform stage 的通用 schema binding 前遍历 resolver IID；唯一 owner
  成功时绑定完整 lowered AST，零 owner/全部 `ENOTSUP` 时保留原 AST 走通用 binder。
- 让无算子的 `block_stream -> dataframe` 路径执行同一 source-stage resolver、binder 与纯 Arrow residual，
  支持 Feature 主用例的直接离线过滤。

## Non-Goals

- 不实现 pushdown 拆分、canonical packet plan、`IBlockStreamReaderFactoryV1`、任务独占 reader 或 pcapfile
  数据面过滤；这些属于 T3/T4。
- 不修改公共 resolver、filter AST、packet schema、channel ABI 或 parser 契约，不增加共享 channel 过滤状态。
- 不调用 `npi::Identify()`，不修改 pcap/pcapng 解码、EOF/错误/取消和 release 生命周期。
- 不读取或修改 `tasks/sprints/**`，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-offline-filter.md`
- `src/channels/pcapfile/pcap_file_channel.h`
- `src/channels/pcapfile/pcap_file_channel.cpp`
- `src/channels/pcapfile/plugin_register.cpp`
- `src/services/scheduler/scheduler_stream_executor.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_mutation_guard.cpp`

既有累计 diff（T2.1/T2.2/T2.3a 已产生，本切片只保留、不修改）：

- `src/channels/pcapfile/CMakeLists.txt`
- `src/channels/pcapfile/packet_filter_domain.h`
- `src/channels/pcapfile/packet_filter_domain.cpp`
- `src/framework/core/filter_binding.cpp`
- `src/tests/test_pcapfile_import/CMakeLists.txt`
- `src/tests/test_pcapfile_import/test_pcapfile_import.cpp`

## 验收命令

- E2E 验证动态加载的 pcapfile plugin 可通过 `IID_FILTER_DOMAIN_RESOLVER_V1` 发现，且 source
  `TIMESTAMP` 领域表达式在直接 `pcapfile -> dataframe` 路径得到精确 residual 结果。
- E2E 验证 source resolver 错误在读取 block 和创建 transform 执行任务前失败；既有普通 source/transform
  filter 在无 owner 或 pcapfile resolver 返回 `ENOTSUP` 时继续由通用 binder 执行。
- 验证唯一 owner、零 owner、owner 冲突、resolver 错误和 IID 遍历错误不会静默降级为未过滤执行。
- `cmake --build build --target test_scheduler_e2e test_scheduler_mutation_guard -j$(nproc)`
- `ctest --test-dir build -R '^(test_scheduler_e2e|test_scheduler_mutation_guard)$' --output-on-failure`
- `git diff --check`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：30 分钟。
- resolver 注册、当前 stage 遍历/fallback 与 direct block residual 全部通过验收后，勾选 T2.3b、T2.3 和 T2，
  将任务标为已完成并立即停止；不自动进入 T3。
- 若正确性必须修改公共接口、reader factory 或 pcapfile 数据面，状态记为“当前错误待修复”，不扩大本切片。

## 验收结果

- pcapfile plugin 已通过 `IID_FILTER_DOMAIN_RESOLVER_V1` 暴露组合持有的 resolver；Scheduler 只按 IID
  遍历，不依赖具体类或动态库名称。
- source stage 与每个有过滤器的 transform stage 均在通用 binding 前执行 resolver 遍历；唯一 owner 使用
  lowered AST，零 owner/全部 `ENOTSUP` 使用原 AST，双 owner、resolver 错误和遍历错误均在首次读取前失败。
- 无算子的 `pcapfile -> dataframe` 已使用相同 resolver、binder 和 `BlockFilterStage` residual；真实两包 pcap
  以 `TIMESTAMP >= 1970-01-01T00:00:02Z` 精确输出 sequence 1，原 block 仍 exactly-once release。
- 非法 `port(70000)` 在创建 transform task 和读取 block 前以 source-stage domain error 失败；普通
  source/transform filter 的无 owner fallback 保持原结果。
- `cmake --build build --target test_scheduler_e2e test_scheduler_mutation_guard test_pcapfile_import -j$(nproc)`：
  通过。
- `ctest --test-dir build -R '^(test_scheduler_e2e|test_scheduler_mutation_guard|test_pcapfile_import)$'
  --output-on-failure`：通过（3/3）。
- `cmake --build build --target test_framework -j$(nproc)` 与对应 CTest：通过（1/1）。
- `git diff --check`、本切片新增行 120 列与尾随空白检查：通过；当前环境未安装 `clang-format`，工程无
  format/lint target。
- T2.3b、T2.3 与 T2 完成即停；未进入 T3，未实现 reader factory、pushdown 或 pcapfile 数据面过滤。
