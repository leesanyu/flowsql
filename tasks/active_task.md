# 当前工作台

WIP=0；operator-description-refinement 已完成；T1～T5、R1～R9 验收和本地提交审查通过。

- 用户授权：指出作者用途等描述应填在编辑框内，要求统一纠正框外说明与空白编辑框分离的问题；沿用此前简洁通用布局和实施授权。
- 业务意图：打开算子时可直接阅读、修改已有用途、输入/参数/输出说明、使用限制及示例；保存/重开后保持编辑内容，不再重复显示框外作者说明。
- Non-Goals：不改执行算法、作者机器契约、三张平铺字段表、组/调用检查/历史入口；不从自然语言生成字段规则、不将NPM数百字段自动铺进本地表；不修改常驻实例的算子安装或重启服务，不提交/推送、不处理既有无关修改。
- 冻结数据：保留notes六项字段；新增可选input_description/parameter_description/output_description:string，最大30000，空字符串为明确清空。usage.annotations原样投影这三项，缺少键时使用作者对应说明。用途/限制/示例优先已有本地文本；旧六项记录中的空框以作者原文初始化，本版保存三个整体说明后尊重文本显式空值。purpose及单项example上限统一为30000以容纳作者原文。
- 页面：作者用途直接预填既有用途框；输入/参数/输出各一个说明框替换原静态短说明，三张字段表不变；限制与示例预填已有折叠编辑框，无重复作者段落。历史作者声明保持只读。草稿使用新key=flowsql.operator-description.inline-editors.<name>，同版草稿/保存期间输入和版本冲突保护保留，不读取旧版空白草稿。
- 主链路：usage→作者初值/已保存覆盖→直接编辑→CAS保存→usage/full精确往返→重开/宿主Worker重启恢复；显式清空不得被作者初值覆盖，组件变化保留草稿，技术详情仍为原始已保存记录。
- 允许文件：
  - tasks/active_task.md
  - tasks/archive/feat-operator-description-refinement.md
  - docs/data-semantics.md
  - config/semantics/api.schema.json
  - config/semantics/openapi.json
  - config/semantics/examples/operator-usage.json
  - src/channels/config/operator_usage.h
  - src/frontend/src/components/OperatorContractPanel.vue
  - src/frontend/src/utils/operatorDescriptionNotes.js
  - src/frontend/src/utils/operatorDescriptionNotes.test.js
  - src/tests/test_config_channel/test_operator_contract_schema.py
  - src/tests/test_config_channel/test_metadata_description_api.py
  - src/tests/test_config_channel/metadata_operator_browser.mjs
  - src/tests/test_config_channel/test_metadata_operator_browser.py
  - src/tests/test_config_channel/operator_contract_browser.mjs
- 步骤：Schema/投影与有效初值测试→通用编辑框与持久化断言→主构建→按影响验收→截图/范围/diff/指纹→归档R9并WIP=0停止。
- 构建：cmake -B build src；cmake --build build --target test_metadata_description test_operator_contract_host flowsql_runtime_config -j4；构建完成后串行重建test_npm_data_contract和flowsql_frontend（同一usage头文件审计回归及最终前端初值修正）；主build操作串行。
- 前端：cwd=src/frontend，node --test src/api/*.test.js src/utils/*.test.js test/*.test.mjs。
- 标准验收：tools/test.sh all --location native --filter '^(test_metadata_description|test_metadata_description_api|test_operator_contract_schema|test_metadata_operator_browser|test_operator_contract_browser|test_metadata_description_client|test_npm_data_contract)$' --jobs 1。
- 选择依据：修改本地说明Schema、usage投影及通用页面，覆盖共享Schema/IID/HTTP/普通客户端及两算子真实浏览器；NPM契约审计回归确认作者文档不变。不改执行契约，不重复算法、外部后端或全仓测试。
- 失败证据与范围核对：首轮20261101T130945Z-937fbb86完整执行14项，13通过、1失败、零跳过，源码/产物指纹一致。独立operator_contract_browser驱动仍断言四个框外输出节点，实际已移入输出说明框；仅将该驱动加入允许清单，改为与真实full契约逐项精确比对四份输出说明并确认框外无重复。保留首轮证据，按原14项清单复跑，不增加超时或删减必需断言。
- 依赖：native仓库根，config/testing/{dependencies,environment}.json；services=[]，既有隔离HTTP/SQLite/Worker/NPI及Chromium/Node，资源锁沿用；无需外部数据库/Kafka/镜像。
- 静态验证：修改C++遵循src/.clang-format；脚本语法、JSON/文档链接、git diff --check；每次patch后git diff --name-only及本轮check_scope.py，保留所有既有修改。
- 证据：evidence/operator-description-refinement/inline-editors/，已保存全工作树baseline与允许文件before快照；标准验收前后源码/主产物指纹一致。
- 时间盒：2026-11-01 13:03～13:33 UTC；到期只记录可验证检查点，不扩范围。
- 停止条件：两算子作者原文直接进入编辑框，保存/清空/重开/历史/冲突与重启断言通过；冻结清单selected=executed=passed、failed=skipped=not_executed=0，截图/diff/范围/指纹审查通过，WIP=0停止。

## 完成证据

- CMake配置与test_metadata_description/test_operator_contract_host/flowsql_runtime_config主构建通过，串行重建test_npm_data_contract与最终flowsql_frontend通过；54项前端测试全部执行通过，失败/跳过/取消为0。
- 最终标准运行20261101T131604Z-bde14316：7功能/回归与7环境入口，selected=executed=passed=14，failed=skipped=not_executed=0，CTest272.64秒。首轮驱动失败证据保存在inline-editors/first-run/，未减少必需清单、放宽行为断言或增加超时。
- 六处说明逐项与两份真实作者契约比对，作者文字仅在对应编辑框显示；修改/清空/保存/重开和宿主/Worker重启精确恢复，保存不改变作者机器契约。18布局、6主题检查、8次真实菜单展开、草稿/期间编辑/冲突/历史/组件隔离全部通过；两份compact与代表性深浅截图已审查。
- 1139源码和79相关主产物验收前后指纹一致，标准runner源码/产物快照也一致；修改C++格式、JS/Python语法、24处文档文件链接、7个JSON块、Diff及本轮15文件允许范围审查通过，既有其他修改保留。
- 独立验收记录evidence/operator-description-refinement/inline-editors/acceptance.json，标准summary位于evidence/test-environment/20261101T131604Z-bde14316/summary.json，截图/日志/指纹均为本地缓存、不入库；同Feature归档已追加R9。
- 8081实际index及入口资源与新主构建一致，npm.basic和explore.baseliner均为contract_state=declared。常驻PID1413915仍映射已删除的旧libflowsql_config_channel.so，刷新可看到新编辑框，保存新增三个整体说明需重启后端加载新Config库；无需再次上传两份算子。本轮未重启用户服务、未清理数据、未提交/推送，切片完成并停止。

## 用户授权的本地提交

- 用户授权：先提交代码；仅创建本地Git提交，不推送、打标签、发布或重启服务。此前“不提交”属于实现阶段约定，本次明确授权替代该限制。
- 业务意图：将已验收的完整算子契约说明重构和R1～R9修正固化为一个可审查提交，保留其他工作区改动。
- Non-Goals：不修改实现或扩大测试范围，不处理AGENTS.md、CLAUDE.md、语义Epic或stream-recompute规划，不纳入evidence、构建产物或私有配置。
- 允许编辑文件：tasks/active_task.md、tasks/archive/feat-operator-description-refinement.md、tasks/product_backlog.md；只对齐本Feature完成状态和提交记录。
- 允许暂存范围：evidence/operator-description-refinement/local-commit/baseline.json中的42个full路径，以及product_backlog.md中的operator-description-refinement一行；逐路径暂存，Backlog其他部分保持未暂存。
- 冻结提交标题：feat: refine operator contracts and simplify description editing。
- 验收依据：T5标准清单39/39、R8清单28/28、最终R9清单14/14和前端54/54，失败/跳过/未执行均为0。提交前核对最终1139源码和79主产物摘要，未改实现时不重复构建或执行已通过清单；全仓、外部数据库/Kafka、镜像和常驻服务加载不声明已验证。
- 验收命令：源码/产物SHA256逐项比对、git diff --cached --name-only、git diff --cached、git diff --cached --check；提交后核对43路径、空索引及四个排除文件与Backlog非目标部分字节不变。
- 时间盒：2026-11-01 13:24～13:44 UTC；停止条件：审查/摘要核对通过，本地提交成功，非目标改动保留，工作台WIP=0后停止。
- 证据：evidence/operator-description-refinement/local-commit/，已保存初始HEAD、索引、全脏路径摘要和文件快照；本地缓存不入库。
- 提交审查结果：43个暂存路径与冻结清单完全一致，Backlog只包含本Feature完成行；8个JSON解析、8个C++文件格式、版权头、暂存Diff及空白检查通过。最终验收的1139源码与79主产物逐项摘要一致，实现没有修改，沿用已执行的39/39、28/28、14/14标准清单和前端54/54，不重复构建/测试。
- 本地提交按本次用户授权执行；提交ID、实际文件清单、空索引和无关文件/Backlog内容保留核对写入同目录本地commit-result.json。未授权远端或部署操作，完成后停止。
