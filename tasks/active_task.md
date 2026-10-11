# 即时工作台

WIP=0；已验收的 NPI wire-layout 与 Baseliner 恢复修复已完成提交审查，源码冻结。

- 用户授权：明确要求“提交代码”；关联 npi-wire-layout T1～T4 和已授权、已验收的 Baseliner 恢复测试修复。
- 业务意图：将验收代码与规格、索引建议准确提交，形成可独立审查的 NPI 和 Baseliner 两个本地提交。
- Non-Goals：不新增实现、不启动 npm-ip-family-pipeline、不修改已验收源码、不推送。
- 允许编辑：tasks/active_task.md；允许暂存及提交的完整路径按下列两组冻结。
- Baseliner 提交：README.md、src/operators/baseliner/durable_store.cpp、
  src/tests/test_baseliner_contract/test_baseliner_store.cpp、src/tests/test_baseliner_contract/test_baseliner_recovery.cpp。
- NPI 提交：src/common/network/netbase.h、src/common/network/wire.h、src/common/algo/objects_pool.hpp、
  src/plugins/npi/layer.cpp、src/plugins/npi/engine.cpp、src/plugins/npi/packet_decoder.cpp、src/plugins/npi/regexmatch.cpp、
  src/operators/npm_basic/core/npm_session_key.cpp、src/operators/npm_basic/modules/icmp/npm_icmp_parser.cpp、
  src/channels/pcapfile/packet_filter_domain.cpp、src/tests/test_packet_contract/CMakeLists.txt、
  src/tests/test_packet_contract/test_packet_contract.cpp、src/tests/test_packet_contract/test_wire_layout.cpp、
  src/tests/test_packet_contract/test_npi_alignment.cpp、src/tests/test_npm_basic/test_npm_basic.cpp、
  src/tests/test_npm_basic/test_npm_icmp_parser.cpp、src/tests/test_config_channel/test_config_channel_e2e.cpp、
  tasks/active_task.md、tasks/specs/feat-npi-wire-layout.md（归档旧路径）、tasks/archive/feat-npi-wire-layout.md、
  tasks/product_backlog.md、tasks/specs/feat-npm-ip-family-pipeline.md（仅已有规划）、
  tasks/archive/feat-baseliner-contract-safety.md、docs/baseliner-acceptance.md（仅归档链接）。
- 前置依赖：对象池及 Regex 释放修复为本轮起点既有代码；新增池析构和真实 .so 生命周期回归直接依赖，随 NPI 提交。
- 时间盒：2026-10-11 23:53～2026-10-12 00:13，20 分钟。
- 验收命令：按文件 SHA256 核对验收快照；解析完整 CTest 84/84、NPI Sanitizer 7/7、恢复 Sanitizer 2/2 报告；
  git diff --check、git diff --cached --check、git diff --cached --name-only；每组提交前精确核对暂存内容。
- 停止条件：两个本地提交完成、提交树与审核清单一致、源码哈希不变、工作台 WIP=0；不开始后续 Feature。
- 已有完成证据：npi-wire-layout T1～T4 已归档；全构建与完整 CTest 84/84；NPI Sanitizer 7/7、恢复 2/2；
  严格编译 12 项、格式/版权及五轮固定 CPU 成本通过，详见归档规格。
- 证据目录：/tmp/flowsql-submit-npi-wire-layout/；此前日志保留各自真实路径，不重复无变化实现的全量测试。

## 本轮验收与提交出口

- 当前 793 个源码文件与最新完整 CTest 验收快照一致；15 个 NPI 直接相关文件也逐一对应 T4 验收哈希。
- 完整 CTest 84/84、NPI ASan/UBSan/LSan 7/7、恢复 ASan/UBSan/LSan 2/2 均无失败、错误或跳过。
- 对象池及 Regex 前置修复与验收源码一致，版权及 clang-format 检查通过；本轮没有修改源码或测试。
- Baseliner 的 4 个文件已精确审核并提交为 7efb672；NPI 的 24 个路径与本工作台一起提交，
  最终提交记录以 git log -2 及 /tmp/flowsql-submit-npi-wire-layout/final-audit.json 为准。
- 暂存内容逐文件与审核快照核对；源码、已有规划和归档正文保持本轮起点内容。
- 停止：npi-wire-layout 已完成；npm-ip-family-pipeline 保持待实施；用户只授权本地提交，不推送。
