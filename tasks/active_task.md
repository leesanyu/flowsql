# 即时工作台

WIP=0：`dataframe-lazy-loading` T1、T2、T3 已顺序验收并勾选，Feature 已归档；停止实施。

## 意图与边界

- 用户授权：T1→T2→T3 顺序完成，每项先验收并标记；本切片完成 Feature 后归档，不提交/推送。
- 业务意图：未加载与在途通道管理保持身份/数据一致；用户明确知道未知行数/Schema 和加载失败；验证主产物主链路及启动收益。
- Non-Goals：不改 CSV 格式/推断语义、公共 ABI，不增预热/淘汰；不改业务 CSV，不重启用户服务，不运行无关外部 DB/Kafka/Docker/全量。
- 时间盒：30 分钟；2026-10-26 05:20 UTC 开始，05:50 UTC 截止；沿用 T3 编号，不新增 Feature Task。
- 停止条件：冻结 Feature 清单全部通过，性能对照与限制记录，规格 T3 勾选、规格文件归档、Backlog 完成，WIP=0。

## 允许文件

- `tasks/active_task.md`
- `tasks/specs/feat-dataframe-lazy-loading.md`
- `tasks/archive/feat-dataframe-lazy-loading.md`（仅全部通过后归档文件；不移动目录）
- `tasks/product_backlog.md`
- `src/services/catalog/catalog_plugin.cpp`
- `src/services/catalog/lazy_dataframe_channel.h`
- `src/services/catalog/lazy_dataframe_channel.cpp`
- `src/tests/test_visualization/test_catalog_lazy_loading.cpp`
- `src/tests/test_visualization/test_dataframe_native.py`
- `src/tests/test_visualization/test_dataframe_browser.py`
- `src/tests/test_visualization/dataframe_browser.mjs`
- `src/frontend/src/views/Channels.vue`
- `src/frontend/src/api/index.js`（仅将硬编码 localhost:8081 改为同源基址）
- `docs/exploration.md`
- 忽略的 `evidence/dataframe-lazy-loading/`，不入库。

前序允许改动仍保留：catalog_plugin.h、test_visualization/CMakeLists.txt；本切片不再编辑这两个文件。每次 patch 后检查 git diff --name-only。

## 冻结契约与验收

- 未加载 rename 在通道锁中更新文件、路径、名称，保留 source_id；解析前在同锁中打开文件，重命名不使在途文件失效。
- delete 在目录锁中移除文件并退役旧句柄，防止旧解析回填；关闭/停止及快照所有权沿用 T2 契约。
- 行数未知显示“未知”，Schema 未知显示“首次访问后确定”；unloaded/loading/loaded/failed 四状态与错误可见，预览后刷新元信息。
- 新增确定性管理断言及原生 SQL/preview 恢复断言；真实 Chromium 核对主通道页未知/已加载/失败状态，复用探索图表端到端回归。
- 构建：`cmake --build build --target test_catalog_lazy_loading test_framework test_visualization_dataframe_store test_visualization_dataframe_workflow flowsql flowsql_scheduler flowsql_web flowsql_visualization -j2`。
- Feature 验收：`tools/test.sh all --location native --filter '^(test_catalog_lazy_loading|test_framework|test_visualization_dataframe_(store|workflow|transfer|native|browser))$' --jobs 1`。
- 选择依据：Catalog 新实现及 DataFrame 共享原语；共享快照 store/workflow、跨进程 transfer、含 Scheduler 的 native 读取/输出、主前端状态与 standalone/embedded 绘图，覆盖全部修改调用链。
- 依赖：根目录、本机主 build/output 产物、隔离 CSV/SQLite/0700 共享目录、随机 loopback 端口、现有 Node/npm/Chromium；无外部数据库、Kafka、Docker、私有凭证；复用 dependencies.json 既有 services=[] fixture/资源锁与 environment.json。
- 已有证据证明沙箱内不能启动本地监听；需外层执行包含 HTTP/Chromium 的验收，按工具权限申请，不调整断言。
- 性能：同一机器、同一批固定文件数/不同总字节数和固定单文件大小/不同文件数，加空目录，旧 Catalog 保存产物与新主产物各三次新进程；分别记录 Catalog Start、列表、首次读取、启动/列表峰值 RSS 和首次读取后峰值 RSS。
- 旧产物与新产物使用同一 common 库、同一探针、同一隔离样本和热文件缓存；证据注明构建模式和限制，不作生产容量保证。
- 核对 selected=executed=passed 且 failed=skipped=not_executed=0；格式检查、Python AST、Vue 构建、变更 diff 审查、源码/主产物摘要。

## 前序证据

- T1：`evidence/test-environment/20261026T044027Z-7d4e78b6/summary.json`，1/1。
- T2：`evidence/test-environment/20261026T044801Z-0730e4b1/summary.json`，3/3；沙箱本地监听失败后外层同清单通过。

## T3 Slice 1 检查点：当前错误待修复

- 主产物构建成功；Feature 清单全部执行 11 项，10 通过、1 失败，零跳过。
- 证据：`evidence/test-environment/20261026T051719Z-7bcbe9af/summary.json`；唯一失败为主通道页等待未加载行超时，standalone 绘图通过。
- 已核查代码：共享 Axios 客户端硬编码 `http://localhost:8081`，随机端口的真实 Web 页面不能取到通道数据；远程浏览器也会请求自身 localhost。这是当前用户状态展示验收的 P1 阻塞。
- 有限修复：同源基址，保持所有 API 路径/方法/请求体，新增允许文件仅 api/index.js；真实浏览器列表/预览及原清单作为路由行为回归，不增加外部矩阵。
- Slice 1 关闭；依用户持续完成授权冻结 Slice 2，仅处理该阻塞并完成原清单、性能及归档，T3 不勾选。

## T3 Slice 2 完成证据

- Feature 最终：`evidence/test-environment/20261026T052611Z-c44b5f03/summary.json`，selected=executed=passed=11，failed=skipped=not_executed=0（7 业务 + 4 fixture）。
- 源码/主产物构建通过，真实浏览器确认新状态和同源请求；冷恢复 SQL/预览/覆盖、快照与两种入口绘图通过。
- 正式性能（无并行构建/验收、每组各3次）：8×约2 MiB Start 1473.99→27.379 ms，列表新0.186 ms，首次目标读取新201.137 ms，启动/列表峰值 RSS 66920→23828 KiB；空目录和固定文件数/大小矩阵均已记录，时延只计 Catalog Start。
- 原始记录 `evidence/dataframe-lazy-loading/performance.json`，范围及源码/主产物摘要 `verification.json`；使用说明和限制写入 `docs/exploration.md`，证据不入库。
- 规格 `tasks/archive/feat-dataframe-lazy-loading.md`；Backlog 已完成。格式/语法检查和本轮 Diff 审查通过；没有扩大外部验收矩阵，没有重启用户服务，没有提交或推送。
