# 即时工作台

事项：`npm-capture-contract` 已完成，规格见 [归档](archive/feat-npm-capture-contract.md)。
关联 Feature Task：T3 已完成。
当前 Atomic Slice：T3.1 已完成；WIP=0。

## 业务意图

让未来 Linux/DPDK 后端能够复用假源契约套件，端到端验证 packet/事实同序、批次归还、背压、终态、统计可用性及任务隔离。

## Non-Goals

- 不实现真实网卡后端、Scheduler/NPM 实时接线或协议算法。
- 不修改既有 BlockStream V1 接口；不提交或推送。

## 允许修改文件

- tasks/active_task.md
- tasks/archive/feat-npm-capture-contract.md
- tasks/product_backlog.md
- src/framework/interfaces/icapture_block_stream_reader.h
- src/framework/core/capture_reader_state.h
- src/framework/core/capture_progress_tracker.h
- src/tests/test_framework/test_capture_contract.cpp
- src/tests/test_framework/main.cpp

## 验收命令

```bash
cmake --build build --target test_framework -j$(nproc)
ctest --test-dir build -R '^test_framework$' --output-on-failure
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
clang-format-18 --dry-run --Werror src/framework/interfaces/icapture_block_stream_reader.h src/framework/core/capture_reader_state.h src/framework/core/capture_progress_tracker.h src/tests/test_framework/test_capture_contract.cpp
clang-format-18 --dry-run --Werror --lines=76:76 --lines=4635:4635 src/tests/test_framework/main.cpp
git diff --check
git diff --name-only
git status --short
```

## 时间盒与停止条件

- 2026-10-02 17:24（Asia/Shanghai）起 10～30 分钟；T3 测试及 Feature 总验收通过后更新规格和需求池并停止。
- 如构建/测试错误在本时间盒无法修复，记录明确错误检查点并停止本切片。

## 检查点

- T0～T2 代码已实现；每阶段 `test_framework` 定向 CTest 1/1 通过、格式及 diff 检查通过。
- T3 待补完整假源链路、统计快照与最终全量验收。
- T3 假源、统计和终态断言通过；最终标准全量构建与完整 CTest 40/40 通过。`main.cpp` 既存未格式化区域很多，仅对本次新增行做 clang-format 检查。
- 最终审查补齐了 reader 重开 generation、释放内存不足返回、非法事实组合、缺失扩展和结构版本拒绝；定向 `test_framework` 1/1 再通过。规格归档、backlog 标记完成；未提交或推送。
