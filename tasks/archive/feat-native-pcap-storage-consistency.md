# Feature: 原生 PCAP 存储生命周期一致性

状态：`[x]` 已完成
优先级：P0
前置 Feature：`pcapfile-channel-persistence`（已完成）

## 业务意图

原生单进程和 Guardian 部署应让受管 PCAP 文件与其 SQLite 持久记录共享同一运行目录生命周期，避免
`/tmp` 被清理而 `build/output/meta` 保留后，严格启动恢复因文件永久缺失而失败。

`start.sh` 先进入 `build/output` 再启动服务；Web 使用 `upload_dir=./uploads` 后仍由
`ManagedCaptureStore` canonicalize 文件路径并向 Scheduler 传递绝对路径。数据库继续使用
`db_path=./meta/flowsql_meta.db`，两者共同归属 `build/output`。

## Non-Goals

- 不修改 pcapfile、Web 或 Scheduler 生产实现，不改变控制面/数据面、ABI 或 SQL 语义。
- 不放宽 `PcapFilePlugin::Start()` 的全量或零恢复，不跳过、自动删除或部分恢复失效记录。
- 不恢复已经丢失的文件，不删除目标两条失效记录以外的数据。
- 不修改 Docker；其数据库和上传文件继续共同位于 `pcap-uploads` named volume。

## 配置与恢复契约

```text
build/output/
├── meta/flowsql_meta.db
└── uploads/pcapfile/*.pcap
```

- `deploy-single.yaml` 和 `deploy-multi.yaml` 的 Web option 都使用 `upload_dir=./uploads`。
- 删除整个 `build/output` 会同时删除原生数据库和受管文件；备份或迁移时也应将二者作为同一运行数据单元。
- 已存在的失效记录只通过显式运维修复处理：先确认服务停止并备份数据库，再以事务和精确断言删除
  `pcapfile.http`、`pcapfile.mq`；任何事实漂移都停止操作。
- 任意其他持久记录的文件缺失、非普通文件、符号链接或格式损坏，启动仍明确失败。

## 主链路

1. Web 将上传文件落到 `build/output/uploads/pcapfile/`，canonicalize 后创建持久 `pcapfile` 通道。
2. 服务重启时 Scheduler 从 `build/output/meta/flowsql_meta.db` 恢复通道，并打开同一运行目录中的受管文件。
3. 若记录或文件无效，插件保持严格失败；管理员修复文件或记录后再启动。

## 原子任务

- `[x]` T0：以部署测试冻结 `./uploads` 契约，更新原生配置和 README，备份数据库并精确清理两条已确认
  失效记录，完成两次启动烟测、定向测试和全量回归。

## Feature 验收

- 原生单进程和 Guardian 配置、已暂存运行配置均不再引用 `/tmp/flowsql/uploads`。
- 部署测试同时冻结 `upload_dir=./uploads` 和 `start.sh` 的 `build/output` 工作目录。
- README 明确原生数据库/上传目录的共同生命周期，并保留 Docker named volume 说明。
- 数据库备份哈希与修复前原库一致；只删除目标两条失效记录，其他表仍存在。
- 连续两次 `./start.sh` 启动不再出现 pcapfile 恢复失败，停止后无残留进程。
- 定向构建/测试、完整构建/CTest 和 `git diff --check` 全部通过。
