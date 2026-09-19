# 即时工作台

事项：`npm-basic-parameters` T3 SQL/E2E、诊断与 Feature 收口
关联 Feature Task：`tasks/archive/feat-npm-basic-parameters.md` T3；本切片完成 T3 和 Feature 完成出口后停止。
当前 Atomic Slice：复用真实 `npm.basic` 插件/PCAP/Scheduler 场景锚定 legacy/V1 SQL 等价、非活动模块忽略及
活动配置失败诊断，补齐算子最小诊断接线，完成定向与全量回归并归档 Feature。
状态：已完成

## 业务意图

- 让既有 Basic/Session SQL 无修改继续运行，并证明等价 V1 SQL 经真实 Scheduler、插件和 PCAP 得到相同 Schema
  与可观察结果。
- 让活动模块参数类型、范围及新旧来源冲突在可执行任务建立前确定性失败，并由 Scheduler 返回稳定类别和
  JSON Pointer 字段路径；非活动和未知模块配置仍可安全携带而不影响执行。
- 以完整构建和 CTest 收口通用参数入口，为后续标签化与协议模块保留稳定的模块命名空间扩展点。

## Non-Goals

- 不修改 SQL Parser、V1 信封/字段 Schema 或参数上下界；T0/T1 已完成这些契约。
- 不新增 runtime 配置通道，不改变 Basic/Session Schema、结果、预算、生命周期和默认值。
- 不实现 `npm-labeling`、Config Channel Resolve、TCP 字节流或具体协议模块，不修改 frontend。
- 不处理当前切片外的既有工作树改动，不 commit、不 push，也不开始后续 Feature。

## 允许修改文件

- `tasks/active_task.md`
- `tasks/specs/feat-npm-basic-parameters.md`
- `tasks/archive/feat-npm-basic-parameters.md`
- `tasks/product_backlog.md`
- `src/operators/npm_basic/npm_basic_operator.h`
- `src/operators/npm_basic/npm_basic_operator.cpp`
- `src/tests/test_npm_basic/test_npm_basic.cpp`
- `src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp`

只有测试证明参数状态缺少稳定类别文本时，才允许追加
`src/operators/npm_basic/npm_basic_task_config.h/.cpp`；不得预先修改。

## 验收命令

```bash
cmake --build build --target test_npm_basic test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^(test_npm_basic|test_scheduler_e2e)$' --output-on-failure
cmake -B build src
cmake --build build --target test_framework test_npm_basic test_scheduler_e2e -j$(nproc)
ctest --test-dir build -R '^(test_framework|test_npm_basic|test_scheduler_e2e)$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

另按 `src/.clang-format` 人工审查本切片 C++ Diff，并检查新增/修改 C++ 行不超过 120 列；若环境提供
`clang-format` 或仓库 format/lint target，则同时运行对应检查。

## 时间盒与停止条件

- 时间盒：30 分钟；若需延续则沿用 T3 和本工作台边界，不拆出伪任务或扩大允许文件。
- 停止条件：T3 的真实 SQL/E2E、活动配置诊断、非活动模块忽略及完整回归全部通过；随后勾选 T3，将规格移入
  `tasks/archive/`、把 Backlog 标为完成、记录完成证据并立即停止。
- 若测试证明需要越过允许文件、出现 P0/P1 跨任务阻塞，或时间盒检查点仍有明确错误，则记录客观证据后停止，
  不通过扩大范围规避。

## 完成证据

- 真实插件/PCAP/Scheduler E2E 保留既有 legacy Basic/Session SQL，并新增等价 V1 SQL；两组 Arrow Schema 和完整
  RecordBatch 分别相同。Basic-only V1 安全忽略非活动 Session 内部错误、未启用 labeling 的非精确引用和未知
  `http1` object；Session V1 安全忽略未知 future protocol object。
- 活动 framework 类型错误、活动 Session 范围错误及 legacy/V1 来源冲突均由 Scheduler Schema probe 返回稳定
  类别与 JSON Pointer，未注册目标 DataFrame、未进入 NPI 包处理；算子通过任务私有不可变字符串保留详细首错，
  未改变 runtime、Schema、结果与 Cancel 行为。
- `cmake -B build src`、三个指定目标构建和完整构建均通过；定向 CTest 3/3 通过（38.97 秒），完整 CTest 15/15
  通过（52.99 秒）。
- `git diff --check`、未跟踪新增文件空白检查和新增/修改 C++ 120 列检查通过；环境无 `clang-format`，仓库无
  format/lint target，已按 `src/.clang-format` 人工审查本切片 Diff。
- T3 已勾选，Feature 规格已移入 `tasks/archive/`，Backlog 已标记完成；未 commit/push，未开始后续 Feature。
