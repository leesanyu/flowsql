# 即时工作台

WIP=0；受管测试资源回收缺陷修复已完成，当前无活动 Atomic Slice。

- 关联 Feature：已归档 `test-environment` 的生命周期缺陷修复，不启动 Kafka packet Feature。
- 业务意图：临时测试项目结束后无容器/卷/网络残留；可复用项目提供显式、可预览、严格归属的清理入口。
- Non-Goals：不全局 prune，不清业务/未知资源，不重置账号/凭证，不改网络/WSL，不实现原生 Kafka 进程管理，不提交或推送。
- 时间盒：2026-10-15 12:09～12:39 UTC（30分钟）；到点只报告当前检查点，不扩大范围。
- 允许文件：本文件；`tools/test_environment/{cleanup.py,runner.py,acceptance.py}`；`src/tests/test_environment/{test_cleanup.py,test_runner.py,CMakeLists.txt}`；`docs/testing.md`。忽略的证据仅写 `evidence/test-environment/`。
- 保留既有变更：`tasks/product_backlog.md`、两份 `feat-kafka-packet-{source,sink}.md`；摘要见本地 evidence。

## 契约与本轮步骤

1. `cleanup_project(project, apply=False)` 返回只含非秘密资源身份的预览；显式 apply 才删除。仅接受精确 `flowsql.test.project` 标签，拒绝冲突 Compose 归属、跨项目卷/网络引用。删除失败非零；空项目重复清理成功。
2. 测试持有独立项目活动共享锁；清理持有非阻塞独占锁，遇活动项目拒绝。`temporary_project(project)` 在 finally 回收本轮临时 Docker 资源，覆盖正常/失败/部分准备失败。
3. CLI：`tools/test.sh cleanup --project NAME [--apply]`；无需秘密配置即可处理孤立卷。可复用主测试资源不随普通测试自动删除。
4. 验证通过后仅清理用户已确认审计文件 `evidence/test-environment/docker-resource-audit-20261015.json` 的18容器/42卷/6网络；删除前逐项复核身份、状态与引用，保留其他资源。

## 测试依赖、验收与停止条件

- Python 单测：原生，无外部服务；验证预览/成功/失败/重复、锁、越权拒绝与 CLI 参数。
- CMake：入口 `cmake -B build src`，构建 `test_environment_config test_environment_probe test_environment_kafka_probe`；标准 `tools/test.sh unit --filter '^test_environment_(config|fixture|runner|cleanup)$'`。
- Docker 生命周期：原生 Python 调用当前 Docker socket，固定镜像来自统一运行时配置；使用全新隔离项目、私有 Linux STATE 与自动生成秘密，不猜测/改写既有凭证。真实验证临时数据库成功/失败回收及孤立卷重复清理，Docker socket 需要工具授权。
- 清理与基线审计只输出 ID/名称/标签/状态/引用，不输出环境变量或凭证；日志放忽略的 evidence。
- 停止条件：回归通过、临时资源零残留、授权清单清理且非目标资源保留、Diff和既有文件摘要检查通过，更新WIP=0后停止。不是本轮完整Feature验收，不引用历史all结果替代本轮执行。

## 完成证据（2026-10-15 12:27 UTC）

- 根因：临时项目仅执行 Compose stop，留下容器/卷/网络；改为覆盖准备和测试的 finally 回收，回收失败上报。
- 新增默认预览、显式 apply 的按项目 Docker 清理入口；精确标签与引用检查、项目活动锁、孤立卷与重复执行均有回归。
- 配置及三个 CMake target 构建成功；四组测试环境回归 4/4，完整 `tools/test.sh unit --jobs 4` selected/executed/passed=104，failed/skipped/not_executed=0；源码/产物快照稳定。
- 全新隔离 Docker 生命周期 9/9：三库实际认证及写权限、成功/失败/部分准备失败回收、活动锁、预览/孤立卷/重复、共享卷拒绝；非目标资源完整保留。证据 `evidence/test-environment/cleanup-fix/lifecycle.json`。
- 已按授权清单精确删除18容器/42卷/6网络，14个项目归零；原有三个数据库ID不变且running，其他容器/卷/网络前后快照一致。证据 `cleanup-fix/authorized-cleanup.json`；正式 CLI 对空项目 apply 再执行成功。
- 首次 CTest 受既有工作目录错误影响，执行0项；保留失败证据，路径核查后重跑及完整unit通过。首次验证快照比较未收敛，调整为规范化挂载顺序并保存前后快照后完整复验通过；该验证自行创建的匿名卷通过Docker事件核实并清理。
- 最终证据 `evidence/test-environment/cleanup-fix/final-verification.json`；原有Backlog和两份packet草稿摘要保持；暂存区为空，未提交或推送。本轮未运行完整integration/all，不声称原生Kafka进程回收或SIGKILL自动回收。
- 在时间盒内完成并停止，不启动后续任务。
