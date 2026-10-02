# 即时工作台

事项：测试用动态库生命周期收口；关联此前 `npm-basic-realtime-integration` 崩溃复盘，无新 Feature Task。
当前 Atomic Slice：测试用 `.so` RAII 所有者及四处直接调用迁移，已完成；WIP=0。

## 业务意图

让测试中的动态库在插件实例、任务和插件产生的 Arrow 结果之后自动卸载，避免提前 `dlclose` 导致析构期崩溃，并把该顺序写成仓库规则。

## Non-Goals

- 不改变生产端 binaddon 加载器、插件 ABI、业务算法或其他测试的加载方式。
- 不移动或删除目录；不修改既有用户工作区中的 Linux 采集后端草稿。

## 接口与测试锚点

- 新增测试专用 `ScopedSharedLibrary`：打开路径、查询符号、析构关闭；禁止复制和手动提前关闭。
- 所有插件实例、任务、Arrow 结果及 Schema 均在库所有者销毁前释放；基准程序的异常路径遵守相同顺序。
- 四个直接使用 `dlopen`/`dlclose` 的测试调用点不再出现手动 `dlclose`。

## 允许修改文件

- AGENTS.md
- tasks/active_task.md
- src/tests/support/scoped_shared_library.h
- src/tests/test_npm_basic/test_npm_basic.cpp
- src/tests/test_npm_basic/benchmark_npm_basic.cpp
- src/tests/test_config_channel/test_config_channel_e2e.cpp
- src/tests/test_builtin/test_builtin.cpp

## 验收命令

```bash
cmake --build build --target test_npm_basic test_config_channel_e2e test_builtin benchmark_npm_basic -j$(nproc)
ctest --test-dir build --output-on-failure -R '^(test_npm_basic|test_config_channel_e2e|test_builtin)$'
build/output/benchmark_npm_basic 8 2
ctest --test-dir build --output-on-failure
git diff --check
git diff --name-only
```

## 时间盒与停止条件

- 2026-10-02 21:12（Asia/Shanghai）起 10～30 分钟；四处迁移、目标构建与测试、完整 CTest、格式及 diff 审查完成后更新工作台并停止。
- 构建或测试错误未能修复时记录明确检查点；不得扩大允许文件范围。

## 检查点

- 启动前确认 `tasks/product_backlog.md` 现有修改和 `tasks/specs/feat-npm-linux-capture-backends.md` 未跟踪文件属于既有工作区内容，本切片不碰。
- 四个构建目标通过；目标 CTest 3/3、完整 CTest 40/40 通过；基准两种模式各处理 16 包并正常退出。
- 基准原有 capability 数断言为 1，与当前插件导出 2 个不符；按现有 `test_npm_basic` 的契约修正后基准通过。
- 四处测试调用已无直接 `dlopen`/`dlsym`/`dlclose`；`git diff --check` 与修改行 clang-format 检查通过。
