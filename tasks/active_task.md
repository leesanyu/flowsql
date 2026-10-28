# 即时工作台

WIP=0：data-semantics-registry 已完成。

- 用户授权范围：按 T1→T5 顺序完成，每个 Task 验收并勾选后进入下一个；本轮承接 T3，依次完成 T3、T4、T5。用户随后明确授权提交代码，按下述边界创建本地提交。
- 规格：[已归档 Feature](archive/feat-data-semantics-registry.md)，T1～T5 全部完成；Backlog F1 标记完成，Epic F2～F5 保持待办。
- 交付：不可变标准/字典/绑定版本库与公共 IID；八个管理接口及 OpenAPI/Schema；`/semantics` 页面；JSON/YAML 交换；普通 Python 客户端及用户说明。
- 构建：Config/Web 默认动态库、前端静态包、所有冻结测试目标、公开运行时契约文件完成构建；未重载用户服务。
- 最终 CTest：`test_config_channel`、`test_config_channel_e2e`、`test_data_semantics`、`test_data_semantics_api`、`test_data_semantics_browser`、`test_router` 和浏览器环境夹具，selected=executed=passed=7，failed=skipped=not_executed=0。
- runner：`20261028T070404Z-1b456de0`；命令 `tools/test.sh all --location native --filter '^(test_data_semantics_browser|test_data_semantics_api|test_data_semantics|test_config_channel|test_config_channel_e2e|test_router)$' --jobs 1`。有筛选 all 只执行上述清单。
- Node 新增/旧 Config 前端回归 16/16；三份完整样例、八个 OpenAPI 请求示例、运行目录契约字节一致、文档链接、Python/JS 语法、C++ 新文件/改动行格式、版权和 diff 检查通过。
- 验证边界：原生临时 SQLite、随机 loopback、实际 Web→Gateway→Router→Provider/IID 和 Chromium；双客户端并发、事务中途回滚、跨进程恢复、旧配置逐字保留。未执行外部数据库/Kafka、镜像或全仓矩阵。
- 证据：`evidence/data-semantics-registry/` 下的构建/交付/Node日志、浏览器截图、最终 runner 摘要与源码/产物指纹，未入库。
- 实施停止条件已达成：没有启动下一个 Feature；仅按后续授权完成本地提交。

## 本地提交切片

- 业务意图：将已验收的标准/字典/绑定能力及交付文档固化为可追溯的本地提交。
- Non-Goals：不改业务代码，不启动后续 Feature，不重跑无变化的完整运行验收，不推送、打标签、发布或部署。
- 本切片允许修改：仅 `tasks/active_task.md` 的提交审计记录；其余已验收文件仅审查/暂存，精确 44 路径清单冻结于本地 `evidence/data-semantics-registry/commit-review.json`。
- 暂存边界：包含 Feature 实现、测试、契约、样例、使用说明、归档规格及其引用的 Epic 边界说明；Backlog 只提交 `data-semantics-registry` 完成条目，原有其他规划逐字保留在工作区。
- 验收依据：已核对 44 个源码/文档及 24 个产物摘要与最终验收一致；本切片仅更新工作台记录。最终 runner 7/7、Node 16/16 的范围及未覆盖项沿用上述声明。
- 验收命令：`python3 evidence/data-semantics-registry/verify_delivery.py`、上述 Node 回归、`git diff --cached --name-only`、`git diff --cached --check`；比对暂存内容和冻结清单，提交后核对 commit 路径、剩余改动及索引为空。
- 执行位置/依赖：仓库根目录，本地 Git、Python/jsonschema、Node、clang-format；不启动服务或外部 fixture，Git 元数据写入按环境权限执行。
- 提交信息：`feat: add versioned data semantics registry`。
- 时间盒：20 分钟；停止条件：精确暂存审查通过、本地提交成功，其他规划及证据/产物保留在工作区。
