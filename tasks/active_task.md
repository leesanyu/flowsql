# 即时工作台

WIP=0：`sql-workflow-guidance` 已完整验收并归档；提交前审查通过，按用户授权执行本地提交。

## 本地提交切片：sql-workflow-guidance

- 用户授权：提交代码；仅本地 commit，不推送、打标签、发布或部署。
- 关联任务 / 业务意图：已完成的 T1～T4；审查并提交统一 SQL 指引、原文示例验收入口及 Feature 收口，使交付可追溯。
- Non-Goals：不修改业务实现或已验收示例，不重跑无变化的完整运行验收，不提交其他 Feature 的规划、证据、构建产物或私有配置。
- 允许文件及精确提交集合：`README.md`、`config/testing/dependencies.json`、`docs/baseliner.md`、`docs/kafka.md`、`docs/netadapter.md`、`docs/npm.md`、`docs/sql-workflow.md`、`docs/vision.md`、`src/CMakeLists.txt`、`src/tests/test_sql_workflow/CMakeLists.txt`、`src/tests/test_sql_workflow/test_sql_workflow.py`、`tasks/active_task.md`、`tasks/archive/feat-sql-workflow-guidance.md`、`tasks/product_backlog.md`。本切片仅修改工作台提交记录，其余文件仅审查/暂存。
- 部分暂存边界：Backlog 仅提交 sql-workflow-guidance 完成条目及必要表结构；工作台仅提交当前 Feature 记录，其他五项规划及其历史记录逐字保留在工作区、不纳入本提交。已有目录维护记录保持 HEAD 内容。
- 验收依据：最终运行清单 `20261027T130719Z-df5174b2` 为 selected=executed=passed=12，failed=skipped=not_executed=0；已核对 11 个 Feature 源码/文档和 24 个产物摘要无漂移。提交前复验 G5、AST/JSON、链接/归档状态及暂存 Diff，不将其表述为新的完整运行验收。
- 验收命令：`python3 src/tests/test_sql_workflow/test_sql_workflow.py build/output docs`、`git diff --cached --name-only`、`git diff --cached --check`；精确比对暂存内容与审查文件，提交后核对 commit 路径、剩余改动及索引为空。
- 执行位置/依赖：仓库根目录，本地 Git 与 Python 标准库；不启动服务或外部 fixture，Git 元数据写入按环境权限执行。
- 提交信息：`docs: add executable SQL workflow guidance`。
- 提交前检查点：G5、源码/产物摘要、AST/JSON、归档链接与验收计数已核对通过；精确暂存后的 Diff 与提交结果另存本地证据，不写入业务文件。
- 时间盒：20 分钟；停止条件：精确 14 文件审查通过、本地提交成功，其他规划改动和本地证据保留，不自动进入后续任务。

## Feature 完成与交付检查点

- 2026-10-27：按最新“继续实施直至完成”授权完成 T4 和整项验收；[归档规格](archive/feat-sql-workflow-guidance.md) 的 T1～T4 全部勾选，Backlog 目标条目标记完成。
- 用户入口：[SQL 采集、分析与输出指引](../docs/sql-workflow.md)；README 与四篇专业说明均已链接，愿景使用当前语法描述主链路，保留旧 SQL/SDK、模型和任务生命周期的规划示意并就近标注。
- 构建：标准 src 配置及 `sql_workflow_runtime test_framework test_scheduler_e2e test_baseliner_operator test_npm_dns_module` 均通过，无 Error；本轮未修改 C++，Python AST、依赖 JSON、Diff/允许文件与空白检查通过。
- 最终冻结清单：`evidence/test-environment/20261027T130719Z-df5174b2/summary.json`，selected=executed=passed=12，failed=skipped=not_executed=0；8 项实际检查与 4 项必需环境 fixture，无删除、跳过或放宽必需断言。
- G1/G2 发现与入口、G3/E01～E04 有限主链路、G4/E05/E06 编排与结束、G5 文档一致性均在当前源码/文档下复验；22 个原文请求、24 个唯一文档块、50 个相关链接/锚点核对通过。
- 11 个 Feature 源码/文档、24 个运行产物 SHA256、构建身份与覆盖映射在 `evidence/sql-workflow-guidance/feature-completion.json`；标准 runner 的源码/产物清单与 Feature 文档摘要在验收前后完全一致。证据由 Git 忽略，未纳入提交。
- 边界：隔离原生进程、真实 `.so`、CSV/SQLite/固定 PCAP 与受控 Stream/provider；C01～C05 仍是未执行的真实设备/Kafka 条件模板，不声明外部认证、数据库/容器矩阵或产品全量通过。
- 完成后停止：未进入后续 Feature，未提交或推送；未改用户服务、设备、Broker、凭证或私有运行配置。

以下保留各阶段冻结与检查点，仅用于追溯，不代表当前待办。

## T4 当前切片冻结：统一入口与整体验收

- 最新用户授权：继续实施直至 Feature 完成，不在切片之间中止；此授权覆盖前序暂停和切片完成即停记录。仍逐项验收、WIP=1，不提交或推送。
- 关联 Feature Task / Atomic Slice：T4；业务意图：统一 README、愿景和专业说明入口，使用户可辨别当前可执行主链路与规划，并用当前文档复验整个 Feature。
- Non-Goals：不修改产品 SQL/API/ABI、调度或业务实现，不扩展示例能力；不操作用户服务、设备、Broker、凭证、外部数据库或容器矩阵；专业说明仅补交叉引用及经核对的必要冲突。
- 允许修改文件：`tasks/active_task.md`、`tasks/product_backlog.md`、`tasks/specs/feat-sql-workflow-guidance.md` 及其归档目标 `tasks/archive/feat-sql-workflow-guidance.md`、`README.md`、`docs/vision.md`、`docs/npm.md`、`docs/baseliner.md`、`docs/kafka.md`、`docs/netadapter.md`、`docs/sql-workflow.md`、`src/tests/test_sql_workflow/CMakeLists.txt`、`src/tests/test_sql_workflow/test_sql_workflow.py`、`config/testing/dependencies.json`。保留前序 `src/CMakeLists.txt` 修改；证据仅写 `evidence/sql-workflow-guidance/`。
- 冻结契约：复用现有 API/ABI；新增离线 G5 文档检查 phase，核对入口链接/锚点、文档块 ID 与实际请求调用集合、显式占位符、条件模板及愿景规划标识。文档可执行样例仍由现有三个真实运行 phase 读取执行。
- 验收命令：`cmake -B build src`；`cmake --build /mnt/d/working/flowsql/build --target sql_workflow_runtime test_framework test_scheduler_e2e test_baseliner_operator test_npm_dns_module -j2`；`tools/test.sh all --location native --filter '^(test_sql_workflow_docs|test_sql_workflow_discovery|test_sql_workflow_finite|test_sql_workflow_stream|test_framework|test_scheduler_e2e|test_baseliner_operator|test_npm_dns_module)$' --jobs 1`；Python AST、依赖 JSON、`git diff --check`、允许范围及归档链接检查。
- 选择依据：docs 覆盖 G5；三个 workflow phase 完整重跑 G1～G4/E01～E06；framework 核验 Parser，scheduler 覆盖持续 provider/编排与结束，NPM DNS/Baseliner 覆盖示例所用共享契约。仅引用专业说明不增加外部矩阵。
- 依赖、位置与权限：仓库根目录、真实 `build/output` 产物、临时 CSV/SQLite/PCAP、受控 mock/ring/provider、随机 loopback 端口；services=[]，统一运行时测试配置及既有资源锁，无外部凭证。HTTP 运行和 esbuild 子进程按环境需要在沙箱外执行。
- 时间盒：30 分钟；检查点：入口、规划标识和 G5 离线检查完成后执行完整冻结清单。时间盒到期保留同一 T4 编号并记录检查点，按最新连续授权继续，不扩范围。
- 停止条件：T1～T4 全部通过，selected=executed=passed 且 failed=skipped=not_executed=0，源码/产物身份和验证边界记录齐全；勾选 T4、归档规格、更新 Backlog，恢复 WIP=0 后结束本 Feature。

## T3 完成检查点

- 2026-10-27：交付 E05 的有限 Stream single 与持续 ring status/stop，以及 E06 的纯 Stream / mixed 两组完整 DAG；新增 12 个文档请求块由测试直接读取，不另写等价成功 SQL。
- 实测断言：single 输出 6 行及完整六列 Schema；无输入持续 ring 正常 stop，处理/输出均为零；纯 Stream 两节点各处理 6 行、输出 clientPort=10000～10005，正常 stop 后 stopped；mixed 同时包含 batch/stream 节点，输出 CSV 原始 3 行并完成。
- 真实边界：纯 Stream 组的有限第一节点 EOF 不自动关闭 finite=false 的中间 ring；首轮因错误预期自动终止而失败（6/7），依据运行节点状态及既有线性组回归，修正文档为核验输出后显式 Stop，保留严格计数和最终状态断言，未改产品实现或增加超时。
- 持续 block 的异步准入由 discovery 验证；运行、history 和正常 Stop/Cancel 由既有受控 Capture provider 回归验证。C01～C05 明确列出 NetAdapter 与 Kafka 四类适配器的方向、契约、权限、结果和新 run 边界，均为未执行的 conditional_template。
- 构建：`cmake -B build src` 及冻结四个目标均通过；沙箱 esbuild EPERM 后，按授权在沙箱外完成标准构建，日志 `evidence/sql-workflow-guidance/build-t3-escalated.log`。无新增/修改 C++ 源文件，前端既有体积提示不作本 Feature 改动。
- 最终标准验收：`evidence/test-environment/20261027T125056Z-95ce76e4/summary.json`，selected=executed=passed=7，failed=skipped=not_executed=0（5 项测试及 2 项必需 fixture）；命令、首轮失败和最终日志均缓存在 Feature 证据目录。
- Python AST、依赖 JSON、24 个唯一文档块/22 个请求对应关系、占位符声明、15 个相对链接、Diff/允许范围和源码/产物摘要已核对；证据未入库。
- 停止：T3 已勾选，工作台 WIP=0；按单切片完成即停规则不进入 T4，Feature 保持进行中、不归档。未提交或推送，未触碰用户服务、设备、Broker、凭证或外部部署。

## T3 切片冻结记录：Stream 与持续来源边界

- 关联 Feature Task：T3；业务意图：让用户用受控本地来源验证 Stream single、纯 Stream Group DAG 与 mixed Group DAG，并能观察状态、结果和正常结束边界。
- Non-Goals：不修改产品 SQL/API/ABI、调度实现或外部适配器；不验证真实网卡、Kafka Broker、认证、外部数据库或容器部署；不进入 T4 文档统一收口。
- 允许修改文件：`tasks/active_task.md`、`tasks/specs/feat-sql-workflow-guidance.md`、`docs/sql-workflow.md`、`src/tests/test_sql_workflow/CMakeLists.txt`、`src/tests/test_sql_workflow/test_sql_workflow.py`、`config/testing/dependencies.json`；保留已有 `src/CMakeLists.txt` 和前序修改，证据仅写入 `evidence/sql-workflow-guidance/`。
- 冻结验收：构建 `sql_workflow_runtime test_scheduler_e2e test_baseliner_operator test_npm_dns_module`；执行 `tools/test.sh all --location native --filter '^(test_sql_workflow_stream|test_sql_workflow_discovery|test_scheduler_e2e|test_baseliner_operator|test_npm_dns_module)$' --jobs 1`；另运行 Python AST、文档块/占位符检查、`git diff --check` 与允许范围核对。
- 选择依据：新增 stream 直接执行 E05/E06；discovery 核验持续来源 sync 拒绝及 G2；Scheduler 既有受控 Capture provider 核验持续异步运行、history 与 Stop/Cancel，并覆盖 Stream/mixed 编排共享契约。Baseliner/NPM 回归沿用本轮初冻清单，不增加外部依赖或矩阵。
- 依赖与执行位置：从仓库根目录使用真实原生 `.so`、隔离 CSV/SQLite、`tcp_session_mock` 与 ring 受控 Stream、随机 loopback 端口；services=[]，统一测试配置，无外部凭证。
- 时间盒：30 分钟；停止条件：E05/E06 文档请求被测试直接读取并执行，single/group/mixed 的状态、节点类型、结果行数和正常 stop/EOF 断言通过，相关回归全绿；随后勾选 T3、恢复 WIP=0 并停止，不自动进入 T4。

## T2 完成与暂停检查点

- 2026-10-27：T2 完整验收通过，规格已勾选；T1/T2 已完成，Feature 保持进行中，不归档。
- 已交付 E01～E04：文档原文 SETUP、12 个唯一准备/请求块、明确替换项与结果核验步骤；测试读取这些原文执行，不另写等价 SQL。
- E01 三行 CSV 双算子独立 WITH 原样输出；E02 SQLite 原生 WHERE/SUM 输出一行字符串 "5"；E03 固定 DNS PCAP 的 Basic/Session 各一个双向会话，包数各方向 1，托管三实体各 1 行，DNS matched/rcode=0/延迟 10 ms；E04 40 行 evaluation、80 行 forecast、预测范围与 NULL 语义、非空模型出口均核验通过。
- 构建通过：`sql_workflow_runtime test_scheduler_e2e test_baseliner_operator test_npm_dns_module`；日志 `evidence/sql-workflow-guidance/build-t2.log`。无新增 C++ 源文件，既有 Arrow 头文件的 unused-parameter 编译警告不属于本次新增代码。
- 主标准验收：`evidence/test-environment/20261027T110032Z-482e0cf1/summary.json`，selected=executed=passed=5，failed=skipped=not_executed=0。
- 共用 SETUP 的 discovery 回归：`evidence/test-environment/20261027T110339Z-171d4c62/summary.json`，selected=executed=passed=2，failed=skipped=not_executed=0；两份清单无重叠，T2 必需范围共 7/7/7。
- 静态检查通过：Python AST、依赖 JSON、文档块 ID/占位符、相对链接文件、Diff 与允许范围；Backlog 与本轮前快照相比只有目标 Feature 的进行中状态变化，保留前序规划。
- 实测限制已写入指引：SQLite 聚合表达式当前返回字符串；普通 DataFrame preview 数值 NULL 显示为零，E04 因此从 SQLite 核验真实 NULL；Web 摘要不透传完整 NPM managed_result，E03 通过运行元数据定位 run_id。仅记录现状，不在此 Feature 修改产品实现。
- 验证仅覆盖隔离原生进程、真实插件与本地 CSV/SQLite/PCAP；未验证真实设备、Broker、认证、外部数据库或容器矩阵。T3/E05/E06/G4 与 T4/G5 尚未实施，不声明 Feature 完成。
- 停止状态：按用户“完成 T2 后暂停”执行，未进入 T3/T4；未提交或推送，保留全部已完成修改与本地证据。

## T1 验收与 T2 切片冻结

- 最新用户指令：完成 T2 后暂停；T2 验收并勾选后恢复 WIP=0，不进入 T3/T4，不归档 Feature。
- T1 已完整验收：标准构建通过；`20261027T104247Z-7d04fd5b/summary.json` selected=executed=passed=3，failed=skipped=not_executed=0；G1/G2 的目录/懒加载/激活/分类/错误入口与 test_framework 回归通过。规格已勾选 T1。
- 关联任务 / 当前 Atomic Slice：T2；交付 E01～E04 的实际 SQL/请求与完整准备步骤，测试直接读取文档块并核验结果。
- 业务意图：用户可从 CSV、SQLite、固定 DNS PCAP 获得确定输出，再独立执行 Baseliner 快照分析。
- Non-Goals：不进入 T3/T4，不改产品业务实现、公共接口或运行配置；不测试外部数据库、设备、Broker。
- 允许文件：沿用 Feature 管理文件、`docs/sql-workflow.md`、`src/tests/test_sql_workflow/CMakeLists.txt`、`src/tests/test_sql_workflow/test_sql_workflow.py`、`config/testing/dependencies.json`；证据目录不入库。`src/CMakeLists.txt` 不再需要修改。
- 冻结验收：构建 `sql_workflow_runtime test_scheduler_e2e test_baseliner_operator test_npm_dns_module`；通过 `tools/test.sh all --location native --filter '^(test_sql_workflow_finite|test_scheduler_e2e|test_baseliner_operator|test_npm_dns_module)$' --jobs 1` 验证 G3 与所用 Scheduler/NPM/Baseliner 回归；Python AST、Diff/范围检查。
- 依赖：真实 NPI/PCAP/NPM `.so`、协议词典、Flow Labeling provider、Baseline 与 Baseliner 插件；隔离 SQLite/CSV/PCAP、Python 标准库、随机 HTTP 端口，services=[]，专用 sql_workflow 锁；统一测试配置，无外部凭证。
- 补充必需回归：T2 将 Runtime 的准备流程改为读取文档 SETUP，discovery 共用该流程，因此在上述 5/5 标准验收后另运行 `tools/test.sh all --location native --filter '^test_sql_workflow_discovery$' --jobs 1`，确认 T1 发现契约未受影响；不扩大到无关模块。
- 时间盒：30 分钟，从 T1 验收后开始；停止条件：E01～E04 原文请求全部执行，关键 Schema/实体/计数/数值与失败边界通过，回归全绿，勾选 T2 并按最新指令暂停。

## Feature 连续实施授权与当前切片

- 用户授权：实施本 Feature 直至 T1～T4 全部完成，不在任务之间中止；按顺序验收、勾选后更新工作台进入下一项。不提交或推送。
- 关联 Feature Task / 当前 Atomic Slice：T1；建立统一指引的能力发现、语法与入口部分，以及隔离原生 HTTP 验收入口。
- 业务意图：用户能查到真实通道与算子状态，按分类选择入口，辨别目录存在、激活与可执行的差异。
- Non-Goals：沿用规格；本切片不编写 T2/T3 示例，不修改产品 SQL/API/ABI、业务实现、用户配置或服务。
- 允许修改：`tasks/active_task.md`、`tasks/product_backlog.md`、`tasks/specs/feat-sql-workflow-guidance.md`、`docs/sql-workflow.md`、`src/tests/test_sql_workflow/CMakeLists.txt`、`src/tests/test_sql_workflow/test_sql_workflow.py`、`src/CMakeLists.txt`、`config/testing/dependencies.json`；本地证据在 `evidence/sql-workflow-guidance/`，保留本轮前已有修改。
- 冻结验收：`cmake -B build src`；构建 `flowsql flowsql_builtin flowsql_catalog flowsql_database flowsql_stream flowsql_scheduler flowsql_task flowsql_web flowsql_router flowsql_binaddon flowsql_npi flowsql_pcapfile flowsql_netadapter flowsql_config_channel test_framework`；`tools/test.sh all --location native --filter '^(test_sql_workflow_discovery|test_framework)$' --jobs 1`；Python AST、`git diff --check` 与范围核对。
- 验收边界：新增 discovery 覆盖 G1/G2 的真实插件目录、激活状态、SQL classify/analyze 参数及错误入口；Parser/阶段过滤共享契约由 test_framework 回归。T2/G3 与 T3/G4 后续再加入，不提前验收。
- 依赖与执行位置：原生主 build/output、Python 标准库、临时 SQLite/CSV/随机 loopback 端口，services=[] 与专用资源锁；统一测试配置来自 config/testing/environment.json/dependencies.json。无外部 DB/Kafka/网卡或凭证；socket 被沙箱禁止，已获准在沙箱外执行本地 HTTP 验收。
- 时间盒：30 分钟，2026-10-27 09:56 UTC 起；停止条件：冻结清单全绿，更新 T1 证据与勾选；按本次连续授权重新冻结 T2 后继续。

## 前序规格编写记录（保留）

### T1 首轮检查点与继续切片

- 首轮时间盒到期：当前错误待修复。原生配置已通过；标准构建的 esbuild 子进程被沙箱 EPERM 阻止，已获准在沙箱外重跑。尚未完成冻结验收，不勾选 T1。
- 原生运行已验证 DataFrame 懒加载元信息与部分目录；准备参数根据接口修正为 block role=source、合法短网卡名和至少覆盖共享 envelope 的缓冲预算；不修改产品实现或放宽断言。
- 继续 T1，允许文件与 G1/G2 边界不变；构建补列 fixture 所需 flowsql_npm_basic、flowsql_capture_af_packet，仍不打开真实网卡。
- 继续时间盒：30 分钟，2026-10-27 10:32 UTC 起；停止条件仍为 T1 完整清单通过，之后按连续授权冻结 T2。

## SQL 工作流指引规格编写边界

- 用户授权：编写 `sql-workflow-guidance` Feature 的规格。
- 关联 Feature Task：无；本轮交付 Feature 规格，不执行规格内的实施任务。
- 当前 Atomic Slice / 业务意图：依据 Backlog 目标及当前代码、使用说明，明确 SQL 采集、分析和输出的能力发现、执行入口、可运行示例与实际边界，形成可实施、可验收的精益规格。
- Non-Goals：不实现或修复功能，不改 SQL 语法、公共接口、运行配置、愿景或使用说明；不进入数据字典、诊断、长期恢复等后续 Feature，不提交或推送。
- 允许修改文件：`tasks/specs/feat-sql-workflow-guidance.md`、`tasks/product_backlog.md`、`tasks/active_task.md`；保留后两者已有未提交内容与前序工作台证据。
- 冻结范围：新建一份含业务意图、Non-Goals、最小数据契约、至多两条主链路及结果导向任务的规格；Backlog 仅补规格链接及同步“规格待创建”的概述，维持待办状态和既有优先顺序。
- 验收命令：`git diff --check`、`git diff --name-only`、规格非空行统计；内联 Python 检查相对链接、任务编号与状态、必要契约和测试锚点，并与 `/tmp/flowsql-sql-workflow-guidance-20261027/` 快照核对 Backlog 其他内容及前序工作台记录未变。
- 选择依据与依赖：纯 Markdown 规格，仓库根目录执行静态与 Diff 检查；只读核查语法、目录与执行实现，无外部服务、凭证、构建或业务测试依赖。未来功能验收范围写入规格，具体命令在实施切片冻结。
- 时间盒：20 分钟，2026-10-27 09:27 UTC 起。
- 停止条件：规格范围和任务语义检查通过、链接及 Diff 无误，记录证据并恢复 WIP=0，停止本轮，不自动开始 T1。

## SQL 工作流指引规格完成证据

- 新增 `tasks/specs/feat-sql-workflow-guidance.md`：业务意图、Non-Goals、当前依据、复用接口与示例记录契约、两条主链路、T1～T4 结果导向任务及 G1～G5 测试锚点；正文在完成证据前为 119 个非空行。
- 已静态核查 Parser、Web、Task、Scheduler 与当前专业说明，明确分类目录、sql/sql_text 请求字段、batch/stream/mixed、持续 block 异步入口、阶段过滤和 NPM/Baseliner 输出衔接边界；不将代码检查计为运行验收。
- E01～E06 规划本地有限数据与 Stream 编排验证；真实 NetAdapter 与 Kafka 示例标为条件模板，受控 provider 验证不外推为设备、Broker、认证或部署验证。
- 一轮边界审查完成：四项任务围绕同一指引交付收口，未引入新 SQL/API/ABI 或后续 Feature 实施；全部任务保持未勾选，G1～G5 与示例尚未执行。
- 静态内容检查通过：11 个相对链接及 README 锚点有效，任务编号、状态、两条主链路、示例与测试锚点、代码围栏和规格尺寸符合本轮冻结范围。
- 与本轮前快照逐字核对通过：Backlog 仅目标规格链接及概述变化，待办状态、优先顺序及其他内容保持原样；前序工作台记录完整保留。
- `git diff --check`、新增规格空白检查与最终范围核对全部通过；仅修改允许的三个 Markdown 文件，索引为空。纯规格修改未运行 CMake 或业务测试，未提交/推送，停止本轮。

## 前序目录维护记录（保留）

以下为前序维护检查点，本轮未复验，不将其标记为整体完成：Codex CLI 误判已修复，进程识别回归与静态检查通过；实际目录迁移和新路径验收仍待用户手动执行，整体维护任务未完成。

## 意图与边界

- 用户授权：项目目录 flowSQL 调整为 flowsql；涉及路径全部修改；2026-10-26 明确历史材料旧目录路径不处理、保留原状。
- 当前 Atomic Slice：交付小写项目根目录及可继续使用的构建、启动、调试路径，更新当前有效说明。
- 业务意图：开发与部署从 `/mnt/d/working/flowsql` 使用项目，避免目录大小写和路径约定不一致。
- Non-Goals：不改主 Web、探索前端、C++/Python 展示，不改 CMake 目标名、凭证、协议、ABI 或业务逻辑；不改历史材料、历史日志、evidence 既有记录及 Git 历史；不提交/推送，不停止或重启用户服务。
- 时间盒：30 分钟；2026-10-26 06:51 UTC 开始；到期记录检查点，未完成时沿用本维护任务。
- 停止条件：根目录项为 flowsql；当前有效路径无旧根名；新路径配置/相关主产物构建/冻结测试通过；历史与展示文件摘要不变；WIP=0。

## 允许修改文件与操作

- `tasks/active_task.md`。
- 当前规则/用户说明：`AGENTS.md`、`CLAUDE.md`、`README.md`、`docs/framework.md`、`docs/kafka.md`、`docs/netadapter.md`；仅目录名称和路径字面量。
- 本地忽略配置：`.vscode/settings.json`、`.vscode/launch.json`；仅项目根路径。
- 本地构建与依赖元数据：`build/`、`.thirdparts_prefix/`、`.thirdparts_installed/` 内含旧根路径的生成配置/脚本/链接元数据；只迁移精确根路径，不改依赖源码和二进制，不删除目录。迁移清单记录在 `evidence/project-path-lowercase/`。
- 如发现当前运行配置内有旧根路径，先列明文件与影响并更新工作台；不直接修改数据库或持久业务文件。
- 根目录改名：从父目录经唯一临时目录项将 `flowSQL` 改为 `flowsql`；不删除目录；之后所有命令显式使用新目录。
- 本地忽略证据：`evidence/project-path-lowercase/`；不入库。
- 初始工作区干净、索引为空；HEAD 与全部跟踪文件摘要在 before.json。每次 patch 后核对 git diff --name-only。

## 冻结契约与验收

- 只调整项目根路径段，保留 FlowSQL/flowSQL 展示、凭证、内部标识及历史材料字节；既有生成二进制的调试历史路径不做二进制替换，新构建采用新路径。
- 目录迁移不重置 `.thirdparts_installed` / `.thirdparts_prefix`，不迁移至另一份 worktree、不改远端。
- 配置：`cmake -B /mnt/d/working/flowsql/build /mnt/d/working/flowsql/src`；显式使用小写绝对路径，实际目录项迁移另行核验。
- 构建：`cmake --build build --target flowsql flowsql_catalog flowsql_scheduler flowsql_web flowsql_visualization test_native_deploy_config -j2`；覆盖默认启动主产物和路径敏感配置，复用既有依赖缓存。
- 验收：`tools/test.sh all --location native --filter '^(test_native_deploy_config|test_product_version_runtime|test_visualization_dataframe_native)$' --jobs 1`。
- 选择依据：原生配置/启动脚本/产物、C++/Python CLI 与 HTTP 版本入口、含 Scheduler 的主产物 DataFrame 恢复/SQL/快照路径。界面和业务源码未修改，不增加无关 DB/Kafka/Docker/全量。
- 依赖：新根目录的主 build/output、本地 Python、隔离 SQLite/CSV/随机 loopback 端口与现有 services=[] fixture；统一 environment.json/dependencies.json，不需外部服务或凭证。
- 必需 HTTP 测试若沙箱禁止监听，保留完整清单并记录未执行；当前环境自动审批拒绝外层执行，不减少断言或跳过。
- 核对 selected=executed=passed，failed=skipped=not_executed=0；相关源码、历史材料、展示文件保持不变；检查目录项、缓存/编译数据库、IDE 路径、主产物路径及 git diff --check。

## 当前状态

- 已完成只读影响分析；用户确认历史材料完全保留，展示保持原样。
- 沙箱隔离了进程视图，沙箱内未见服务不能证明宿主服务已停止；两次宿主只读核验申请被自动审批拒绝（策略禁止 require_escalated），不据此移动目录或停止/恢复服务。
- 根目录改名需要父目录写权限；当前仅仓库与 /tmp 可写。先完成仓库内修改、生成元数据及可运行验证，真实目录项迁移保留为环境阻塞。
- 已建立 4078 个生成元数据文件、119111 处根路径替换的精确清单。小写路径配置遇 EROFS 后，已按原 SHA256 恢复全部生成文件，保持现有构建可用；清单在 metadata-migration.json，恢复结果在 metadata-restoration.json。
- 小写路径配置失败：沙箱仅授权旧大小写路径，新路径写 build-info.json/CMakeCache.txt 返回只读文件系统；日志 configure.log。
- 本地 loopback bind 返回 EPERM，必需 HTTP 验收无法在当前沙箱执行；宿主核验提权申请被自动审批拒绝，不重复申请或绕过限制。
- 保持完整验收门槛；环境阻塞期间仅执行现有路径的原生配置及无监听 CLI 检查点，不将其等同于新目录验收。

## 环境受限检查点

- 必需的目录迁移、新路径配置构建及 HTTP 验收仍未完成，不标记整个维护任务完成。
- 临时检查：`cmake --build build --target test_native_deploy_config -j2` 与 `tools/test.sh unit --location native --filter '^test_native_deploy_config$' --jobs 1`，仅证明现有路径的部署契约未变。
- C++ 与 Python `--version` 无监听检查，必须保留原 FlowSQL 展示；不改用户服务。

- 本地权限实测：父目录 `/mnt/d/working` 不可写，旧根可写，小写根不可写；未执行目录移动。

## 本轮最终证据与继续条件

- 当前有效路径已更新：6 个跟踪规则/说明文件和 2 个忽略的 IDE 配置；仅修改路径，未改凭证、展示或内部标识。
- 162 个历史材料文件（archive/sprints/docs archive）及全部 1002 个 src 文件 SHA256 保持不变；初始工作区无其他修改。
- 本地生成元数据迁移试验已全部按原 SHA256 恢复；保留依赖缓存，不删除目录。现有路径重新配置/构建 test_native_deploy_config 成功。
- 首轮检查点测试与构建重叠被 runner 判为 artifact_changed，未计通过；构建结束后复测通过，最后以显式 native 位置再次核对。
- 最终原生单测：evidence/test-environment/20261026T070824Z-b60e5d58/summary.json，selected=executed=passed=1，failed=skipped=not_executed=0。
- C++ flowsql、flowsql-worker、python3 -S -m flowsql.worker 三入口版本输出检查通过，保留 FlowSQL 品牌；cli-checkpoint.json。
- 尚未执行根目录改名、新根路径配置构建、test_product_version_runtime 和 test_visualization_dataframe_native；完整迁移验收未通过，不标记完成。
- 环境需要允许父目录写入以改名、允许 /mnt/d/working/flowsql 写入，以及本地 HTTP/loopback 验收。当前自动审批拒绝 require_escalated，未绕过。
- 当前实际目录项仍为原大小写；不停止/重启用户服务，不提交/推送。后续沿用本维护任务并先核验宿主运行状态，再完成冻结迁移与验收。

## 继续执行切片

- 用户再次授权：继续执行根目录改名及既定路径迁移，范围沿用原冻结清单；不新增业务改动、不提交/推送。
- 时间盒：30 分钟；2026-10-26 07:12 UTC 开始。
- 先申请父目录/新根路径执行权限，核验宿主是否存在服务，再经临时名称完成大小写改名；成功后迁移当前生成元数据，并在新路径执行原冻结配置、构建和验收。
- 当前无权限修改父目录、新根路径或监听端口；未取得执行权限前不再迁移生成元数据，避免重复上轮回退。

## 继续执行结果

- 用户“继续执行”后重新核验：父目录与小写根路径依旧不可写，loopback bind 依旧 EPERM。
- 为已明确授权的目录改名提出精确执行申请（先检查宿主服务、无服务才临时名中转，失败回滚），自动审批在创建进程前拒绝；实际改名脚本未执行。
- 审批明确认可用户授权足够，拒绝原因是 require_escalated 与父目录写入不符合当前环境硬限制；不能用替代路径绕过。
- 本轮未重新迁移生成元数据、未重复构建/测试；保留前轮现有路径检查点，未将其计为新路径验收。
- 等待执行环境允许 /mnt/d/working 的目录改名、/mnt/d/working/flowsql 写入及本地监听后继续；无需再次确认业务范围。

## 手动执行脚本准备切片

- 用户要求：提供所有需要执行的命令，由用户在普通 WSL 终端手动执行；不再申请提权或尝试目录改名。
- 关联任务：沿用本项目目录小写维护任务，不新增 Feature Task。
- 当前 Atomic Slice / 业务意图：交付可审查、可重入的迁移脚本，让用户完成剩余目录改名、生成路径迁移和新路径验收。
- Non-Goals：沿用原范围；不执行脚本的 apply 模式，不停止用户服务，不删除构建/依赖缓存，不修改历史、源码、凭证或展示，不提交/推送。
- 允许修改：仅 `tasks/active_task.md` 与忽略目录 `evidence/project-path-lowercase/` 内新增手工脚本、说明及静态验证结果；保留先前六个规则/说明文件的路径修改及两个 IDE 修改。
- 脚本契约：检查宿主服务并在有服务时停止操作；经唯一临时名改目录；修改前保存元数据原字节；精确迁移旧根路径；顺序配置、构建、测试；核对真实目录项、选中/执行/通过集合与历史/源码摘要；错误非零退出并保留日志，不自动标记维护完成。
- 用户手动构建清单：在原清单基础上补 `flowsql_builtin flowsql_database flowsql_router`，它们由已冻结的 DataFrame 与版本 HTTP 验收实际加载；不扩大测试边界。
- 手工元数据清单：在原 4078 条清单上补入后续检查点构建新生成的 8 个 `compiler_depend.make/internal`；另存 `manual-generated-paths.json`，不覆盖既有历史证据。
- 用户手动验收仍为原三项测试及 CTest 必需 fixture；从 native 位置执行，使用现有统一测试配置，无外部 DB/Kafka/Docker 依赖。
- 本切片验收：Python AST/`--help`、只读计划检查、`git diff --check` 与范围核对；不把静态检查计为真实迁移验收。
- 时间盒：30 分钟，2026-10-26 07:19 UTC 起；停止条件：脚本及完整命令交付、静态检查通过，工作台记录 WIP=0/等待用户执行。

## 手动执行脚本交付结果

- 本切片已完成：`evidence/project-path-lowercase/manual-migrate.sh` / `manual-migrate.py` / `MANUAL.md`，均为本地忽略文件，不入库。
- Python AST、Shell 语法、`--help` 与无参数计划模式通过；没有调用 `--apply`，没有目录移动、生成元数据替换或新路径测试。
- 只读核对 1248 个跟踪文件：除原冻结规则/说明与工作台以外均匹配初始摘要；历史材料、全部 src 文件保持不变。
- 手工清单共 4086 条，其中 4078 个文件当前存在，8 个已由 CMake 移除的临时依赖文件记录为不存在，不作为测试跳过。
- 当前生成元数据补扫发现的 8 个新增依赖文件全部已纳入手工清单；备份和精确路径替换只在用户执行 apply 时发生。
- 复用标准 runner 只读选择，冻结三项业务测试与 `test_visualization_dataframe_native__environment`；此处只核对清单，不计为执行或通过。
- 静态结果：`manual-static-validation.json`；当前补扫：`manual-metadata-scan.json`。整体迁移必须等待用户执行后的 `manual-*/result.json` 与标准测试 summary 才能收口。
- 交付命令从 `/mnt/d/working` 执行；用户先保存编辑内容并停止项目服务。成功后从 `/mnt/d/working/flowsql` 重新打开工作区；没有提交/推送、没有启动常驻服务。

## Codex 进程误判修复切片

- 事实依据：用户宿主输出确认 PID 98583 为 `/root/.nvm/versions/node/v25.6.1/bin/node`，工作目录是旧根，入口为同前缀的 `bin/codex`。原检测将所有项目工作目录内的 Node 进程列为服务，导致执行在任何迁移之前停止。
- 关联任务 / 业务意图：沿用项目目录小写维护；让用户在保持 Codex CLI 运行时完成手动迁移，继续阻止真实项目服务和构建并发。
- 当前 Atomic Slice：仅修复已确认的 Codex CLI 误判，复验进程检查和静态入口。
- Non-Goals：不取消进程检查、不按 PID 硬编码放行、不忽略全部 Node、不停止服务、不运行 apply、不改源码/历史/构建元数据、不提交/推送。
- 允许文件：`tasks/active_task.md`、`evidence/project-path-lowercase/manual-migrate.py`、`evidence/project-path-lowercase/MANUAL.md`，以及新增 `manual-process-validation.json` 本地证据；其余既有 diff 保持原样。
- 最小方案与风险：只放行通过 Node 启动、入口名为 codex 且真实入口位于项目根之外的 CLI；项目内同名入口继续拦截。风险边界是入口识别，使用用户复现与正反场景验证，其他进程规则不变。
- 冻结验收：以模拟 `/proc` 记录复现用户场景，并验证项目 Node/Vite、npm、CMake、C++ 服务、Python Worker 仍拦截，以及无关 Node/项目内同名 codex 边界；Python AST/`--help`、Shell 语法与默认计划、`git diff --check`/范围核对。不需要 CMake 或外部服务，不运行真实迁移。
- 时间盒：20 分钟，2026-10-26 07:35 UTC 起；停止条件：上述回归与静态检查通过，WIP=0，交付相同手工重跑命令。

## Codex 进程误判修复结果

- 先以模拟 `/proc` 复现用户报告：原实现误拦截普通 Codex CLI 和带 Node 参数的 Codex CLI；其余 9 个正反场景符合预期。
- 修复仅位于 Node 入口识别：入口名必须为 codex、绝对入口路径与真实文件均在项目根之外；其他 Node、项目内同名入口、实际构建与服务进程规则不变，不按 PID 豁免。
- 修复后 11 个进程识别回归场景全部通过，包括用户复现、Node 参数、项目 Vite/npm/CMake/FlowSQL/Python Worker、无关 Node、项目内 codex 和把 codex 当普通参数的边界。
- Python AST、`--help`、Shell 语法和默认计划模式通过；`git diff --check` 与允许范围检查通过，索引保持为空。
- 证据：`evidence/project-path-lowercase/manual-process-validation.json` 保留修复前后结果；这是进程识别回归，不是整体迁移验收。
- 未执行 apply、未移动目录、未修改生成元数据、未结束任何进程、未提交/推送；用户可以保持本 Codex CLI 运行，重跑原迁移脚本。
