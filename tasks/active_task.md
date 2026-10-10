# 即时工作台

WIP=0。baseliner-contract-safety 规格切片已完成并停止；T1～T4 尚未实施，已有运行代码及其他未提交改动保持。

## 当前 Atomic Slice

- 关联 Feature：[Baseline/Baseliner 生命周期与评估契约修复](specs/feat-baseliner-contract-safety.md)；本切片为实施前契约冻结，T1～T4 均未开始。
- 当前切片：补充 Feature、精益规格和验收锚点，明确两项 P1 与类型化评估就绪状态的最小修复范围。
- 业务意图：Scheduler 调度的 Baseline 任务在停机/取消时安全释放资源，算子通过算法契约识别有效的事前基线。
- Non-Goals：本切片不修改运行代码/测试/公共头；不处理结果发布取消空窗、通用 DataFrame 查询/CSV 展示、常规去重或算法数值调整；不提交/推送。
- 公共契约冻结：保持现有 IPlugin、Baseline 任务虚表和结果结构布局；新增独立版本化评估能力；就绪状态随同一次提交结果返回，Relation 按 routed result 表达。
- 允许文件：`tasks/active_task.md`、`tasks/product_backlog.md`（仅新增该 Feature 行）、`tasks/specs/feat-baseliner-contract-safety.md`；临时证据 `/tmp/baseliner-contract-safety-spec/`。
- 验收命令：每次 patch 后 `git diff --name-only`；`git diff --check`；`awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/specs/feat-baseliner-contract-safety.md`；`python3 /tmp/baseliner-contract-safety-spec/verify.py`（新增文档空白、相对链接、任务层级、独立 C++17 接口片段语法及起始 hash 范围检查）。本切片不运行构建/CTest，不以既有测试证明修复。
- 时间盒：20 分钟，从 2026-10-10 21:32（Asia/Shanghai）开始；到期到达明确检查点并停止，不扩大文件范围延长。
- 停止条件：三项修复的契约、边界、兼容策略及回归锚点可独立审查，文档检查通过后更新 WIP=0 并停止；不勾选运行修复任务，不自动开始 T1。
- 起始快照：`/tmp/baseliner-contract-safety-spec/before-hashes.json`（979 个现存文件）、`before-status.txt` 和两份工作文档原文；仅对照本切片增量，保留既有修改/未跟踪文件。

## 规格切片完成证据与停止

状态：已完成。2026-10-10，Backlog 新增一个 Feature，规格含 4 个未勾选一级任务，完成证据前 124 个非空行。
完成一轮契约审查：停机排空先于算法 Close、最后会话所有者关闭、原虚表/结构布局保持、单次提交仅学习一次、
Relation 状态逐 routed result 对齐均有明确验收锚点；实现阶段仍需以确定性交错和兼容回归证明。
规格相对链接、任务层级、新文件空白、`git diff --check` 通过；接口片段使用当前公共头独立通过
`c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only`，仅证明声明可编译，不代表实现或 ABI 回归已通过。
起始 979 个文件中仅两份任务文档变化，另外新增该规格，977 个已有文件 hash 保持；旧工作台历史完整保留，
Backlog 除新增该 Feature 行外保持原文。证据：`/tmp/baseliner-contract-safety-spec/verification.json`、
`contract-syntax.log`、`diff-check.log`、两份本轮增量 diff 及前后 status。
本切片未修改运行代码、公共头或测试，未运行运行时构建/CTest，未提交/推送。下一实施任务为 T1；
按 AGENTS.md §1.4 及 flowsql-atomic-slice Procedure 7 完成即停，未自动进入下一切片。

## 本 Feature 跨任务问题隔离

- Scheduler 结果发布取消空窗（P2）：runner 解除 wake 后、DataFrame 登记前的取消未覆盖发布边界；前轮探针 `/tmp/baseliner-code-review/publication.log`。本 Feature 不处理。
- 通用 DataFrame SQL 查询/CSV 展示：沿用下方历史隔离记录，本 Feature 不处理。

## 前一 Feature 工作台（历史：baseliner-single-source T6）

WIP=0。T6 全部验收完成并停止；baseliner-single-source Feature 已归档，未提交/推送。

## 当前 Atomic Slice

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，T6。
- 当前切片：输入排序/转换与模型/结果出口成本、六项 Sanitizer、五轮重复、格式/严格编译、文档与 Feature 归档均完成。
- 业务意图：三种输入与两出口用户能复核数值等价、失败保证、使用条件和本次实际测量的成本，旧两段托管保证保持。
- Non-Goals：不改算法、运行代码、公共 ABI、SQL 语法或数据库后端；不修复已隔离的普通 DataFrame 查询/CSV 展示问题；不修改其他 Feature，不提交/推送。
- 公共契约冻结：沿用 T1～T5 接口和 Schema；仅成本测试新增输出测量字段。排序/转换合并测量 DataFrameReader::Open，真实模型导出计时覆盖只读导出与模型编码/目标暂存，峰值 RSS 用独立子进程，所有路径校验相同负载产物。
- 允许文件：`src/tests/test_baseliner_contract/test_baseliner_cost.cpp`；`docs/baseliner.md`、`docs/baseliner-acceptance.md`；`tasks/active_task.md`、原规格 `tasks/specs/feat-baseliner-single-source.md`、归档目的 `tasks/archive/feat-baseliner-single-source.md`、`tasks/product_backlog.md`（仅该 Feature 的状态/链接）；`/tmp/baseliner-t6-final/` 证据目录。构建产物由工具生成。
- 验收命令：`cmake --build build --target test_baseliner_cost -j4`；`ctest --test-dir build -R '^test_baseliner_cost$' --repeat until-fail:5 -V`；定向 ASan/UBSan/LSan（reader/operator/evaluation/store/lifecycle、拥有型查询），halt_on_error=1/detect_leaks=1；本 Feature clang-format-18、严格编译、独立公共头及增量 hash/Diff 审查；全量构建/CTest已有本轮基线，若产品代码变化则重新运行，新增成本目标最终重新验收且来源单列。
- 时间盒：30 分钟，从 2026-10-10 20:49（Asia/Shanghai）开始；到期到达可验证检查点并停止，不扩大范围延长。
- 停止条件：本切片全部锚点通过后更新工作台；只有 T6 全部契约/证据达到才勾选与归档。真实产品缺陷先保留日志并冻结最小修复范围；不提前更新完成状态。
- 起始快照：`/tmp/baseliner-t6-final/before/`、`before-hashes.json`；上轮完整基线证据 `/tmp/baseliner-t6-output/` 独立保留。

## T6 最终完成证据与停止

状态：已完成。当前源码完整 build 通过，日志无 warning/error；本 Feature 的全量 CTest 78/78 来源为前一切片，
生产实现/公共接口/其他测试 hash 保持；本轮更新成本目标单独五轮通过，test-provenance.json 明确来源，不合称单次运行。
旧四路径 + 新五路径共 45 个测量产物，960 条观测/5760 结果行一致；模型出口 24 行、最终桶 39，源保持、task_count=0。
六项当前 ASan/UBSan/LSan 通过（94 个仪器化翻译单元），54 实现/11 公共头严格编译、98 全文件 + DataFrame 修改区间格式通过。
旧格式/警告例外和普通依赖边界如实保留；文档三种配置经过真实解析器验证。生产 SQL/模型和结果独立出口用法、
排序/转换、峰值 RSS、模型导出/编码、发布及建表准备成本已写入 docs/baseliner-acceptance.md，不推导生产 SLA。
T6 已勾选，规格归档 tasks/archive/feat-baseliner-single-source.md，仅更新本 Feature 的 Backlog 状态/链接；其他工作保留。
证据 /tmp/baseliner-t6-final/：verification.json、test-provenance.json、cost-measurements/summary、sanitizer-results、
format-final、strict-results、doc-check、scope、before、diff；旧完整基线 /tmp/baseliner-t6-output/ 独立保留。
完成即停，不进入其他 Feature，不提交/推送。跨任务 DataFrame 查询/CSV 展示问题继续隔离，不声明已修复。

## T6 前一切片完成证据（历史）

状态：已完成。标准配置/完整构建已通过（配置 39.89s、构建 209.88s），日志无 warning/error。
独立单次完整 CTest 78/78 通过，882.49s；JUnit failures/errors/skipped 均为 0，包含全部 16 项 baseliner 测试。
旧恢复 30 组通过（716.70s），维护 3.77s、生命周期 16.19s、生产 56.04s、原成本 2.36s 通过。
生产保留 43 组原用例与 43 次插件重载恢复，并通过三种输入 × 四类结果/模型目标的 12 组、
三后端同库双表回滚、混合出口失败状态、typed 空结果及来源/目标/poll 拒绝断言。
原成本夹具本轮只运行一次：960 条观测、24 个模型身份、四路径各 5760 行及数值汇总一致；
尚未完成 T6 五轮重复和 DataFrame 排序/转换/独立模型导出成本，不把该夹具当作生产规模证明。
首次 NPM 必跑 MySQL 连接失败已核查为 WSL 表 127 将 127.0.0.1 转发至 loopback0；
容器健康且 IPv6 三端口成功/IPv4 三端口超时，诊断 network-diagnosis.json；首次 36 项运行后中断，
first-attempt-interrupted.log 和退出码 130 保留。仅临时将该地址路由到 lo 后重跑完整 CTest。
本轮 setpriv --bounding-set=-net_admin 条件明确保留，不证明特权 DPDK/NetVSC 清理已修复；真实数据库用例未跳过。
run_full.py 完成后已恢复原始路由，route_restore_rc=0、route_matches_original=true；前后条目和探测均有证据。
最终 hash 范围：979 个起始文件仅 tasks/active_task.md 变化，978 个保持，无越界；git diff --check 通过。
证据 /tmp/baseliner-t6-output/verification.json、full-tests.xml、full-ctest.log、full-ctest-detail.log、
test_baseliner_production.log、test_baseliner_recovery.log、test_baseliner_cost.log、scope.json、before/、diff/。
T6 尚待：本次实现的定向 Sanitizer、五轮及新增输入/模型出口成本、Feature 格式/严格编译、生产使用/验收文档收口。
按原子切片完成即停，不启动后续切片，不勾选 T6，不归档 Feature，不提交/推送。

## T5 历史最终验收与停止

状态：已完成。T5 规格已勾选；T6 未执行。定向 13 项最终结果全部通过，combined-tests.xml 为 11 项未受最后修复影响结果 + 最新算子/生产 2 项的带来源汇总。
旧恢复 30 组（732.05s）；生产原 43 组及 43 组插件重载恢复、新 12 组输出组合及三后端同库别名事务/混合失败完整通过。
最终输出测试 2/2（算子 0.81s，生产 70.78s），模型/结果未发布取消为 cancelled，实际登记后取消保留 committed 与行数。
三后端 store（6.13s）、旧维护（3.89s）、生命周期（16.17s）、Scheduler（37.77s/1.45s）、reader/契约/接口/evaluation 均通过。
构建零 warning/error，10 个 C++ 格式、4 份实现与 1 公共头严格编译通过；原始失败运行与修复依据保持。
证据 `/tmp/baseliner-t5-output/verification.json`、`combined-tests.xml`、`combined-test-provenance.json`、`before/`、`diff/`、`scope.json`。
允许文件内完成用户文档/规格与范围收口；不进入 T6，不归档整个 Feature，不提交/推送。

## 跨任务问题隔离

目标：现有通用 DataFrame SQL 查询/CSV 展示契约（独立后续任务，不改本 T5）。
生产对照执行普通 DataFrame SELECT 指定列+task_key WHERE 时结果为空，实际登记通道含 720 行；
证据 `/tmp/baseline-operator-t73/production-1791627836314714929/{t5_csv.csv,t73_query.csv}` 和 production-second.log。
T5 验收直接读取 owning channel 的 Arrow 快照并选择列，对照所有数值、NULL、枚举及 Schema，避免依赖这条非阻塞查询路径。
Catalog CSV 现有序列化也将 NULL 数值显示为 0 且使用六位小数，不能把 CSV 展示当作 Arrow 数据正确性的证据。
不扩大 Catalog、DataFrame 或通用 SQL 的公共契约；此处只记录可复核现象，不声明根因或实施修复。

## T5 前一切片检查点

状态：进行中且检查点通过。存储三真实后端通过（5.49s），算子最终 1/1（1.06s），Scheduler E2E/mutation guard（37.50s/1.66s）通过。
四实现严格编译及新公共头独立 C++17 严格编译通过；范围 975 起始文件中 965 保持，10 既有允许文件变化、1 新头，无越界。
首次生产测试数据库新出口三后端通过，DataFrame 数值对照断言失败；改为对单元格数值精度及 NULL/枚举验证、规范排序后第二次生产运行中。
原始失败日志/XML 保留；第二次生产尚未结束，不提前声明生产通过。摘要关系名已改为新模式的实际目标，需最终重建。
本切片沿用 T5，完成剩余出口失败/Schema/来源/旧恢复矩阵及文档收口；首切片不勾选规格。

## T4 历史工作台

# 即时工作台

WIP=0。T4 已完成并停止；T5/T6 保持待办，未提交/推送。

## 当前 Atomic Slice

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，T4。
- 当前切片：T4 最终验收与必要修复已完成，记录证据并停止。
- 业务意图：单表、CSV 以及两段多 dataset 的用户能独立查询最终模型参数；原模型配置和托管恢复保持。
- Non-Goals：不实施三段 INTO/T5、两新指定表联合事务、完整 Feature/T6 成本验收；不改算法或旧 ABI；不提交/推送。
- 允许文件：上一 T4 切片的接口/模型导出/契约/编解码及测试文件（下方历史清单）；另允许
  `src/framework/interfaces/iblock_transform_model_output.h`（新增）、
  `src/operators/baseliner/model_output.h`、`model_output.cpp`（新增）、`baseliner_operator.cpp`、`CMakeLists.txt`；
  `src/services/scheduler/scheduler_stream_executor.cpp`、`scheduler_routes.cpp`；
  `src/tests/test_baseliner_contract/test_baseliner_operator.cpp`、`test_baseliner_store.cpp`、`test_baseliner_production.cpp`；
  `tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`、`docs/baseliner.md`。
- 接口冻结：新增独立可选 `IBlockTransformModelOutputTaskV1`，ModelOutputTarget 返回拥有型目标；
  BindModelOutput 接收 version/size=1、primary_target 和恰一个共享数据库租约/私有 DataFrame 暂存通道；
  EOF 提交模型数据库或暂存 DataFrame，Scheduler 仅在 completed 时登记并 CompleteModelOutputPublication 确认。
  无 model_output 时不绑定、不导出。数据库采用独占原子会话、Schema 校验和按逻辑键事务写入；
  附加失败使任务失败，保留已发布旧 generation 的真实状态，不声称跨出口回滚。
- 验收命令：`cmake -B build src`；`cmake --build build --target test_baseliner_contract test_baseliner_evaluation test_baseliner_operator test_baseliner_store test_baseliner_production test_baseliner_recovery test_scheduler_e2e test_scheduler_mutation_guard -j4`；
  聚焦 CTest（含模型出口/恢复/生产/普通 Scheduler 回归）、clang-format-18、公共头严格编译、增量 Diff/hash 核验。
- 时间盒：30 分钟，2026-10-10T17:17:19+08:00～2026-10-10T17:47:19+08:00。
- 停止条件：输出及 T4 完整验收达到后勾选 T4 并 WIP=0；若到期保留明确检查点，仍沿 T4 收口，不启动 T5/T6。
- 起始快照/hash：`/tmp/baseliner-t4-model-output/before/`、`before-hashes.json`（保留所有既有用户改动）。

## T4 输出接线编译检查点

2026-10-10 17:45：T4 已完整验收，规格已勾选。最终 9 项分别取自 final-first-tests.xml 的 6 项、
final-output-tests.xml 的算子/存储 2 项和 production-final-tests.xml 的生产 1 项；
combined-tests.xml/provenance 明确记录来源，未抹去原始失败结果。30 组恢复矩阵 629.60s，
生产最后 33.20s：原 43 组及 43 组重载恢复、两段双 dataset inline/exact、三后端三类模型导出/重放、
DataFrame/typed 空/来源保护、模型失败后已提交 generation 保留及事务回滚通过。
MySQL 显式 schema 跨默认数据库遗漏先由新回归复现，再按服务/实际 schema 修复；失败清理 Cancel 不再覆盖 failed，
显式取消仍报告 cancelled。最后 checkpoint 断言改为只读 SQL，不申请失败任务未到期的写租约。
最后构建复核无 warning/error；21 个 C++ 格式、8 个实现/2 个独立公共头严格编译及增量 Diff/hash 核验通过，
既有编译诊断例外和临时 WSL 文件时间警告的重新构建结果均留证。源文档 973 个起始文件仅允许的 21 个变化，
952 个保持（含 Backlog/其他任务改动），新增 5 个允许文件；证据 `/tmp/baseliner-t4-model-output/verification.json`。
未实施 T5/T6，未执行完整 Feature/T6 验收，未提交/推送；完成即停。

2026-10-10：WITH 创建/归一、模型目标绑定、Schema 校验、事务幂等写、DataFrame 暂存/成功登记及摘要已接线。
`build-output.log` 中 operator/store/production/recovery/scheduler_e2e 首次构建完成，无 error；
后补的实际数据库身份覆盖检查需重新编译和验收。尚未宣称输出运行验收通过；T4 保持待办。
继续同一 T4 的验收切片，不进入 T5/T6。

## T4 输出接线切片边界（历史）

WIP=1。T4 模型层切片已通过，继续 T4 输出接线直至整体验收；不进入 T5/T6，不提交。

## 当前 Atomic Slice

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，T4。
- 当前切片：WITH 创建/归一/异步策略一致接线，模型表/DataFrame 资源绑定、最终输出及生产 SQL 验收。
- 业务意图：单表、CSV 以及两段多 dataset 的用户能独立查询最终模型参数；原模型配置和托管恢复保持。
- Non-Goals：不实施三段 INTO/T5、两新指定表联合事务、完整 Feature/T6 成本验收；不改算法或旧 ABI；不提交/推送。
- 允许文件：上一 T4 切片的接口/模型导出/契约/编解码及测试文件（下方历史清单）；另允许
  `src/framework/interfaces/iblock_transform_model_output.h`（新增）、
  `src/operators/baseliner/model_output.h`、`model_output.cpp`（新增）、`baseliner_operator.cpp`、`CMakeLists.txt`；
  `src/services/scheduler/scheduler_stream_executor.cpp`、`scheduler_routes.cpp`；
  `src/tests/test_baseliner_contract/test_baseliner_operator.cpp`、`test_baseliner_store.cpp`、`test_baseliner_production.cpp`；
  `tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`、`docs/baseliner.md`。
- 接口冻结：新增独立可选 `IBlockTransformModelOutputTaskV1`，ModelOutputTarget 返回拥有型目标；
  BindModelOutput 接收 version/size=1、primary_target 和恰一个共享数据库租约/私有 DataFrame 暂存通道；
  EOF 提交模型数据库或暂存 DataFrame，Scheduler 仅在 completed 时登记并 CompleteModelOutputPublication 确认。
  无 model_output 时不绑定、不导出。数据库采用独占原子会话、Schema 校验和按逻辑键事务写入；
  附加失败使任务失败，保留已发布旧 generation 的真实状态，不声称跨出口回滚。
- 验收命令：`cmake -B build src`；`cmake --build build --target test_baseliner_contract test_baseliner_evaluation test_baseliner_operator test_baseliner_store test_baseliner_production test_baseliner_recovery test_scheduler_e2e -j4`；
  聚焦 CTest（含模型出口/恢复/生产/普通 Scheduler 回归）、clang-format-18、公共头严格编译、增量 Diff/hash 核验。
- 时间盒：30 分钟，2026-10-10T16:49:20+08:00～2026-10-10T17:19:20+08:00。
- 停止条件：输出及 T4 完整验收达到后勾选 T4 并 WIP=0；若到期保留明确检查点，仍沿 T4 收口，不启动 T5/T6。
- 起始快照/hash：`/tmp/baseliner-t4-model-output/before/`、`before-hashes.json`（保留所有既有用户改动）。

## T4 模型层切片完成证据

2026-10-10：独立可选 IBaselineModelParametersV1 与最终 Arrow model_parameters Schema 已实现。
Value/Ratio 输出真实滚动/季节/月位置/bootstrap 分量，Relation 输出明确身份/版本的 basis 和 routed 参数。
WITH 目标不进入 TaskConfig/原文/hash；非法/重复/未知、poll 拒绝和精确 config 版本均有断言。
CMake 对应目标编译通过，CTest `slice1-tests.xml` 3/3（含 fixture）：实际系数数值、Relation basis、
导出前后完整 checkpoint 相等、最终时间、冷/空与预算失败通过。新增测试夹具任务按顺序关闭，生产默认不变。
证据 `/tmp/baseliner-t4-model-output/`；此前失败原始日志保留，不将失败运行称为全绿；T4 尚未勾选。

## T4 模型层切片边界（历史）

WIP=1。按用户“实施 T4 直至完成”推进当前 T4；不进入 T5/T6，不提交。

## 当前 Atomic Slice

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，T4 模型参数导出。
- 当前切片：独立只读模型参数能力、最终 Arrow Schema 与 WITH 选项契约及测试。
- 业务意图：向用户输出实际生效的 Value/Ratio/Relation 参数，不改变学习状态、配置/hash 或旧恢复语义。
- Non-Goals：不实施三段 INTO、两出口联合事务/T5、T6 成本验收；不改算法/原虚表/版本化 struct；不提交/推送。
- 允许文件：`tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`；
  `src/framework/interfaces/ibaseline_model_parameters.h`（新增）；
  `src/plugins/baseline/task/value_task.h`、`ratio_task.h`、`relation_task.h`、`baseline_task_base.h`；
  `src/plugins/baseline/checkpoint/checkpoint.h`、`model_parameters.cpp`（新增）、`src/plugins/baseline/CMakeLists.txt`；
  `src/operators/baseliner/baseliner_contract.h`、`baseliner_contract.cpp`、`evaluation.h`、`evaluation.cpp`、
  `result_codec.h`、`result_codec.cpp`；
  `src/tests/test_baseliner_contract/test_baseliner_evaluation.cpp`、`test_baseliner_contract.cpp`。
- 接口冻结：新增独立可选 `IBaselineModelParametersV1::ExportModelParameters(series_key,max_bytes,output) const`；
  调用与学习串行；版本化拥有型结果含状态/成熟度/参数 JSON，失败不改 output；参数为空表示未训练。
  `ConfigSnapshot` 的 model_output 是执行选项，仅 ResolveConfig 写入，不参与 TaskConfig 或 hash。
  模型行每 dataset/metric/series/epoch 一份，真实参数有独立版本，不输出 checkpoint/配置/诊断作为模型。
- 验收命令：`cmake -B build src`；`cmake --build build --target test_baseliner_contract test_baseliner_evaluation -j4`；
  `ctest --test-dir build -R '^test_baseliner_(contract|evaluation)$' --output-on-failure`；clang-format-18、严格编译及增量 Diff/hash 核验。
- 时间盒：30 分钟，2026-10-10T16:35:30+08:00～2026-10-10T17:05:30+08:00。
- 停止条件：当前检查点验证通过后记录证据，继续前先重冻同一 T4 的输出接线切片；T4 全部锚点通过才勾选。
- 起始快照/hash：`/tmp/baseliner-t4-model-output/before/`、`before-hashes.json`。

## 上轮工作台（历史）

WIP=0。任务顺序与两段输入兼容检视已完成并停止；T1～T3 已完成，后续按 T4 → T5 → T6 实施。

## 当前 Atomic Slice

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)；重排待实施 T4～T6 并补充兼容契约。
- 当前切片：将模型输出/结果输出/整体验收按实际依赖重编号，核查两段输入共用路径并补验收锚点。
- 业务意图：两段数据库来源用户保留多表分析、持续消费和联合恢复，新增出口仅在显式选择时启用。
- Non-Goals：不修改运行代码、公共接口、Parser、Backlog 或使用文档；不实施输出能力、不构建/运行 CTest、不提交/推送。
- 允许文件：`tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`。
- 契约边界：新编号 T4 模型输出、T5 结果输出、T6 整体验收；T1～T3 与历史证据保留原样，旧编号单独映射；
  snapshot 限制只约束新出口，不能约束两段输入本身；旧托管完整 Schema/发布与恢复路径保持。
- 验收命令：`python3 /tmp/baseliner-output-compat-spec-20261010/audit.py`（文档/编号/历史/范围核验）；
  `awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/archive/feat-baseliner-single-source.md`；
  `git diff --check -- tasks/active_task.md`；两份文档本轮增量 Diff 审查与代码路径只读证据核查。
- 时间盒：20 分钟，2026-10-10 16:16:36～16:36:36（Asia/Shanghai）。
- 停止条件：任务顺序与兼容约束通过文档验收后恢复 WIP=0，停止本切片，不自动开始新 T4。
- 起始快照/hash：`/tmp/baseliner-output-compat-spec-20261010/before/`、`before-hashes.json`，覆盖 973 个现有文件。

## 本轮完成证据

2026-10-10：待办已重排为 T4 模型输出、T5 结果输出、T6 整体验收；规格、验收锚点和当前工作台使用新编号，
历史记录保留当时编号并给出映射。新增两段输入兼容矩阵，明确多 dataset/poll、旧托管多 Schema/恢复、
WITH 解析/归一/异步策略、配置/恢复 hash、无额外导出开销及同数据库不同表的保护边界。
12 处现有代码/测试片段作为静态核查证据；只识别未来改动的共用路径，不把旧测试或静态检视当成新增能力验收。
文档核验通过：6 个任务及验收编号顺序、原 JSON/SQL 示例、10 个相对链接、历史记录与增量 Diff；
973 个起始文件仅允许的 2 份文档变化，其余 971 个（含 Backlog 和运行代码）保持。
规格完成证据前 202 个非空行，已复检仍属同一来源→学习→产物主链路，保留统一 Feature。
证据 `/tmp/baseliner-output-compat-spec-20261010/`：verification.json、code-review.json、before-hashes.json、before/、diff/。
未构建/运行 CTest、未实施新 T4～T6、未提交/推送；完成即停。

## 输出规格补充切片（历史，重编号前）

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)；补充拟新增 T5/T6 的契约与任务。
- 当前切片：将 WITH model_output / 单一 INTO 的输出分工补入规格，并同步 Feature 目标与验收依赖。
- 业务意图：单表测试及 CSV 离线分析用户能分别指定模型参数与评估/预测结果的位置，保持 SQL 通用语法。
- Non-Goals：不修改代码、公共头、SQL Parser、算法或运行文档；不实施新增任务、不运行构建/CTest、不提交/推送。
- 允许文件：`tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`、`tasks/product_backlog.md`。
- 契约边界：model_output 为与 config/parameters 并列的可选 WITH 参数；INTO 保持单一主结果出口；
  有限分析默认导出最终模型参数，保留旧托管恢复与已有结果 Schema，不承诺跨通道原子提交。
- 验收命令：`python3 /tmp/baseliner-output-spec-20261010/audit.py`（文档示例/链接/编号/范围核验）；
  `awk '/^## 完成证据/{exit} NF{n++} END{print n+0}' tasks/archive/feat-baseliner-single-source.md`；
  `git diff --check -- tasks/active_task.md tasks/product_backlog.md`；本次三份文档增量 Diff 检查。
- 时间盒：20 分钟，2026-10-10 15:50:56～16:10:56（Asia/Shanghai）。
- 停止条件：规格与任务补充通过文档验收后立即恢复 WIP=0；不自动开始 T5/T6 或 T4。
- 起始快照/hash：`/tmp/baseliner-output-spec-20261010/before/`、`before-hashes.json`，覆盖 973 个现有文件。

## 输出规格补充完成证据（历史，重编号前）

2026-10-10：已补充 WITH model_output / 单一 INTO 的输出分工、有限最终模型 Schema、只读导出、
预算/失败/生命周期和 checkpoint 边界；新增待实施 T5/T6，T4 保留编号并后置，Backlog 同步 Feature 目标。
文档核验通过：规格完成证据前 179 个非空行、6 个一级任务且无第三层编号；1 份 JSON、5 份 SQL
（含 3 份新输出示例）、52 个相对链接及增量 Diff 检查通过。973 个起始文件仅允许的 3 份文档变化，
其余 970 个保持；T1～T3 完成状态、历史证据及其他 Backlog 条目保持。
证据 `/tmp/baseliner-output-spec-20261010/`：verification.json、before-hashes.json、before/、diff/、audit.py。
本轮仅文档验收，未构建/运行 CTest、未实施输出能力、未提交/推送；完成即停。

## T3 切片边界（历史）

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，Feature Task T3。
- 当前切片：完成 DataFrame task adapter、Scheduler 生产 SQL 路由、托管指纹恢复和真实 CSV 验收。
- 业务意图：上传 CSV 直接复用原基线聚合/分析能力，来源快照、Schema、顺序和生命周期保持受控。
- Non-Goals：不执行 T4、不改算法/结果 Schema/存储协议、不提供 DataFrame poll/CSV 更新跟踪、不改 Catalog/Web；不提交/推送。
- 允许文件：`tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`、
  `src/framework/interfaces/iblock_transform_dataframe_input.h`、
  `src/operators/baseliner/baseliner_operator.h`、`src/operators/baseliner/baseliner_operator.cpp`、
  `src/operators/baseliner/snapshot_input.h`、`src/operators/baseliner/snapshot_input.cpp`、
  `src/operators/baseliner/dataframe_reader.cpp`、`src/framework/core/dataframe.cpp`、
  `src/services/scheduler/scheduler_plugin.h`、`src/services/scheduler/scheduler_routes.cpp`、
  `src/services/scheduler/scheduler_stream_executor.cpp`、
  `src/tests/test_baseliner_contract/test_baseliner_dataframe.cpp`、
  `src/tests/test_baseliner_contract/test_baseliner_production.cpp`、
  `src/tests/test_baseliner_contract/test_baseliner_operator.cpp`。
- 接口冻结：新增独立可选 DataFrame provider capability，不改旧虚表；task 输入绑定共享租约，
  Read 一次固定快照，复用原 SnapshotInput 聚合；指纹作为来源 epoch，在引擎恢复前校验并绑定初始进度。
- 完成契约：真实 EOF/尾桶释放后才通过零行 progress batch 发布完成位置；取消不能发布成功 EOF；
  同快照恢复跳过学习，内容或完整 Schema 变化拒绝恢复；普通 DataFrame 算子保持原路由。
- 空表契约：DataFrame 的显式 Schema 可生成零行 Arrow batch；空列按明确逻辑类型生成零长目标列；
  非空字符串数字/日期仍拒绝，不猜测。该修复不改公共接口，只触及零行物化路径。
- 验收命令：`cmake -B build src`；`cmake --build build --target test_baseliner_dataframe test_baseliner_operator test_baseliner_production test_scheduler_e2e test_framework test_builtin -j4`；
  `ctest --test-dir build -R '^test_(baseliner_(dataframe|contract|aggregation|operator|production)|scheduler_e2e|framework|builtin)$' --output-on-failure`；
  本轮 C++ clang-format-18、公共头独立 C++17 严格编译、git diff --check 和起始 hash/增量 Diff 核验。
- 时间盒：30 分钟，2026-10-10 14:57～15:27（Asia/Shanghai）。
- 停止条件：切片验收通过立即停止；T3 全部验收锚点达到才勾选；到期保留明确检查点或错误，不扩大范围。
- 起始快照/hash：`/tmp/baseliner-single-source-t3-integration/before/`、`before-hashes.json`。

## T2 切片边界（历史）

- 关联 Feature：[baseliner-single-source](archive/feat-baseliner-single-source.md)，Feature Task T2。
- 业务意图：让测试单表与 CSV 离线分析用户直接选择来源，并在 SQL WITH 内配置一个或少数基线指标。
- 已对齐：用户要求先补规格和任务，再实施；采用已说明的单表 parameters JSON，规格已落稿。
- 交付：冻结独立可选 provider 来源配置归一接口；测试先复现 reader 拒绝三段来源；
  接通仅声明能力的三段 SQL 路由、WITH 归一及 snapshot/poll 的两段租约/三段表绑定。
- Non-Goals：不实现 DataFrame/T3、其他算子能力、SQL 新语法、算法/结果格式/存储协议或完整 T4；不提交/推送。
- 允许文件：`tasks/active_task.md`、`tasks/archive/feat-baseliner-single-source.md`、
  `src/framework/interfaces/iblock_transform_source_config.h`（新增）、
  `src/operators/baseliner/snapshot_reader.h`、`src/operators/baseliner/snapshot_reader.cpp`、
  `src/operators/baseliner/poll_input.cpp`、`src/operators/baseliner/baseliner_operator.h`、
  `src/operators/baseliner/baseliner_operator.cpp`、`src/services/scheduler/scheduler_routes.cpp`、
  `src/services/scheduler/scheduler_stream_executor.cpp`、
  `src/tests/test_baseliner_contract/test_baseliner_snapshot.cpp`、
  `src/tests/test_baseliner_contract/test_baseliner_production.cpp`。
- 验收命令：`cmake --build build --target test_baseliner_snapshot test_baseliner_poll test_baseliner_production test_scheduler_e2e -j4`；
  `ctest --test-dir build -R '^test_(baseliner_(contract|snapshot|poll|production)|scheduler_e2e)$' --output-on-failure`；
  本轮 C++ clang-format-18；新公共头单独 C++17 严格编译；文档/Diff/hash 范围核验。
- 时间盒：30 分钟，2026-10-10 14:10～14:40（Asia/Shanghai）。
- 停止条件：本切片验收通过即更新状态并停止；若到期保留明确检查点，完整目标全部达到才勾选 T2。
- 风险边界：旧 V2/数据库输入/执行策略虚表与版本化 struct 不变，普通三段数据库查询走既有路径。
- 起始文件快照/hash：`/tmp/baseliner-single-source-t2/before/`、`before-hashes.json`。

## T3 完成证据（历史）

2026-10-10：T3 生产接线与恢复验收完成，规格 T3 已勾选，T4 保持待办。
DataFrame adapter、provider 显式能力与 Scheduler 接通；快照指纹作为 epoch 在恢复前校验，
中途 checkpoint 绑定来源证据，EOF/尾桶释放后才发布完成位置；普通 DataFrame 算子及数据库输入保持。
真实 CSV 的三类指标/forecast 与数据库输入逐个逻辑单元格一致，三个托管目标、完整/精确 config、
单表 WITH、范围/filter、重复/尾桶、仅表头 CSV、单 Schema DataFrame 240 行出口均通过。
生产 43 组及插件重载恢复 43 组通过；同内容重放 rows_written=0，结果/checkpoint 保持；
内容或完整 Schema 变化拒绝且模型/结果保持。任务层的契约/预算失败、取消后中途恢复
（模拟写入租约到期）、reader/adapter 的租约/列寿命及取消断言通过。
定向 8 项通过来自两个原始 CTest 结果：final-test.xml 中七项不变目标通过，production-final.xml 中
最后重建后的生产一项通过；combined-test.xml 是明确记录来源的汇总，不抹去先前失败证据。
构建复核无 warning/error，三份相关实现与公共头严格编译通过，12 个完整 C++ 文件和 DataFrame
两处修改范围的格式、Diff/hash 核验通过。源码/任务/文档共 934 个起始文件，仅允许的 15 个变化，919 个保持。
证据 /tmp/baseliner-single-source-t3-integration/：production-final.log/xml、production-detail.log、
combined-test.xml、combined-test-provenance.json、verification.json、final-build-check.log、production-build.log、
strict-compile.log、final-scope.json、before/、diff/。
时间盒 15:27 到期后冻结代码；最终验收命令随后返回通过结果，仅记录完成证据，没有继续实现或扩大测试范围。
先前失败均为新增验收夹具/辅助函数问题：SQLite :memory: 不能托管、builtin 未激活、跨后端原生查询
Schema 差异、空结果辅助函数假定非空及 SQL/通道读取接口选择；已核查并在允许测试文件中修正。
最后生产验收通过 1/1（31.22s，CTest 总计 31.44s），零失败/错误/跳过；不声明已执行 T4 全量验收。
未提交/推送；完成即停。

## T3 reader 检查点证据（历史）

2026-10-10：T3 的 DataFrame reader/可选输入契约切片完成，T3 整体不勾选。
新增独立 IID/size/version 输入接口，旧虚表不改；reader 固定 Arrow 快照并保留共享来源租约，
显式安全整数转换及 float32→float64，按原时间/稳定键排序、范围/filter 后选择最大 revision，
相同最大 revision 保留以交由既有 aggregator 拒绝冲突；page_rows 分页、真实 EOF。
版本化指纹覆盖完整原始 Schema/metadata、所有原列逻辑值及原行序，切片偏移不影响指纹；
源缓冲、排序索引与受限 Arrow 转换/页缓冲计入预算，页/列缓冲持有池所有权，取消不返回成功 EOF。
首个锚点先在 reader 未实现处失败；最终定向 CTest 4/4（1.06s，零失败/错误/跳过），
三类指标与原类型化原始页聚合逐项相等；乱序/分页/重复/尾桶、范围/filter/负时间/最大 revision、
来源/版本/类型/溢出/NULL 键/预算拒绝、内容/Schema/未使用列变更、来源替换/租约/列寿命、
typed 空快照/NULL 策略/取消均有断言。构建无 warning/error，四个 C++ 格式、reader 严格编译、
公共头独立 C++17 -Wall -Wextra -Werror 和 Diff 范围检查通过。
证据 `/tmp/baseliner-single-source-t3-reader/`：final-build.log、final-test.log/xml、verification.json、
final-ctest-detail.log、before/、diff/、final-scope.json；初始文档重建依据见 snapshot-provenance.json。
后续仍沿用 T3：task DataFrame adapter、Scheduler 能力路由、内容指纹绑定托管恢复及真实 CSV→生产 SQL 验收。
本轮 typed 空快照直接使用零行 Arrow batch；CSV 空文件/仅表头的注册链路尚未验证；
核查到 DataFrame::ToArrow 仅 pending_rows>0 时 Finalize，后续接线须验证空 Schema 是否保持。
停止在本切片，未运行 T4 全量验收，未提交或推送。

## T2 完成证据（历史）

2026-10-10：T2 完成，三段数据库 source、完整 config 与单表 WITH 简写已接通生产 SQL。
新可选 provider 接口保持旧虚表/struct；reader 验证三段唯一表并租用两段连接，poll 仍需真实发布进度。
构建无 warning/error；定向 CTest 6/6（目录 fixture 1 组，71.01s），JUnit 无失败/错误/跳过。
生产 38 组（原 26 + 新 12）及 38 task 插件重载恢复通过；新增四后端三段 snapshot/poll/单表 WITH 与
两段数值结果一致，跨表配置/缺进度拒绝，源 Schema/内容保持；普通查询及 builtin.passthrough 原 E2E 通过。
新公共头单独 C++17 -Wall -Wextra -Werror、十个 C++ 格式、文档/Diff/hash 范围检查通过。
现有 931 个文件中仅允许的 11 个文件变化（9 个 C++ 与工作台/规格），920 个保持；新增一个允许公共头。
证据 `/tmp/baseliner-single-source-t2/`：verification.json、final-build.log、final-regression.log/xml、
final-ctest-detail.log、final-scope.json，原文及本次 Diff 为 before/、diff/。
首次回归的关系 fallback 问题已在本切片范围内修复；WSL IPv4 超时通过临时 IPv6 验收配置处理，系统路由未改。
停止在 T2，不自动开始 T3/T4，不声明 DataFrame 或扩展 Feature 完整验收；未提交或推送。

## T1 完成证据

2026-10-10：新规格/Backlog 已补三段数据库、CSV DataFrame 与单表 WITH 场景，四个 Feature Task 已登记。
T1 完成；契约库支持唯一表绑定、DataFrame snapshot 限制、单数 dataset JSON 归一；旧完整 JSON/Config 兼容。
新增断言先复现来源格式限制，最终目标构建无 warning/error、契约 CTest 1/1（0.17s）无失败/错误/跳过；
三个 C++ 文件格式、文档 JSON/链接/编号及 git diff --check 通过。
776 个源码/归档文件核验只有允许的三个 C++ 文件变化，其余 773 个保持，前序工作区改动保留。
证据位于 `/tmp/baseliner-single-source-final-build.log`、`final-test.log/xml`、`final-scope.json`，
本次增量 Diff 与经起始 hash 验证的原文位于 `/tmp/baseliner-single-source-t1-review/`。
停止在 T1，不自动开始 T2～T4，未运行扩展 Feature 的完整构建/CTest 或生产输入验收，未提交或推送。

## 前序完成状态

关联规格：[Baseline 封装算子](archive/feat-baseline-operator.md)。
2026-10-10：完整构建、完整 CTest 77/77（716.78s，零失败/错误/跳过）、生产 26 组及 26 task 插件重载恢复、联合恢复 30 组、五轮成本重复、定向 ASan/UBSan/LSan、格式/严格编译/文档配置/Diff/hash 范围检查通过。
本续切片只修改允许的测试 CMake 目录 fixture、使用/验收记录和任务状态，前序 C++ 与无关改动保持冻结。
WSL 临时本机路由已恢复。最终 CTest 使用 setpriv --bounding-set=-net_admin；系统 DPDK 特权 NetVSC/failsafe 清理崩溃已作为跨任务问题记录在验收文档，未宣称修复。
成本及运行条件见 [验收记录](../docs/baseliner-acceptance.md)，使用见 [说明](../docs/baseliner.md)。
本机原始证据 /tmp/baseline-operator-t73/，汇总 verification.json，最终完整 full-ctest.log/xml。
已停止本任务，未提交或推送。
