# 即时工作台

事项：在 README 补充 Docker 镜像发布与 Compose 部署方式。
关联 Feature Task：无；已交付部署能力的使用文档同步。
当前 Atomic Slice：只更新快速开始中的镜像构建、服务编排、跨机器发布和数据持久化说明。
状态：已完成；WIP=0。

## 业务意图

让发布者按当前 Dockerfile 与两份 Compose 文件构建并分发镜像，明确基础服务和数据库叠加服务的启动方式。

## Non-Goals

- 不修改 Dockerfile、Compose、构建脚本或程序行为。
- 不发布镜像、不启动容器、不修改线上数据。
- 不自动提交或推送。

## 核心契约与事实锚点

- Dockerfile 从本地 `build/output` 和依赖缓存复制产物；Scheduler 固定加载 Flow Labeling 插件，构建时需保证产物存在。
- 两份 Compose 均引用 `flowsql:latest`；数据库文件只作为基础 Compose 的叠加文件使用，ClickHouse XML 从部署目录挂载。
- Docker 的运行数据使用 named volumes；首次创建 `flowsql-config` 后，更新镜像不会覆盖卷内配置。

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

并只读核对 README 命令引用的文件、目录及构建输入实际存在。

## 时间盒与停止条件

- 本轮约 10～30 分钟；文档与配置吻合、验收通过后记录证据并停止。
- 仅处理 Docker 镜像发布与部署用法。

## 完成证据

- README 已补齐镜像构建、基础与数据库叠加编排、跨机器发布、初始数据库配置和 named volume 升级说明。
- 两种 Compose `config --quiet`、`git diff --check` 通过；README 代码围栏成对。
- 已核对 Dockerfile 引用的构建产物，以及数据库叠加文件引用的 ClickHouse XML 文件均存在。
- 改动限于 README 和工作台；本轮未提交或推送。
