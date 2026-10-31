# 当前工作台

WIP=0；关联已完成的 metadata-description-query、channel-description-refinement 与 channel-description-default-initialization，Atomic Slice：精确审查并创建本地提交。

- 业务意图：按用户“提交代码”指令，将已完成的通道说明体验及其尚未提交的对象说明基础作为一个可独立构建的本地提交交付。
- Non-Goals：不实施后续算子精简规格，不纳入其他规划和 Stream Task 改动；不修改功能源码，不扩大验收矩阵，不提交 evidence/构建产物/私有配置，不推送或部署。
- 允许修改：`tasks/active_task.md`，以及暂存检查发现的两处纯格式修复（Python 测试夹具行尾空格、默认初始化归档规格末尾空行）；Python 修复前后 AST 必须一致，其他源码/文档保持已验收内容。精确暂存清单、Backlog 三条完成项的部分暂存与排除清单冻结于 `evidence/channel-description-refinement/local-commit/plan.json`（48 个完整路径 + 1 个部分路径）。保存开始时 HEAD、空索引、全部既有改动哈希及原始 diff，提交后核对。
- 提交契约：本地 message 为 `feat: refine channel descriptions and object metadata workflows`；包括对象说明接口/保存与 UI、时间与关联、默认说明初始化、布局及来源自动核对、DataFrame 点击自动加载、对应测试/客户端/使用文档/归档规格。后续 operator-description-refinement 规格、Epic/Backlog 后续规划和 stream-recompute 状态保持工作树原样。
- 验收与依据：复核已有完整验收 metadata-description-query 59/59、channel-description-refinement 28/28、默认初始化 API/客户端 4/4、最新 DataFrame UI 2/2 的 Summary 和当前源文件/产物摘要；各项 failed=skipped=not_executed=0。没有功能源码新变更时复用已验证证据，不将其表述为本轮重新执行。
- 验收命令：浏览器脚本 `node --check`、Python 内存编译、既有 Summary/指纹核查、`git diff --cached --name-only`、`git diff --cached --check`；审查 staged diff，提交后验证 HEAD 路径、索引和原有工作树字节保留。完整日志限 `evidence/channel-description-refinement/local-commit/`。
- 执行位置/依赖：仓库根目录 native，Git、Python、Node；不依赖或启动外部服务，不重载常驻服务。
- 时间盒：30 分钟；停止条件：精确 staged 审查与静态检查通过，工作台恢复 WIP=0，完成一次本地 commit 后停止；任何未通过均不声明提交完成。

## 已完成实施证据

- 最新 DataFrame 自动加载切片验收：`evidence/channel-description-refinement/dataframe-auto-load/acceptance.json`；默认前端产物已重建，隔离浏览器 selected=executed=passed=2，零失败/跳过/未执行。
- 其他已完成能力与验收见三份归档规格；上轮工作台完整快照保留在 `evidence/channel-description-refinement/local-commit/active-before.md`。

## 本地提交审查完成

- 精确暂存为 49 个路径：48 个完整文件，Backlog 仅加入三个已完成 Feature 行。后续算子精简规格、Epic/Backlog 其他规划和 Stream Task 保留原工作区字节。
- 已核对 59/59、28/28、4/4、2/2 既有验收 Summary，失败/跳过/未执行均为零；当前功能源码与最新来源快照一致，后端产物与完整通道精简验收一致；首次加载 UI 使用最新 2/2 行为记录。此提交切片不重跑已通过的功能测试或扩大矩阵。
- 已清理两处暂存格式问题，Python 测试夹具前后 AST 相同；16 个相关脚本/JSON 静态检查、精确 staged 路径/内容审查、Git whitespace 检查通过。审查与指纹记录：`evidence/channel-description-refinement/local-commit/`（不入库）。
- 本地提交出口采用冻结的 message；结果由 Git 历史及同目录 `acceptance.json` 记录。完成本地提交后停止，不推送、不发布、不部署。
