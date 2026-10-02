# 即时工作台

事项：`npm-basic-realtime-integration`，规格见 [feat-npm-basic-realtime-integration.md](archive/feat-npm-basic-realtime-integration.md)。
关联 Feature Task：T3。
当前 Atomic Slice：T3.3 已完成；WIP=0。

## 业务意图

验证真实 Scheduler SQL 的 consumer 失败回收，复核 T0–T3 的最终 diff、格式与全量构建测试，然后更新规格和 Backlog。

## Non-Goals

- 本切片不实现采集后端、新 SQL、协议算法或其他 Feature；不改变现有结果 Schema 或 V1/V2 ABI。
- 不从单调时间推断事件水位；不提交或推送。

## 允许修改文件

- tasks/active_task.md
- tasks/specs/feat-npm-basic-realtime-integration.md
- tasks/product_backlog.md
- src/framework/core/pipeline.cpp
- src/framework/core/pipeline.h
- src/services/scheduler/scheduler_stream_executor.cpp
- src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp
- src/tests/test_framework/test_capture_contract.cpp
- src/operators/npm_basic/npm_basic_operator.cpp
- tasks/archive/feat-npm-basic-realtime-integration.md

## 验收命令

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
git diff --name-only
```

## 时间盒与停止条件

- 2026-10-02 20:33（Asia/Shanghai）起 10～30 分钟；完整 CTest、格式与 diff 审查通过后归档 Feature 并停止。
- 构建或测试错误未能修复时记录明确检查点。

## 检查点

- T0、T1、T2 已完成；`test_npm_basic`、`test_framework` 均通过。T2 已验证无包、有包、积压 present/unknown 和单调快照。
- T3.1 的真实 SQL 假 reader 单 stage、多 stage 测试已通过：周期快照和 EOF 最终态具有既有 Schema，下游 transform 收到时间输出，reader/buffer 各释放一次。
- T3.2 的 SQL source/transform residual、source error、取消、释放失败、transform 错误、Stop 和不同 generation/结果实体已通过。
- T3.3 的 managed consumer 写入失败后 reader 回收通过；全量构建成功，完整 CTest 40/40 通过，修改行格式与 `git diff --check` 通过。Feature 已归档，未提交或推送。
