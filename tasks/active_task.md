# 当前工作台

WIP=0；npm-data-semantics：交付与验收已完成，本地提交清单和暂存内容审查通过。

- 关联 Feature：tasks/archive/feat-npm-data-semantics.md，T1～T7及后续Web说明布局/时间展示已验收完成。
- 当前 Atomic Slice：完整 Feature 的精确提交审查与本地 commit。
- 业务意图：将已验收的NPM领域说明、真实运行关联、静态默认字典及页面优化形成可复核的本地版本。
- Non-Goals：不修改已验收代码、不实施其他Feature、不扩大测试/部署范围、不推送/发布/重启常驻服务、不提交证据、构建产物、私有测试配置或无关规划。
- 允许工作树修改：仅tasks/active_task.md。允许暂存：evidence/npm-data-semantics/local-commit/manifest.json冻结的49个完整文件及2个局部文件；Backlog仅NPM行，Epic仅完成状态行和F4段。无关feat-stream-recompute.md及其他规划改动原样保留。
- 验收命令：git diff --check；源码/产物与最新验收摘要逐项比对；JSON/Python/Node语法及Node时间单测；git diff --cached --name-only、--stat、--check及暂存内容审查；提交后git show、git status和未提交文件内容比对。代码和产物摘要未变时复用既有通过的冻结验收，不重复完整回归。
- 依赖/位置：native本地Git及现有Python/Node工具；无外部服务或秘密配置读取。Git索引和对象写入按本地提交授权执行，不改变远端。
- 时间盒：2026-11-03 08:09:00～08:29:00 UTC，20分钟。
- 停止条件：暂存清单/内容/空白检查及源码摘要全部通过，工作台回WIP=0后执行本地提交；提交后核对索引为空、无关工作树改动保持，即停止。
- 基线及提交审查证据：evidence/npm-data-semantics/local-commit/，不入库。原完整工作台已快照保存。

## 本轮完成证据

- 精确清单51个路径：49个完整文件、Backlog的NPM完成行、Epic的完成状态行和F4段。无关stream-recompute及其他规划内容保持工作树原文。
- 47个源码/文档与验收基线一致；最新时间展示验收的35项源码及运行产物摘要全部一致。未改已验收代码，未重复完整回归。
- 本轮JSON解析4项、Python语法5项、Node脚本语法、时间展示单测5/5、29个C++文件版权头及暂存空白检查通过；49个完整暂存文件和2个局部文件逐字核对通过。
- Feature完成验收沿用本地10/10、调度/Task5/5、外部12项最终逐项全通过的记录；保留ClickHouse原失败及标准入口2/2复验。后续Web布局4/4、时间编辑5业务及5夹具10/10均通过，无必需跳过或未执行。
- 用户已明确授权本地提交；提交标识以Git日志为准。证据、构建产物及私有测试配置未暂存；不推送、部署或重启常驻服务。

## 上一切片：已验收的时间说明优化

- 关联 Feature：tasks/archive/feat-npm-data-semantics.md（已完成）；沿用已确认的第 4 项调整，不重开其他任务。
- 授权：用户确认统一 NPM epoch、让新旧对象正确识别、简化时间列、业务含义归入字段说明、完整结构留技术详情、未知时间定义以不可编辑状态展示。
- 业务意图：NPM 时间点/时长显示真实类型，避免将标准时间误认成特殊类型；编辑普通说明保留原有时间契约和历史。
- Non-Goals：不转换业务数值、不根据名字或数值猜时间、不迁移已有字典/历史、不改公共 Schema/API、不加新时间类型或任意同义词解析、不改算法/数据库后端、不部署或重启常驻服务、不提交/推送。
- 冻结契约：新 NPM instant epoch 使用 1970-01-01T00:00:00Z，UTC/ns 保持。读取已有 Unix epoch 仅视为同一已知基准，不写回；来源冲突比较亦认可这两个明确表达。普通正文保存保留原 time_semantics/来源/精确历史。简化时间编辑器只显示实际支持的 Unix时间戳/日期时间/时长/非时间；不支持的定义显示只读状态及按需简短摘要，不提供保留伪类型、不自动选择非时间。时间列至多显示基准/已有时区；业务 description 放在字段说明编辑区，只在正文未包含相同文字时补充，完整 JSON 保持。
- 允许文件：tasks/active_task.md；docs/data-semantics.md；src/operators/npm_basic/npm_data_contract.json；src/channels/config/metadata_description_time.h；src/frontend/src/components/ChannelDescriptionPanel.vue；src/frontend/src/utils/channelDescriptionTime.js；src/frontend/src/utils/channelDescriptionTime.test.js；src/tests/test_config_channel/test_metadata_description.cpp；src/tests/test_config_channel/test_npm_data_contract.cpp；src/tests/test_config_channel/npm_semantics_browser.mjs。证据 evidence/npm-data-semantics/time-editor/ 不入库。
- 实施：先用 Node/C++断言锚定新旧 epoch 识别、无推断、不支持定义、说明去重与保存保持；统一 NPM 契约并接入共享编辑器；补实际浏览器读取/保存/重读与已有字典断言，同步文档，单轮 Diff 审查。
- 构建：cmake -B build src；cmake --build build --target test_operator_contract_host test_metadata_description test_npm_data_contract test_npm_result_semantics -j4（包含隔离 Web/插件及前端 output/static）。
- 验收：node --test src/frontend/src/utils/channelDescriptionTime.test.js；tools/test.sh all --location native --filter '^(test_metadata_description|test_npm_data_contract|test_npm_result_semantics|test_metadata_channel_browser|test_npm_semantics_http)$' --jobs 1；node --check src/tests/test_config_channel/npm_semantics_browser.mjs；修改行clang-format、JSON/脚本语法、git diff --check、基线范围/产物摘要审查。
- 选择依据：修改 NPM 作者时间契约、共享时间冲突比较与 Web 编辑展示；覆盖契约生成/解析/静态字典、说明保存与原时间字段行为、通用通道浏览器及 NPM 真正读写重启；不改数据库方言/调度，不重复三库/调度或全仓。
- 依赖/位置：native，统一 config/testing/environment.json、config/testing/dependencies.json；现有 SQLite/动态插件、隔离 Python Worker/Web/Gateway、缓存 Node/Chromium、临时loopback端口，沿用 semantics_http/visualization_browser 资源锁；无外部数据库/Kafka/凭证/系统网络修改。必要本地构建及端口权限按工具审批执行。
- 时间盒：2026-11-03 06:49:00～07:19:00 UTC，30 分钟。
- 停止条件：冻结清单全绿、Diff及范围审查通过则WIP=0并停止；到时仅记录当前可验证检查点，续作前重新冻结，不扩大测试矩阵。
- 基线：evidence/npm-data-semantics/time-editor/worktree-before.json、before/；前轮 48 个预存改动保留。当前源路径已核查，新增epoch一致性不改变数值/时区/单位/历史。

## 完成证据

- NPM契约20处epoch值统一为1970-01-01T00:00:00Z，时间点仍为UTC/ns；所有时长不补基准和时区。旧Unix epoch只在读取/比较时识别为同一基准，既有字典及历史不迁移。
- Web删除保留伪类型和嵌套时间长文。支持的时间正常显示模式及简短基准/已有时区；不支持定义显示只读状态、单位和可展开人可读摘要。业务时间含义收入字段说明区，相同正文不重复，完整JSON保留。
- CMake配置及test_operator_contract_host（含前端）、test_metadata_description、test_npm_data_contract、test_npm_result_semantics全部构建通过。前端Node单测5/5、C++格式/JSON/脚本语法和git diff --check通过；Vite仅有既有chunk体积提示。
- 标准入口runner 20261103T065514Z-88a61bfd：selected=executed=passed=10，failed=skipped=not_executed=0；为冻结的5项业务及5项环境夹具。说明保存10.91秒、通用通道浏览器98.42秒、NPM契约2.14秒、结果说明52.03秒、NPM HTTP/浏览器226.23秒。
- 浏览器真实存入旧基准写法/自定义基准及分钟单位后核对正常识别/只读状态；普通正文保存重读后完整时间语义和精确历史保持；原四列表格/搜索/主题/关联、重复运行、PCAPNG、插件重启及已有对象静态字典均通过。浏览器阅读不预览/执行数据，显式保存动作1次；fixture导入使用独立API请求。
- 35项本轮源码及运行产物验收前后摘要一致；修改范围为冻结10文件，其他42项预存改动不变。实际截图与单轮Diff已复核，完整证据在本轮目录，不入库。
- 未提交/推送、未重启常驻服务或部署。当前实现和隔离验收完成，不声明常驻进程已加载新NPM契约。
