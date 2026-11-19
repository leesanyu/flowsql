# 当前工作台

- Feature：`data-semantics-read-optimization`；T1～T5 全部验收并归档；WIP=0。
- Atomic Slice：T5 第一切片，最终共享契约回归、同配置性能复测、当前用户说明及归档。
- 状态：已完成；2026-11-20 01:53 Asia/Shanghai。
- 时间盒：2026-11-20 01:35–02:05 Asia/Shanghai，30分钟；到期记录明确检查点，必要时沿用 T5 续接。
- 业务意图：让静态读取、任务准备和完整页面具备可重复的最终证据，文档与当前使用方式一致。
- Non-Goals：不新增产品能力、不启动下一 Feature、不扩展真实数据库/Kafka认证或容器矩阵、不执行无筛选全仓测试、不提交/推送。

## 契约与允许修改文件

- 产品接口及源码冻结为 T1～T4 结果；只将现有前端语义工具断言登记为标准 runner `test_metadata_reading_frontend`，无外部服务，16条 Node 断言。
- 允许：`src/tests/test_config_channel/CMakeLists.txt`、`config/testing/dependencies.json`、`docs/data-semantics.md`、`docs/testing.md`、`tasks/active_task.md`、`tasks/product_backlog.md`、`tasks/specs/epic-data-semantics-dictionary.md`、`tasks/specs/feat-data-semantics-read-optimization.md` 及其最终归档文件 `tasks/archive/feat-data-semantics-read-optimization.md`、本地 `evidence/data-semantics-read-optimization/`。
- 所有先前 Feature diff 和用户初始 diff 保留；每次 patch 后核对 `git diff --name-only`，只审查本次 Feature Diff。

## 构建及冻结验收

- 执行位置 native；配置唯一来源 `config/testing/environment.json`；依赖声明 `config/testing/dependencies.json`。临时 SQLite/PCAP、本地 Worker/插件、Node/Vue、Chromium、回环 socket；性能额外需要 GDB/ptrace 和已冻结的兼容基线产物。无外部账号/数据库/Kafka/模型/容器。
- `cmake -B build src`
- `cmake --build build --target test_data_semantics test_data_semantics_host test_channel_dictionary test_channel_dictionary_consistency test_operator_contract_model test_operator_contract_check test_operator_contract_output test_operator_contract_host test_operator_contract_outputs test_operator_contract_block test_operator_contract_execution test_baseliner_contract_invocation test_metadata_description test_npm_data_contract test_npm_result_semantics benchmark_npm_throughput -j4`
- `tools/test.sh all --location native --filter '^(test_data_semantics|test_data_semantics_api|test_channel_dictionary|test_channel_dictionary_schema|test_channel_dictionary_api|test_channel_dictionary_consistency|test_operator_contract_model|test_operator_contract_schema|test_operator_contract_check|test_operator_contract_output|test_operator_contract_outputs|test_operator_contract_block|test_operator_contract_execution|test_operator_contract_api|test_operator_contract_preflight|test_baseliner_contract_invocation|test_metadata_description|test_metadata_description_api|test_metadata_description_client|test_metadata_reading_frontend|test_npm_data_contract|test_npm_result_semantics)$' --jobs 1`
- 选择依据：快照公共结构改变需重编实际消费者；注册表/字典/API/Schema/派生/预检/批流执行及通用 block helper、普通客户端闭环均受共享契约影响。前端登记测试覆盖无损JSON、作者/本地分离和时间解释。T4 三类真实浏览器产品源码冻结且已通过，复用其最终范围证据，不重复整套浏览器。
- 功能测试完成后独占执行：`FLOWSQL_SEMANTICS_BASELINE_ROOT="$PWD/evidence/data-semantics-read-optimization/baseline/output" cmake --build build --target benchmark_semantics_reads -j4`；完整 all phases，保持所有既定阈值。计时期间不构建或运行其他测试。
- 最终性能覆盖五类 HTTP 5次预热+100次、8并发104次、三类IID计数/p95；37对象十二实体首次+5次重复任务分段、人工补充/引用/业务内容；混合IPv4/6离线与受控实时各5轮交替、首包/周期p95及独立零读取/解析审计。基线173产物摘要重新核验；最终源码/产物摘要缓存未入库。
- `git diff --check`；复用已全绿20个改动C++文件 clang-format 检查（T3后无C++修改），核对最终Diff/文档链接/实际runner summary。

## 完成与停止

- T5 所有必需范围绿色、性能阈值通过、文档同步后才勾选 T5、归档规格文件并同步 Backlog/Epic。只移动文件，不移动或删除目录。
- 完成整个 Feature 后停止，不自动开始其他 Feature；未执行范围不声明验证。
- T1～T4 证据详见规格。T4 最终通过范围为通道 `20261119T172858Z-0c945db9`、算子 `20261119T171709Z-d7c5e6db`、NPM `20261119T173200Z-440d6149`；此前失败项均已明确保留诊断记录，不计为通过。

## 当前检查点

- 配置、16个冻结构建目标通过；最终标准 runner `20261119T174107Z-d473c74e` 39/39（22功能+17夹具），失败/跳过/未执行均零，源码/产物在验收期间一致；日志 `t5-regression.log`。前端工具16条断言包含于注册用例。
- 冻结基线173个产物摘要重新核验，零不匹配，`baseline/verification-final.json`。
- 完整 `benchmark_semantics_reads` 所有阶段已通过：usage/resource p95 2.690/1.276ms，IID最大0.007820ms；重复语义准备下降73.04%，离线/受控实时吞吐为基线100.37%/100.70%。首包/周期p95分报，静态读取/解析审计为零。
- 用户说明已同步最终测量日期、环境、负载、冷启动、计时边界及限制；T1～T5全部勾选，规格文件归档，Backlog/Epic同步完成。
- 20个改动C++文件格式检查、最终Diff及证据核对通过；汇总 `acceptance-final.json`，最终源码/产物摘要 `final-manifest.json`。未执行全仓发布、外部数据库/Kafka认证部署矩阵或容器交付。
- 已按用户授权完成整个Feature并停止；不启动后续Feature，不提交/推送；本次仅移动规格文件，未移动或删除目录。
