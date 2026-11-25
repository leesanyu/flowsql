# 当前工作台

- 关联 Feature：已归档的 [data-semantics-json-reading](archive/feat-data-semantics-json-reading.md)，本轮为用户指定的界面修正。
- Atomic Slice：删除 SQL 编辑器下方“查看对象说明 · JSON”按钮；已完成，WIP=0。
- 业务意图：DESC 成功时直接打开 JSON 弹框，关闭后编辑器下方不保留重复入口。
- Non-Goals：不修改结果列/历史结果/探索的说明入口，不改 JSON 窗口、后端、存储及说明数据契约。
- 核查依据：Tasks.vue 的 workbench-description 区块仅提供重新打开入口；DESC 成功分支已主动调用 openDescription。
- 最小契约：私有函数 `openDescription(observed)` 直接接收本次 DESC 响应；取消专用于重新打开的 description 响应缓存，保留请求代次检查和窗口关闭生命周期。
- 允许文件：`tasks/active_task.md`、`src/frontend/src/views/Tasks.vue`；本地构建和验收证据写入忽略的 `evidence/data-semantics-json-reading/remove-description-button/`。
- 验收命令：`cmake --build build --target flowsql_frontend -j2`；`tools/test.sh integration --location native --filter '^test_consumption_browser$' --jobs 1`；`git diff --check`。
- 验收选择：已有真实浏览器用例覆盖通道/算子 DESC 自动弹框、关闭、迟到请求、后续 SELECT 和历史结果；仅删除低影响界面入口，不新增镜像实现的测试。
- 依赖与执行位置：本机现有 CMake 构建、Node/npm、Chromium、Python Worker 和受管临时 HTTP/SQLite 夹具；使用 `config/testing/environment.json` 与现有 dependencies.json 声明；无外部数据库、Kafka 或 Docker 依赖。
- 时间盒：2026-11-24 18:47～19:07（20 分钟）。
- 停止条件：前端构建、所选浏览器回归、Diff 审查通过后完成本次修正；发现阻塞先记录证据，不扩大业务范围。
- 完成结果：已删除按钮及重新打开专用的响应缓存，DESC 成功分支直接传递响应打开 JSON 窗口；源码与生成的 Tasks 页面资源均无该按钮文字。
- 验收证据：`flowsql_frontend` 构建成功；现有工作台 Chromium 回归及环境夹具 2/2 通过，失败/跳过/未执行均为零；run `20261124T104813Z-fabcba53`。日志见上述本地证据目录的 `build.log`、`test.log`。
- 审查：本次 Diff 仅涉及允许的两个文件，`git diff --check` 通过；无 C++ 变更。Vite 仍有已有的大于 500 kB 分包提示，未调整构建阈值。
- 状态：本次修正完成，未提交或推送。
