# 即时工作台

WIP=0；已完成 Atomic Slice：修复 Web 入口的 DataFrame 快照代理。当前无进行中的切片。

## 意图与边界

- 关联 Feature Task：已归档 `visualization-dataframe` / T4、T5 的缺陷修复，沿用既有契约，不新增 Feature。
- 用户授权：2026-10-23 用户确认前轮最小修改方案。
- 业务意图：用户从 Web 数据探索选择 DataFrame 时可以申请、续期、释放快照并绘图，不再收到空 405 引起的 JSON 解析错误。
- 已核查：8081 的 snapshots/acquire、renew、release 均返回 405/空响应；18910 的 acquire 返回正常 JSON 参数错误。
  Web 代理白名单、预路由放行和正式 POST 路由均遗漏三个快照操作。
- Non-Goals：不改计算/存储/ABI、前端通用错误处理、部署配置、已有五个文件改动；不启动四库/Kafka/Docker，不执行全量 all；实现阶段不提交或重启用户服务。
- 时间盒：20 分钟；2026-10-22 17:44 UTC 开始，18:04 UTC 截止。
- 停止条件：冻结验收全部通过并记录证据后立即停止；到期则保留当前检查点/错误证据，不扩大范围。

## 允许修改文件

- `tasks/active_task.md`
- `src/services/web/exploration_proxy.hpp`
- `src/services/web/web_server.cpp`
- `src/tests/test_visualization/test_dataframe_browser.py`
- `src/tests/test_visualization/dataframe_browser.mjs`
- 本轮人工证据写 `evidence/visualization-dataframe/web-proxy-fix/`；用例沿用自身 evidence 子目录，标准 runner 自行写 `evidence/test-environment/`；均不入 Git。
- 启动前已有修改：`config/deploy-single.yaml`、`config/deploy-visualization.yaml`、`docs/exploration.md`、
  `src/tests/test_framework/test_native_deploy_config.cpp`、`src/tests/test_visualization/test_dataframe_native.py`；全部保持原样。

## 契约与执行步骤

- HTTP 契约：`POST /api/visualization/v1/snapshots/{acquire,renew,release}` 读取完整 JSON body 后转发；状态、JSON、Content-Type 保留上游结果。
  GET/HEAD/DELETE 等错误方法及未知 snapshots 操作仍拒绝；既有 compute/cancel 与静态资源代理保持原契约。
- 先扩展测试并在旧主产物上复现失败，再补齐共享 POST 路径判断、预路由放行和正式路由，避免请求体在预路由阶段尚未读取。
- 浏览器覆盖独立探索与 Web `/explore` iframe：申请、12 类图表、刷新版本、续期、关闭释放及数据库/DataFrame 切换；额外 HTTP 断言三个快照操作、错误参数/错误方法。
- 每次 patch 后检查 `git diff --name-only`，对比五个既有文件的启动摘要。

## 冻结验收

- 执行位置：仓库根目录；使用现有 `build`（CMake 入口 `src`），验证默认 `build/output` 产物。
- 依赖：现有 Node/Chromium 和本地 C++ 产物；用例自行创建临时 SQLite、CSV、0700 共享目录和动态 loopback 端口，finally 回收自身进程。
  声明沿用 `config/testing/dependencies.json` 中三个 native 用例（services=[]、既有资源锁），无需外部凭证/服务。
  标准 runner 配置来源 `config/testing/environment.json`；需要本地监听及子进程权限。
- 构建：`cmake --build build --target flowsql_web -j2`。
- 修复前复现：`tools/test.sh integration --location native --filter '^test_visualization_dataframe_browser$' --jobs 1`，应在经 Web 申请快照处失败并保留证据。
- 修复后验收：`tools/test.sh integration --location native --filter '^test_visualization_(entry|dataframe_browser|dataframe_transfer)$' --jobs 1`。
  选择依据：entry 覆盖既有 Web 路由/iframe/静态资源；dataframe_browser 覆盖实际 Web DataFrame 主链路；dataframe_transfer 回归快照生命周期与传输契约。
- 核对 3 个业务用例和标准 runner 自动纳入的 3 个必要环境 fixture：selected=executed=passed=6，failed=skipped=not_executed=0；
  检查本次 C++ diff 的 clang-format、Python/JS 语法、`git diff --check`，记录源码/主产物摘要。

## 本轮证据

- 修复前回归：`evidence/test-environment/20261022T175007Z-6ec109ec/summary.json`；fixture 通过，浏览器用例在 Web 申请快照断言处失败 `(405, '')`。
- 首次沙箱执行：`evidence/test-environment/20261022T174557Z-a175f254/summary.json`；因本地 socket 被禁止未进入业务验证，随后已在允许本地服务的环境重跑。
- 已完成：共享 POST 白名单、预路由放行及正式路由均支持 snapshots/acquire、renew、release；请求体在正常路由读取完成后转发。
- 默认产物：`cmake --build build --target flowsql_web -j2` 成功，`build/output/libflowsql_web.so` 已更新，并由隔离服务实际加载验收。
- 冻结验收：`evidence/test-environment/20261022T175432Z-b6a6d044/summary.json`，
  selected=executed=passed=6（3 业务 + 3 fixture），failed=skipped=not_executed=0；未在验收期间改变源码或产物。
- 浏览器：`evidence/visualization-dataframe/ui/{standalone,embedded}/dataframe-browser.json`；
  独立页、Web `/explore` iframe 各 13 项检查通过，均覆盖 12 类图表、申请/续期/刷新/释放和数据库/DataFrame 切换。
  `ui/proxy-http.json` 记录正常快照链路、JSON 参数错误、错误方法及未知操作仍拒绝。
- 格式与边界：本次 C++ 改动行 clang-format、Python/JavaScript 语法、`git diff --check` 通过；五个既有文件 SHA-256 保持不变。
- 本轮汇总与源码/产物摘要：`evidence/visualization-dataframe/web-proxy-fix/acceptance.json`（本地，不入 Git）。
- 运行边界：实现阶段 17:54 UTC 再查 8081 用户进程仍返回旧的空 405；本轮未重启用户进程。随后用户于 2026-10-23 确认手动测试问题已解决。
- 状态：2026-10-22 17:56 UTC 实现与验收已完成，停止本切片。未执行仓库全量、四库、Kafka、Docker 或镜像验收。

## 本地提交

- 用户授权：2026-10-23 用户确认“问题已经解决，提交代码”；仅创建本地提交，不推送。
- 提交范围：上述允许修改清单的 5 个文件（2 个源码、2 个回归测试和本工作台）；既有 5 个文件改动、evidence、构建产物及私有配置不纳入。
- 提交标题：`fix(web): proxy DataFrame exploration snapshot requests`。
- 提交前核对：4 个源码/测试文件及主 .so 的 SHA-256 与已通过验收时一致；沿用本轮实际执行的 6/6 结果，无须重复运行测试。
  工作台仅补充本次提交授权与用户手动验证结果；验收边界和未验证范围保持不变。
- 提交前暂存区为空；按明确路径暂存并检查 staged diff、`git diff --cached --check`，提交后核对路径、暂存区及既有文件摘要。
