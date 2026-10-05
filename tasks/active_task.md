# 即时工作台

关联 Feature Task：数据库平台配置维护；规格 [数据库平台](archive/feat-database-platform.md)。
当前 Atomic Slice：取消预置数据库通道。
状态：已完成；WIP=0。用户明确要求删除默认 flowsql_db，已验收并停止本片。

## 业务意图

原生和 Docker 新部署从空数据库通道列表开始，由用户按真实连接参数创建通道，避免展示不存在的默认连接。

## Non-Goals

不删除真实数据库，不改驱动或连接参数解析，不改 Stream 通道，不重启用户服务，不提交/推送。
当前运行配置无默认 flowsql_db，已有 flowsql-mysql 配置逐字节保留；保留前序构建持久化修复及其他未提交工作。

## 冻结契约与主链路

原生 config/flowsql.yml 和 Docker config/docker/flowsql.yml 的 channels.database_channels 为 []。
调整现有部署回归断言，校验两份模板均为空序列；同步 README 的首次部署说明。
首次构建初始化空数据库通道列表；现有运行配置继续由上一切片的初始化保护保留。

## 允许修改文件

- tasks/active_task.md
- config/flowsql.yml
- config/docker/flowsql.yml
- src/tests/test_framework/test_native_deploy_config.cpp
- README.md
- build/**（标准构建生成物；build/output/config/flowsql.yml 不得改变）
- /tmp/flowsql-remove-default-db-20261005/**（基线及验证证据）

基线 /tmp/flowsql-remove-default-db-20261005/baseline.json；前序工作台和允许文件均有快照。
每次 patch 后 git diff --name-only，核对相对本轮基线只修改允许文件。

## 验收命令

```bash
cmake --build build --target test_native_deploy_config test_docker_deploy_config -j8
ctest --test-dir build -R '^(test_native_deploy_config|test_docker_deploy_config)$' --output-on-failure
git diff --check
```

同时验证空目录首次初始化得到空数据库通道列表，以及构建后用户运行配置 SHA256 与本轮基线一致。
修改 C++ 行符合 src/.clang-format；只审查本轮 diff。

## 时间盒与停止条件

2026-10-05 09:01UTC 起 15 分钟，09:16UTC 截止。
验收通过后 WIP=0 并停止；到期只记录完成、检查点通过、当前错误或明确阻塞，不扩展范围。

## 完成证据

- 2026-10-05 09:04UTC 完成；验收后停止，不启动下一切片。
- 原生及 Docker 模板均改为 database_channels: []；其他 Stream 配置保持原值。
- 部署回归断言校验两份模板不预置数据库通道，README 改为由用户在 Web 中创建真实连接。
- 所列两个 CMake target 构建通过，最终日志零 warning/error；相关 CTest 2/2 通过。
- 两份模板在新路径首次初始化后均为空数据库通道列表，且文件字节与模板一致。
- 当前运行配置已无默认 flowsql_db；现有 flowsql-mysql 的文件 SHA256 在构建前后保持一致。
- 修改行 clang-format-18 检查零替换，git diff --check 通过；相对本轮基线无越界修改。
- 前序配置持久化修复、数据库管理测试及用户 Backlog 修改保留；未重启服务、未写数据库、未提交/推送。
- 证据位于 /tmp/flowsql-remove-default-db-20261005：baseline.json、build.log、tests.log 和首次初始化配置。
