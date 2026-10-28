# 当前工作台

WIP=0；关联：`channel-data-dictionary / 本地提交`；已验收 Feature 的提交审查完成，按下述出口执行本地提交及提交后核验。

## T5 实施与验收记录

- 业务意图：重启后绑定可恢复；来源观察只能由真实 describe/apply 建立，导入不冒充当前观察；并发冲突不覆盖、写入失败不暴露半个绑定；普通客户端可消费精确版本与标准上下文。
- Non-Goals：不做 F3～F5、不新增后端/数据面/部署矩阵，不改用户服务或旧卷，不提交推送。
- 冻结内部契约：不变更旧公共 IID/虚表。SemanticsSaveRequest 增内部 `source_observed=false`；仅 channels/apply 置 true；新增不可导出的 semantics_observation(exact_reference) 随绑定同事务提交；通用 save/import 无该能力。Registry 只读 ObservationTrusted(exact_reference)；v2 未经 apply 观察的绑定要求显式 observation 确认，check 仍重取来源。幂等 apply 可原子授予可信性而不强制内容增版。升级已有绑定保留非空 declared_schema 并继续原检查。
- 允许文件：`tasks/active_task.md`、`tasks/specs/feat-channel-data-dictionary.md`、归档目标 `tasks/archive/feat-channel-data-dictionary.md`、`tasks/product_backlog.md`；`src/channels/config/data_semantics_registry.h`、`src/channels/config/data_semantics_registry.cpp`、`src/channels/config/channel_dictionary.cpp`；`src/tests/test_config_channel/test_channel_dictionary_consistency.cpp`、`src/tests/test_config_channel/CMakeLists.txt`、`src/tests/test_config_channel/test_channel_dictionary_api.py`、`src/tests/test_config_channel/channel_dictionary_browser.mjs`；`docs/data-semantics.md`、`examples/semantics/client.py`；`src/frontend/src/components/ChannelDictionaryEditor.vue`（A6 收口同名替换的旧/新身份及复用入口）。
- 验收构建：`cmake -B build src`；`cmake --build build --target test_channel_dictionary_consistency test_channel_dictionary test_data_semantics test_config_channel test_channel_dictionary_database test_channel_dictionary_host test_data_semantics_host test_config_channel_e2e test_database_manager test_environment_probe test_catalog_lazy_loading test_kafka_structured test_kafka_channel_admin test_mysql test_postgres test_clickhouse test_sqlite test_router test_framework test_stream test_builtin test_scheduler_e2e -j4`。
- 完整验收：`tools/test.sh doctor --config evidence/channel-data-dictionary/environment.json --location native --filter '^(test_channel_dictionary_database|test_mysql|test_postgres|test_clickhouse)$'`；`tools/test.sh all --config evidence/channel-data-dictionary/environment.json --location native --filter '^(test_channel_dictionary_consistency|test_channel_dictionary_database|test_channel_dictionary_database_unit|test_channel_dictionary_browser|test_channel_dictionary_api|test_channel_dictionary_iid|test_channel_dictionary_sources|test_channel_dictionary|test_channel_dictionary_schema|test_data_semantics|test_data_semantics_api|test_data_semantics_browser|test_config_channel|test_config_channel_e2e|test_database_manager_unit|test_catalog_lazy_loading|test_kafka_structured|test_kafka_channel_admin|test_mysql|test_postgres|test_clickhouse|test_sqlite|test_router|test_framework|test_stream|test_builtin|test_scheduler_e2e)$' --jobs 1`；新增/改动行 clang-format，Node/Python 编译、git diff --check，默认 SO/static/契约产物 SHA256。
- 选择依据：A7/A8 新增可信性/原子回滚/双客户端/无写锁/普通精确客户端；Config 共享版本库变更纳入全部 F1/Config 回归；收口 A1～A6 与本 Feature 实际影响的 Catalog/Kafka/Database/Router/框架回归。按影响筛选，不执行全仓 all 或外部 Kafka 消费/认证、Docker 镜像/部署矩阵。
- 依赖与执行位置：仓库根 native；沿用唯一运行时配置 `evidence/channel-data-dictionary/environment.json`，项目 flowsql_dictionary_20261028；三外库真实认证+目录/专用夹具 DDL，SQLite/注册表临时库、CSV、离线 Kafka 配置、真实 Chromium、随机本地端口，标准 resources/fixture 锁。源码/产物在 runner 期间冻结；秘密不进证据摘要/Git。
- 时间盒：30 分钟，自 10:32 UTC 起；到期关闭并记录同 T5 检查点，按用户连续实施授权续轮。停止条件：A1～A8 与冻结清单全部通过、计数一致，说明/范围/产物审查通过，才勾选 T5、归档规格、Backlog 完成、WIP=0。

## 既有证据

- T1 `20261028T080414Z-91737661` 4/4；T2 `20261028T093500Z-d4d4179a` 18/18；T3 `20261028T101406Z-0dceb563` 20/20；T4 `20261028T102956Z-baf9f92a` 9/9；均零失败/跳过/未执行，规格已顺序勾选。
- 未重载用户服务，未 commit/push；既有服务/卷均保留。

- T5 首轮 runner `20261028T104211Z-347b7f09`：selected=executed=34，passed=32，failed=2，skipped=not_executed=0。既有/四库/HTTP/普通客户端通过；修正 channels/apply 的 stale expected 优先返回 REVISION_CONFLICT（不被新 observation 确认遮住），以及回滚夹具显式复用旧字典 field_id 映射。新增测试输出失败码以定位，不放宽验收。
- T5 第二轮 runner `20261028T105051Z-ce5adf7c`：34/34 执行、33 通过、1 失败，零跳过/未执行；新增一致性与浏览器已通过。既有映射负例要求 BINDING_MISMATCH 优先于 stale revision；最终处理顺序固定映射校验→版本冲突→变更确认，保留原断言并完整复验。
- A6 收口补充：按当前 locator 匹配历史绑定来源，展示同名替换旧/新 object_key 和旧精确字典的显式复用按钮；现有真实浏览器新增删除/重建后 unbound 与历史身份断言。保持既有身份服务与 API 不变。

- 30 分钟时间盒检查点：进行中且检查点通过。最终 runner `20261028T105820Z-7417d24e`：selected=executed=passed=34，failed=skipped=not_executed=0；sources/artifacts 前后完全一致。格式、Python/Node、diff 检查通过，交付摘要 `evidence/channel-data-dictionary/delivery.json`。本轮关闭；剩余同 T5 规格完成标记、归档与文档链接最终审查。

## 同 T5 收尾切片

- 业务意图：将已验收的交付状态写回规格/Backlog，保证完成证据和用户入口可追溯。
- Non-Goals：不再编辑源码/测试/契约，不启动 F3，不更改已冻结验收清单。
- 允许文件：仅 `tasks/active_task.md`、`tasks/specs/feat-channel-data-dictionary.md`、`tasks/archive/feat-channel-data-dictionary.md`、`tasks/product_backlog.md`。
- 验收：最终 runner 34/34 及稳定摘要已完成；文档相对文件链接、五个 Task 勾选、Backlog 完成、git diff --check、范围检查。无需重复执行已通过业务用例。
- 时间盒：10 分钟，自 11:02 UTC 起；停止条件：规格归档/状态/证据核对通过即 WIP=0。

- 已完成：T1～T5 已依序完整验收并全部勾选；规格归档至 `tasks/archive/feat-channel-data-dictionary.md`，Backlog `[x]`，WIP=0。最终 runner 34/34，源码/产物摘要稳定；未开始 F3，未重载用户服务、未提交/推送。

## 用户授权的本地提交切片

- 业务意图：用户明确要求“提交代码”，将已验收的通道字典能力形成可审查的本地提交。
- Non-Goals：不改源码、测试或公开契约，不吸收无关 Backlog 规划，不推送、打标签、发布、部署或重载用户服务。
- 修改边界：仅本工作台与 `tasks/product_backlog.md` 的 F2 状态说明；暂存范围为 `evidence/channel-data-dictionary/commit-review.json` 冻结的 71 个精确路径，该清单及证据不入库。Backlog 仅暂存 F2 完成条目，其余原有规划保留在工作区。
- 起点：HEAD `013975ff0faed4a7db042ee95318e8c52ec1fbfd`，index 为空；原始状态与 Backlog 已缓存于同目录 `precommit-status.txt`、`precommit-backlog.md`。
- 提交消息：`feat: generate and bind channel data dictionaries`。
- 验收依据：最终 runner `20261028T105820Z-7417d24e`，selected=executed=passed=34，failed=skipped=not_executed=0；提交前核对源码/默认产物摘要，不重复运行已通过且未变化的业务清单。外部 Kafka 数据面/认证、容器交付矩阵及产品发布全量仍未验证。
- 验收命令：显式路径 `git add -- <paths>`，Backlog 使用基于 HEAD 的仅 F2 索引内容；`git diff --cached --name-only` 与冻结清单完全一致，审查完整 `git diff --cached`，执行 `git diff --cached --check`；本地 `git commit` 后核对提交路径、index 为空、仅无关规划留工作区且内容完整。
- 依赖与权限：仓库根 native，仅本地 Git 元数据写入权限；不需要外部测试服务或网络。证据、构建产物与私有配置均排除。
- 时间盒：30 分钟；停止条件：精确路径/差异/摘要检查通过并完成本地提交及提交后核验，恢复 WIP=0。
- 提交前审查通过：完整暂存差异及精确 71 路径已核对，Backlog 索引仅含 F2 完成条目；62 个 runner 覆盖的源码/测试/依赖文件、12 个默认产物/公开契约摘要一致，runner 前后 sources/artifacts 一致，`git diff --cached --check` 通过。未改业务源码、未重复运行已通过的验收；本地提交及核验结果缓存于 `evidence/channel-data-dictionary/local-commit.json`，不入库。
