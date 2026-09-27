# 即时工作台

事项：刷新 README 中已过时的快速开始与 NPM 说明。
关联 Feature Task：无；已交付能力的使用文档同步。
当前 Atomic Slice：只更新 README 的启动、前端、测试、NPM 结果与项目结构说明。
状态：已完成；WIP=0。

## 业务意图

让首次使用者按当前仓库的入口启动和测试，并能据当前 `npm.basic` 实现完成离线多实体存储及查询。

## Non-Goals

- 不修改代码、配置、前端页面、功能规格或归档任务。
- 不宣称生产实时 NPM、DNS/HTTP/TLS/ICMP 模块已经交付；不设计 SQL 编辑器补全与浮动帮助。
- 不自动提交或推送；提交由用户后续单独指令决定。

## 冻结事实与验收锚点

- 单进程启动与前端重建以 `start.sh` 当前选项为准；测试入口以 CMake/CTest 为准。
- NPM 默认 Basic 输出、可选 Session、托管数据库单目标、结构化 run 摘要、三种版本关系及显式保留参数，与代码和 E2E 保持一致。
- 已完成 `stream-time-drive`，生产 NPM 实时接线仍待 `npm-capture-contract` 与 `npm-basic-realtime-integration`。
- 校对 README 链接、示例 SQL 与路径；仅检查文档，不扩大构建测试范围。

## 允许修改文件

- `tasks/active_task.md`
- `README.md`

每次 patch 后执行 `git diff --name-only`，确认只有以上文件。

## 验收命令

```bash
git diff --check
git diff -- README.md tasks/active_task.md
git diff --name-only
```

并以只读检查确认 README 的路径和链接存在、关键用法与 `start.sh`、NPM 配置解析和 Scheduler E2E 一致。

## 时间盒与停止条件

- 本轮约 10～30 分钟；完成上述文档改动和校验后记录证据、置 WIP=0，并停止。
- 发现独立实现问题只记录事实，不混入本轮代码修改。

## 完成证据

- 已刷新快速开始、前端重建与静态资源同步、完整及 NPM 定向测试入口。
- 已核对 PostgreSQL Compose 密码，并修正 NPM 算子需上传激活的部署说明。
- 已补充 Basic/Session 模块与参数默认值、观测域语法、Schema v1/v2、四后端多实体存储及按 run_id 查询示例。
- 已说明保留期、失败运行查询状态与当前生产实时能力边界，并更新项目结构和文档索引。
- README 本地链接、目录、代码围栏、JSON 参数示例与关键配置检查通过；Schema 列数和托管响应字段已对照当前代码核查。
- `git diff --check` 通过；改动范围仅为 `README.md` 和本工作台。
- 本轮为文档更新，未重新执行编译或 CTest；提交与推送另按用户指令执行。
