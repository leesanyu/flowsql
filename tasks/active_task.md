# 即时工作台

事项：在 README 补充 Docker 镜像文件交付方式。
关联 Feature Task：无；已交付部署能力的使用文档同步。
当前 Atomic Slice：补充 `docker save/load`、离线数据库镜像、Compose 导入后启动和运行数据边界。
状态：已完成；WIP=0。

## 业务意图

让发布者无需镜像仓库即可通过镜像文件将 FlowSQL 交付至另一台机器，并能正确启动基础或数据库叠加编排。

## Non-Goals

- 不修改 Dockerfile、Compose、构建脚本或程序行为。
- 不实际导出、传输或导入镜像，不启动容器或修改运行数据。
- 不自动提交或推送。

## 核心契约与事实锚点

- `docker save/load` 保留 `flowsql:latest` 镜像与标签；Compose 用 `--pull never` 只使用已导入镜像。
- 完全离线使用数据库叠加文件时，还需交付 `mysql:8.0`、`postgres:16`、`clickhouse/clickhouse-server:24.3` 镜像及 ClickHouse XML。
- 镜像文件不包含 named volumes 的运行数据，已有部署迁移需单独备份与恢复。

## 允许修改文件

- `README.md`
- `tasks/active_task.md`

每次 patch 后执行 `git diff --name-only`，确认只有以上文件。

## 验收命令

```bash
docker compose -f docker-compose.yml config --quiet
docker compose -f docker-compose.yml -f docker-compose.databases.yml config --quiet
git diff --check
git diff -- README.md tasks/active_task.md
```

并只读核对 README 命令中的 Compose 文件、数据库镜像标签与 `docker compose up --pull never` 选项。

## 时间盒与停止条件

- 本轮约 10～30 分钟；新增交付流程与现有 Compose 配置吻合、验收通过后记录证据并停止。
- 仅处理 Docker 镜像文件交付说明。

## 完成证据

- README 已补充 `docker save/load` 的 FlowSQL 镜像文件交付、Compose 配置打包、基础服务导入启动和完全离线的三数据库镜像交付命令。
- 已说明镜像文件与 named volumes 的数据边界、CPU 架构兼容性及 `docker export/import` 不适用。
- 两种 Compose `config --quiet`、README 代码围栏检查及 `git diff --check` 通过；数据库镜像标签与配置文件挂载路径已核对。
- 改动限于 README 和工作台；本轮未提交或推送。
