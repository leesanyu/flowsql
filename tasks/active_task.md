# 即时工作台

WIP=0；2026-10-16 Asia/Shanghai。

## 最近完成：kafka-packet-sink / T4 / Atomic Slice 10

- 状态：已完成。T1→T2→T3→T4依次验收并勾选；规格归档至[feat-kafka-packet-sink.md](archive/feat-kafka-packet-sink.md)，Backlog[x]，未开始其他任务。
- 交付：严格packet sink配置/共享编码、有界独占writer、网卡异步SQL INTO与Web表单、真实原生/Docker矩阵和部署/故障回归、配对使用说明。
- 最后切片：修正e2e等待态为Scheduler实际的pending；部署测试核对空sink为configuration_required、已安装能力available和async要求。保留100包/实际提交/远端无EOF断言与原超时。
- 验收：全量CMake构建退出0；无筛选all轮`20261015T190756Z-8e9526d7`选中/执行/通过均157，失败/跳过/未执行均0，源码/产物前后摘要一致；前端8/8及build、ASan/UBSan 2/2、28个C++文件格式/版权和Diff检查通过。
- 真实链路：原生/Docker各11身份及续期、独立逐字节核对、真实两进程重启/原密钥恢复；AF_PACKET→Kafka→NPM与本地同批100帧对照，两族basic/session各50包/3200字节，确认100/采集丢包0；源down为source_capture，探针stop不伪造远端EOF。
- 环境：独立受管项目`flowsql_sink_final_104b8f4141`，统一配置`/tmp/flowsql-test-0/flowsql_sink_final_104b8f4141/environment.json`；新项目使用实测空闲端口，原项目/容器/卷保留。Docker/socket与私有namespace权限已在本轮实际验证。
- 证据：`evidence/kafka-packet-sink/t4-acceptance.json`、`accepted-all/`、`t4-accepted-{native,docker,capture,e2e}.log`、`final-environment-location.json`；本地缓存未入库。此前配置预检/测试契约失败轮保留，不计入通过。
- 跨任务记录：NPM两个run同时维护同SQLite时曾出现`npm.basic realtime maintenance failed`，复现日志t4-device-fault-regression.log；本Feature不扩修NPM，e2e对照同批帧并分别收尾本地/远端run。
- 提交收口：依用户“提交代码”指令，仅将本 Feature 已验收的实现、测试、文档和任务记录纳入本地提交；不推送，证据缓存不入库。
