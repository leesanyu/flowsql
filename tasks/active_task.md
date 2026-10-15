# 即时工作台

WIP=0；当前无活动 Atomic Slice。

## 最近完成：Kafka 结构化数据接入 (`kafka-log-source`)

- 已依次实施、验收并勾选 T1→T2→T3→T4；规格归档至 tasks/archive/feat-kafka-log-source.md，Backlog 已完成。
- 交付固定 JSON→Arrow Schema/字段类型/单位/null契约、认证复用消费、任务独占reader、成功处理后显式确认、Web配置与异步SQL接线。
- 最终构建返回0；完整 all selected134/executed134/passed134/failed0/skipped0/not_executed0；原生/Docker结构化数据面各11身份、认证矩阵各21项与部署回归通过；前端37/37及build通过；格式/Diff检查通过。
- 完整验收使用统一配置的运行时副本，仅更换独立测试项目名；旧同名数据库卷保持原样。源码/产物前后摘要一致，初始97路径中非本Feature摘要、HEAD与暂存区保持。
- 完成证据 evidence/kafka-log-source/、evidence/test-environment/20261015T053828Z-540d2aa4/，均未入库；此前失败记录保留，不计入通过。
- 确认只覆盖当前批次处理及当次输出投递；不承诺最终事务原子、有状态恢复或Exactly-once。用户说明 docs/kafka.md。
- 跨任务观察：catalog/builtin卸载后同进程重载出现重复注册失败，未改该模块；脱敏复现 evidence/kafka-log-source/t4-catalog-reload-observation.log。
- 无后续任务自动启动；未执行 git add、commit 或 push。
