# 即时工作台

关联 Feature Task：T5；规格：[NetAdapter](archive/feat-npm-linux-capture-backends.md)。
当前 Atomic Slice：已完成NetAdapter Feature的本地提交与精确暂存审计。
状态：已完成；WIP=0。T0～T5与完整DoD已完成；用户明确授权本地提交，本片审计后提交并停止。

## 业务意图

以完整构建/CTest/格式检查及原始证据完成NetAdapter Feature验收，使T0～T5状态与实际交付一致。

## Non-Goals

不新增功能、不修改生产行为/ABI/配置/Schema/进度门禁，不删除失败证据，不推送远端。
不移动/删除目录；仅在DoD完整后将规格文件归档，不声称物理网卡吞吐或同接口/同目标并发支持。

## 冻结契约与主链路

前序验收：三后端真实收包、多队列T4、采集阶梯48项（4个AF_PACKET高负载失败保留）；
无过滤SQL15/30/60秒、功能15/15、剩余周期4/4、持续25tick/s三后端×两帧長×四profile24/24。
慢分析/慢输出6项、受控过载恢复三后端实际通过；压力阶段真实丢包不计入零丢包持续能力。
完成证据逐项保存命令、预设阈值、范围、原始JSON/log和失败边界，引用历史事实必须有实际文件。
全量build/完整CTest100%通过，全部Feature变更C++格式、shell/Python语法、diff检查通过后：
规格勾T5/Feature完成，将文件移入tasks/archive；Backlog行勾完成/改链接，工作台记录最终证据。

## 允许修改文件

- tasks/active_task.md
- tasks/specs/feat-npm-linux-capture-backends.md（完成证据/状态及移出规格文件）
- tasks/archive/feat-npm-linux-capture-backends.md（仅该规格文件归档）
- tasks/product_backlog.md（仅该Feature状态/规格链接）
- docs/netadapter.md（最终证据/边界/命令）
- src/channels/netadapter/pfring-8.8.0-close.patch（仅规范补丁文本空白）
- src/channels/netadapter/pfring-8.8.0-netns.patch（仅规范补丁文本空白）
- build/netadapter-validation/**（日志/证据清单/快照）

生产与测试代码本片只读，提交范围为已验收NetAdapter实现/公共V2迁移/诊断/测试/文档与完成状态。
精确路径清单/tmp/flowsql-netadapter-commit-paths.txt；Backlog仅NetAdapter行入index，
独立native zero-copy与DPDK用户态采集规划两行保持工作树未暂存；build原始日志/缓存不入提交。
每patch后git diff --name-only；核查空index、暂存完整清单与内容哈希、git diff --cached --check后本地git commit。
用户本轮“提交代码”已明确授权，不推送。

## 验收命令

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
clang-format-18 --dry-run --Werror <全部Feature变更的C++文件>
bash -n src/tests/test_netadapter/run_isolated_validation.sh
python3 -m py_compile src/tests/test_netadapter/run_validation_matrix.py
git diff --check
git diff --name-only
```

完整CTest如有sandbox网络/权限失败则按权限流程复验，不把skip当真实网卡验收。

## 时间盒与停止条件

2026-10-04 17:03UTC起10分钟，17:13UTC截止；精确index审计通过后本地提交、核查HEAD与剩余两项规划改动并停止。

## 完成证据

2026-10-05（北京时间）T5完整验收通过：功能15/15、剩余周期4/4、持续24/24、慢分析/慢输出6项及最终恢复3项通过。
AF_XDP旧15/30/60秒原数据库一致性3/3通过，T4双TAP四RX队列实发/回收证据已复核。
完整build通过且无Error/Warning；全部42个Feature变更C++格式、shell/Python语法及diff检查通过。
首轮沙箱完整CTest44/48（socket/数据库权限失败），获准升级后完整48/48通过（66.83秒），两轮日志均保留。
完整原始日志：t5-final-full-build.log、t5-final-format.log、t5-final-full-ctest-unrestricted.log。
机器核查的完整证据清单build/netadapter-validation/t5-completion-evidence.json，持续汇总t5-sustained-25-aggregate.json。
可持续声明限WSL2隔离Ethernet25tick/s各选定工况；历史高负载/并发/权限失败原始记录保留，不提高阈值或放宽门禁。
规格T5已勾选并归档，Backlog该Feature已完成/改链接。
用户本轮授权提交：复核完整DoD日志、当前验证入口/二进制哈希和精确提交范围；复用已通过48/48完整CTest。
仅提交已验收Feature，独立规划两行与build证据保持原样，不推送；本地提交结果以Git命令及HEAD核查为准。

暂存审计发现两份PF_RING补丁空白context行尾空格；仅缩减空白上下文，以原始代码片段git apply --check验证，生产代码不变。
