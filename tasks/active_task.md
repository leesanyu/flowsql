# 当前工作台

- 关联工作：用户要求 DataFrame 首次查看字典时加载懒加载数据；独立行为调整。
- Atomic Slice：字典页发现 pending DataFrame 后触发现有加载入口，重读字典，覆盖首次/重复/失败/空表行为及文档。
- 状态：已完成；WIP=0。
- 业务意图：用户首次打开 DataFrame/CSV 的数据说明即可看到真实字段，无需先关闭字典再手动预览。
- Non-Goals：不改后端接口或加载器、不让普通 HTTP describe 自动加载、不自动保存说明、不改数据库/Kafka/关联选择器行为、不执行任务或全仓回归、不提交推送。
- 时间盒：2026-11-20 12:58–13:28 Asia/Shanghai，30 分钟；本切片在时间盒内完成。

## 当前契约与主链路

- 前端先请求现有 describe；仅 `locator.kind=dataframe` 且 `structure.state=pending` 时，调用 `api.previewDfChannel(name, 1, 1)` 触发现有加载，加载完成后再次 describe。已发生真实 CSV 加载失败时，describe 返回 `SOURCE_UNAVAILABLE: CSV_LOAD_FAILED`，同一入口允许重新尝试加载以支持修复来源后的显式重试；其他读取错误直接报告。
- 每次打开/刷新最多尝试加载一次，加载中显示状态并禁用编辑与按钮；失败显示具体错误并保留草稿，允许用户显式刷新重试，不自动循环。
- 已加载对象再次打开或刷新不请求数据预览；响应只取 1 行触发加载，不打开预览抽屉或展示业务行，底层仍按现有懒加载机制读取整个 CSV。
- 普通 API 的 describe、列表、数据库/Kafka 和关联选择器不因此自动加载。数据加载不等于业务质量验证，不写字典或绑定。

## 允许文件及实施步骤

- `tasks/active_task.md`
- `src/frontend/src/components/ChannelDescriptionPanel.vue`
- `src/tests/test_config_channel/metadata_channel_browser.mjs`
- `src/tests/test_config_channel/test_metadata_channel_browser.py`
- `docs/data-semantics.md`
- 本地忽略证据 `evidence/dataframe-dictionary-loading/`，并允许现有浏览器 runner 按既有位置更新其忽略证据。
- 先更新真实 Chromium 断言与空 CSV 夹具，冻结一次加载、加载中禁用、重试、草稿保留、空数据、重复进入/刷新和 API 只读边界；再实现组件并同步使用说明。
- 其他已存在改动为用户 `AGENTS.md` 和上轮工作台；全部保留。每次 patch 后检查 `git diff --name-only`，只修改本清单。

## 冻结构建与验收

- `cmake -B build src`
- `cmake --build build --target flowsql_frontend test_channel_dictionary_host test_operator_contract_host -j4`
- `tools/test.sh all --location native --filter '^(test_metadata_channel_browser|test_metadata_description_api|test_metadata_reading_frontend)$' --jobs 1`
- 选择依据：真实浏览器覆盖组件、首次和重复打开、错误/重试、空 CSV、原有说明保存/下载/草稿与数据库/Kafka页面；description API 回归证明后端 describe 仍不加载 pending；前端工具断言覆盖共享 JSON 和本地补充工具。
- 测试使用 `config/testing/environment.json` 与 `config/testing/dependencies.json`，native，隔离临时 CSV/SQLite、本地 HTTP/插件、Node/Chromium及 Python Worker，沿用现有 fixture、超时和资源锁，不增加外部服务或凭证。
- 构建只执行上述 CMake 目标，不手动删除目录，不重建无关模块；无 C++ 修改。Vue 由 Vite 编译；Node语法检查改动浏览器脚本，Python AST检查改动夹具。
- `git diff --check`；核对选中与实际执行清单，必需用例零跳过。源码/产物摘要、日志和结果写入忽略证据缓存。

## 停止条件

- 用户要求的首次查看加载、重复打开复用、失败提示/显式重试、空 CSV 和原有读取/保存行为均通过所选验收后更新证据并停止。
- 时间盒到期保留当前任务，记录已通过检查点和具体错误，不扩范围。
- 与本切片无关的问题仅记录，不修改额外文件；未验证范围不声明通过。

## 完成证据

- 已实现 pending DataFrame 首次查看字典时自动触发现有加载入口，完成后重读真实字段；加载中禁用编辑和重复请求，响应仅取 1 行，不打开预览抽屉或保存说明。
- 真实 Chromium 验证首次加载、已加载实例重新打开/刷新不重复加载、空 CSV 字段正常展示及 0 行状态；真实 CSV 加载失败显示具体错误，不循环重试，修复文件后显式刷新加载成功，对象和字段草稿均保留。
- 原有说明保存、无损下载、来源变化/冲突草稿、数据库与 Kafka 元数据读取、插件宿主重启持久化均通过同一浏览器用例。普通 HTTP describe 及宿主重启后的 describe 仍保持 pending，不自动加载。
- CMake 配置与三个冻结目标构建通过；Python 夹具 AST、Node 脚本语法、Vue/Vite 构建及 `git diff --check` 通过。
- 最终标准 runner `20261120T050908Z-5b9ff4d4`：3 个功能用例与 3 个夹具共 6/6；失败、跳过、未执行均为 0。浏览器用例实际执行 102.47 秒，沿用既有超时与资源锁。
- 初期新增测试的变量作用域冲突和错误列表请求地址均已修正；失败记录保留，最终完整清单重新验收通过，不将失败轮次计入通过。
- 本地证据 `evidence/dataframe-dictionary-loading/`：`configure.log`、`build.log`、`regression-final.log`、`acceptance.json`；源码、静态资源和构建产物摘要在最终验收期间一致，见 `manifest-final-before.json` 与 `manifest-after-validation.json`；浏览器截图与宿主日志在其 `channel-browser/`。
- 用户说明已同步新的字典加载方式和 API 读取边界；未修改后端接口、算法或其他部署目录，未执行性能复测、全仓回归或外部数据库/Kafka 矩阵。
- 用户原有 `AGENTS.md` 改动保留，未提交/推送。当前切片完成并停止。
