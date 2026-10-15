# 即时工作台

WIP=0；当前无活动 Atomic Slice。

## 最近完成：分析结果 Kafka 发布 (`kafka-result-sink`)

- T1～T5 已全部验收并勾选；规格归档至 `tasks/archive/feat-kafka-result-sink.md`，Backlog 已标记完成。本轮 T5 在09:42～10:12 UTC时间盒内收口。
- 交付 `INTO kafka.<name>` 的数据库查询、只读 DataFrame 快照与兼容算子结果发布，携带无损 JSON 信封、稳定 key/revision 和确认摘要；成功要求 delivery 回调全部确认。
- 重启后已核验受管容器/卷标签并恢复原匹配凭证，保留账号和数据卷；统一运行时配置使用私有绝对引用，命令从规范 `flowSQL` 路径执行。两位置只读 doctor 各26项通过。
- 当前完整构建退出0；无筛选 all（`20261015T095158Z-f3769946`）selected/executed/passed=140，failed/skipped/not_executed=0；源码/产物在执行前后和最终复核均一致。
- 原生/Docker 真实认证各21项、source/sink各11种身份与部署恢复均通过；包含独立消费者、重执行重复、负向权限/认证/缺Topic/超限、部分发布、续期及真实受控断连。断连均保留confirmed=3/unknown=3。
- 前端39/39与生产构建、重新构建的codec/writer/control ASan/UBSan/leak、11个新增C++文件clang-format及Diff检查通过。构建既有getcwd与前端bundle大小提示保留。
- 本轮固定负载样本：1000行、128字节文本、单writer/分区、Write+Finish计时；native约13282条/秒、10.53MB/秒、CPU27.0%、峰值RSS55236KiB、取消2ms；Docker约13424条/秒、10.64MB/秒、CPU28.3%、峰值RSS55620KiB、取消3ms。未固定CPU亲和性，仅为本地短样本，不代表生产容量。
- 完成证据 `evidence/kafka-result-sink/t5-final-acceptance.json`、`evidence/test-environment/20261015T095158Z-f3769946/`；旧失败/恢复日志和切片工作台快照均保留，证据未入库。用户说明 `docs/kafka.md`。
- 本轮源码与两份packet规格及其他既有改动保持；不承诺Exactly-once、跨系统事务、NPM全部实体直接发布；未启动后续任务，未执行git add/commit/push。
