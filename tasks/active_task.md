# 工作台：agent-data-access 已完成

- 授权：用户要求连续完成整个 `agent-data-access` Feature，自动执行；不提交/推送，不扩展其它 Feature。
- Feature：`tasks/archive/feat-agent-data-access.md`；最终 Atomic Slice：T4 部署说明、可运行样例及整体验收，状态：已完成。
- 前检查点：真实分析正式清单 `20261127T073603Z-d54f117d` passed、2/2、零跳过、源码/产物稳定；含 PCAP/PCAPNG DNS 原始值/统计/直方图、Baseliner 数据库与 DataFrame、配置与隐式输入授权、响应丢失/重试、底层取消/超时、崩溃重启不重投、结果固定摘要和独立安装 MCP 重启。前面失败轮次不作完成证据。
- 业务意图：使用者能从独立包和统一框架部署相同受控能力，获得可复核的完整验收清单；仅暴露专用认证监听器。
- Non-Goals：不实现诊断 Agent/F2/F3、不新增 Kafka 数据面/网卡采集/镜像交付、不跑全仓测试、不处理其它 Feature 的既有 diff。
- 允许文件：本工作台、`tasks/specs/feat-agent-data-access.md`、最终归档文件 `tasks/archive/feat-agent-data-access.md`、`tasks/product_backlog.md`、`README.md`、`docs/agent-data-access.md`、`config/agent-data-access/{policy.example.json,mcp.example.json,deploy.example.yaml}`、`src/app/CMakeLists.txt`、`src/mcp/tests/{analysis_probe.py,test_mcp_contract.py}`；仅修复实际验收暴露的夹具阻塞。若发现产品失败，先更新切片及允许文件再修改。
- 公共契约：沿用已通过的 12 工具/JSON Schema；分析清单 derived_sources 带实际 provider 的 SourceRef；结果依据使用精确 metadata_basis 避免分页重复整个说明；无旧接口兼容层。
- 文档交付：独立 Python 安装和仓库外启动、双受众凭证、HTTP/MCP 相同入口、查询子集/快照/无损类型/租约/摘要、固定分析及派生范围、超时与状态待核对、保护路由及远程 TLS 配置，明确已验证范围。
- 验收构建：`cmake -B build src`；`cmake --build build --target flowsql flowsql_data_access test_access_contract test_access_store test_access_backends test_access_sources test_access_dataframe test_access_analysis_host test_visualization_dataframe_workflow test_visualization_workflow -j8`。受影响可视化插件上一检查点已编译，源码此后未改。
- 完整清单：`FLOWSQL_TEST_STATE=/tmp/flowsql-access-test-state FLOWSQL_TEST_MCP_PYTHON=/tmp/flowsql-mcp-release/bin/python tools/test.sh all --config /tmp/flowsql-access-test-config/environment.json --location native --filter '^test_(access_(contract|store|mysql|postgres|clickhouse|sources|dataframe|analysis)|mcp_contract|visualization_dataframe_workflow|visualization_workflow)$' --jobs 1`。仅这些实际变更模块及共享计算回归，不运行无筛选 all。
- 依赖与权限：四库使用统一运行时私有配置（仅隔离项目名与私有状态目录不同，凭证不入日志），标准入口 native 真实认证/库/权限预检；夹具只改自建表/视图。其余仅本地 SQLite、Arrow、已注册抓包、真实插件及 HTTP。Kafka/网卡仅目录无副作用测试，不准备 broker/设备。独立 MCP 虚拟环境含其声明依赖和测试客户端，生产包仓库外运行。
- 额外交付验证：打包 wheel 并核对安装源；实际 `flowsql --config` 启动专用样例，无 Gateway/Router/SQL 路由，查询固定业务样例。C++ clang-format、Python 编译/静态检查、Schema/样例解析、Git diff 检查；本地证据在 `evidence/agent-data-access/`。
- 停止条件：新增/既有冻结用例全部绿色且零跳过、源码产物摘要稳定、样例实际启动成功后，核对 A1～A9 证据并勾选 T1～T4，将规格文件归档、backlog 完成；不提前宣称完成。时间盒到期记录检查点并沿同一 T4 连续执行。
- 静态检查补充：仓库实际 Ruff 规则发现本 Feature 新 Python 文件有未用导入、注解/导入格式、测试子进程与字典样式报警。允许文件增加 `src/mcp/flowsql_mcp/*.py`、`src/tests/test_data_access/{test_access_analysis.py,run_mcp_tests.py}`，只作该清单静态修正，契约不变；随后重建 wheel 并重新安装，完整回归仍覆盖全部 MCP/分析路径。

- 交付检查点：专用样例在仓库外实际框架启动成功，策略过滤后 count=1/sum=20.5，认证 401、Scheduler 路由 404；证据 `evidence/agent-data-access/deployment-smoke.json`。新虚拟环境 `/tmp/flowsql-mcp-release` 仅安装 wheel 声明依赖后再加入测试 pytest，全部包源码摘要一致；最终回归切换此环境。Ruff 与 C++ 格式检查通过。
- 归档链接检查：现有 Epic/F2/F3/F4/F5 文档均直接链接 F1 规格。允许文件补充 `tasks/specs/{epic-llm-skill-diagnosis,feat-diagnosis-artifacts,feat-diagnosis-skills,feat-diagnosis-agent,feat-diagnosis-workspace}.md`，仅在最终归档时同步 F1 链接和 Epic 的 F1 状态，不改其他任务内容或推进状态。
- 整体回归 `20261127T074926Z-eacc7611`：20/20 已执行、19 通过、1 失败、零跳过，源码与产物稳定。真实分析与新安装 MCP、四库、Store、快照、共享计算均通过。唯一失败 `test_access_sources` 使用早期空 workflow，冻结后的 Schema 要求至少一个 view，实际在入参层返回 400；应以合法 workflow 验证流/Config 的 422 不支持边界。允许新增修改 `src/tests/test_data_access/test_access_sources.cpp`，只修该夹具。重建同目标后以原来源/SQLite清单复跑；未变更模块已有本轮通过不重复全部执行。

## 最终收口（2026-11-27）

- T1～T4 和 A1～A9 已验收，Feature 规格文件已归档，backlog/Epic 的 F1 标为完成；F2～F5 只同步 F1 链接，未实施。
- 冻结清单 11 用例 + 9 环境夹具，共 20 项最终全部通过、零跳过/未执行。整轮 `20261127T074926Z-eacc7611` 19/20；仅旧目录夹具空 workflow 与新 Schema 冲突，合法 workflow 修复后 `20261127T075530Z-3483e998` 4/4。
- 已比较两轮 sources/artifacts 摘要：只变更 `src/tests/test_data_access/test_access_sources.cpp` 与 `test_access_sources`，其他产品与测试产物不变；各轮执行期间摘要稳定。逐项最终证据：`evidence/agent-data-access/acceptance.json`（未入库）。
- 实际样例在仓库外框架进程启动成功，认证 401、Scheduler 路由 404、策略过滤 count=1/sum=20.5；新虚拟环境 wheel 安装、源码摘要一致；官方 MCP 客户端与独立重启通过。
- 构建目标均通过，无编译错误/警告；C++ 格式、Ruff 检查/格式、Python 编译、Schema/样例解析、相对链接及 diff 检查通过。
- 本次 diff 审查完成，无旧兼容分支、客户端可扩权参数或原生 SQL 入口。无 git commit/push，未动已有用户数据库容器/卷或其他 Feature 实现。
- 不宣称已验证 Kafka 数据面、网卡真实采集、镜像或未选仓库模块。用户授权的本 Feature 全部收口，停止，不进入 F2～F5。
