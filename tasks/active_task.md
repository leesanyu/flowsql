# 即时工作台

事项：将 Docker 数据库扩展 Compose 文件改为准确名称。
关联 Feature Task：无；部署配置命名修正。
当前 Atomic Slice：把 `docker-compose.full.yml` 改名为 `docker-compose.databases.yml` 并同步当前引用。
状态：已完成；WIP=0。

## 业务意图

让文件名表明它为基础 Compose 增加 MySQL、PostgreSQL、ClickHouse 及 Scheduler 的数据库就绪依赖。

## Non-Goals

- 不改变服务、数据卷、端口或数据库配置语义。
- 不启动或停止容器，不迁移数据库数据。
- 不重写历史 Sprint 记录，不提交或推送。

## 核心契约与事实锚点

- 新文件作为 `docker-compose.yml` 的叠加文件使用，合并后服务与卷保持一致。
- 部署契约测试读取新路径，并继续检查 PostgreSQL 服务。

## 允许修改文件

- `tasks/active_task.md`
- `docker-compose.full.yml`（重命名前路径）
- `docker-compose.databases.yml`（重命名后路径）
- `src/tests/test_framework/test_docker_deploy_config.cpp`

每次 patch 后执行 `git diff --name-only` 和 `git status --short` 检查改动边界；仓库其他已有未提交改动保持原状。

## 验收命令

```bash
docker compose -f docker-compose.yml -f docker-compose.databases.yml config --quiet
cmake --build build --target test_docker_deploy_config -j8
./build/output/test_docker_deploy_config
clang-format-18 --dry-run --Werror src/tests/test_framework/test_docker_deploy_config.cpp
git diff --check
```

## 时间盒与停止条件

- 本轮约 10～30 分钟；文件改名、引用更新及上述校验通过后记录证据并停止。
- 仅处理 Compose 叠加文件命名及直接引用。

## 完成证据

- `docker compose -f docker-compose.yml -f docker-compose.databases.yml config --quiet` 通过。
- `cmake --build build --target test_docker_deploy_config -j8` 与 `./build/output/test_docker_deploy_config` 通过。
- `clang-format-18 --dry-run --Werror` 与 `git diff --check` 通过。
- 当前代码和 Compose 使用处均改为新文件名；历史 Sprint 记录保留原名。未提交或推送。
