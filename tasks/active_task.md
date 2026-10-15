# 即时工作台

WIP=0；`kafka-packet-source` 已完成，当前无执行中的 Atomic Slice。

- 用户要求 T1→T2→T3→T4 连续实施；各 Task 完整验收后均先在规格勾选，再推进下一 Task。
- 最后切片：T4 / Atomic Slice 8（时序回归同步与最终验收），状态“已完成”；2026-10-16 Asia/Shanghai。
- 交付：严格 flowsql_packet_v1 配置/codec；独占 Capture V2 reader、位点确认/拓扑归属门禁；Kafka→NPM 实时托管路径与 Web；真实身份/部署/失败/重启回归及用户说明。
- 最终验收：标准全量 CMake 构建通过；不筛选 all（20261015T160222Z-c6564005）selected/executed/passed=148，failed/skipped/not_executed=0；清单一致，源码和产物前后摘要稳定。
- 实际位置覆盖：原生和 Docker 各11种 packet 身份组合；GSSAPI/OAuth 更新后消费；ACL/坏消息/提交不确定、重放/续读、分区归属/epoch、进程新run/空状态、部署重启均通过。
- 其他检查：前端8个测试文件/build，ASan/UBSan，22个C++文件 clang-format，git diff --check 及本Feature Diff审查通过；Scheduler同步回归定向4/4通过，并在最终all再次通过。
- 环境：默认测试项目旧卷所有权不明，未复用或重置；从默认配置派生UUID受管项目 flowsql_packet_accept_d708cf4975，所有子进程共用私有运行配置。失败准备轮和旧Scheduler断言失败轮保留，不计通过。
- 本地证据：evidence/kafka-packet-source/final-acceptance.json、t4-all-final.log、t4-scheduler-red-observation.json、t4-diff-review.json；统一完整清单及摘要位于 evidence/test-environment/20261015T160222Z-c6564005/，均未入库。
- 规格已移入 tasks/archive/feat-kafka-packet-source.md，T1～T4均已勾选；Backlog source已[x]；只修复sink草稿两处共享契约链接，不实施sink。
- 用户说明：docs/kafka.md 的原始包packet source、外部发布格式、进度/重启和验证/性能章节；README及docs/npm.md有入口。
- 边界：单分区0/Ethernet/一个活动发布者；不承诺Exactly-once或跨重启内存会话连续；固定负载成本样本不代表生产容量。
- 提交收口：用户已明确授权本地提交；允许纳入本Feature已验收代码/测试/文档、归档规格、本工作台及Backlog的source完成行。sink草稿及其Backlog行保留在工作区，不纳入提交；归档规格对未入库草稿仅作文字引用。
- 提交前核对148/148验收的源码/产物摘要、精确暂存清单和Diff；不push，不自动开始其他Feature。
