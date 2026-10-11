# 即时工作台

WIP=0；`baseliner-contract-safety` T4 最终验收完成，Feature 已归档，本切片完成即停。

## 当前 Atomic Slice

- 关联 Feature Task：`baseliner-contract-safety` / T4；沿用原编号。
- 用户授权：NPI 问题已剥离为独立任务，本轮聚焦本 Feature，将 T4 收口完成。
- 业务意图：在当前一致源码下验收任务排空、取消期间会话有效及类型化就绪契约，形成可复核的完整回归、兼容、成本和归档证据。
- Non-Goals：不实施 NPI wire/分析地址新布局，不新增网络字段/规则成本任务；不修改生产源码、CMake 或测试；
  不撤销既有源码，不调整算法、源数据、结果 Schema、配置或 checkpoint，不提交/推送。
- 允许文件：
  - `tasks/active_task.md`
  - `tasks/product_backlog.md`
  - `tasks/specs/feat-baseliner-contract-safety.md`
  - `tasks/archive/feat-baseliner-contract-safety.md`（验收通过后归档同一规格文件）
  - `tasks/specs/feat-npi-wire-layout.md`（仅同步跨 Feature 责任与链接）
  - `docs/baseliner-acceptance.md`
  - 临时证据 `/tmp/baseliner-contract-safety-closeout/`；复用既有仪器化 build 和主工作区依赖缓存。
- 实施步骤：解除等待新网络布局的外部前置；标准配置/全量构建/完整 CTest；当前定向 ASan/UBSan/LSan、
  真实 `.so` 与数据库生产回归；固定 CPU 五轮契约及既有路径成本；格式/严格编译、兼容和增量 diff/hash 审查；通过后勾选 T4 并归档。
- 验收命令：`cmake -B build src`、`cmake --build build -j4`；
  `setpriv --bounding-set=-net_admin ctest --test-dir build --output-on-failure --output-junit /tmp/baseliner-contract-safety-closeout/full-tests.xml`；
  `python3 /tmp/baseliner-contract-safety-closeout/run_checks.py sanitizer`（保持 detect_leaks=1）；
  固定 CPU 的五轮 `test_baseliner_contract_cost` / `test_baseliner_cost`；修改区间 clang-format-18、C++17 严格语法编译，
  公共头/codec/契约与 HEAD 比对；每次 patch 后 `git diff --name-only` 和增量范围/hash 检查，最终 `git diff --check`。
- 环境边界：沿用已有 WSL 去除 net_admin 条件；真实后端不跳过，凭据只从已有本机私有 options 文件读取。
  若受沙箱网络/ptrace 限制，保留原日志并按工具权限流程复跑；不关闭检查器或加入抑制。
- 时间盒：30 分钟，2026-10-11 15:24～15:54（Asia/Shanghai）。
- 停止条件：全部验收和归档完成即停，WIP=0；若时间盒到期或出现当前范围外阻碍，记录明确检查点，不混入 NPI 实施任务。

## 起点与证据

- T1/T2/T3 已完成；旧 T4 中 NPI 对齐/池/parser/Regex 的原始证据保持，不能作为尚未实施新布局的验收。
- 用户本轮授权将网络新布局的最终部署验收留在 `npi-wire-layout`，本 Feature 独立收口；
  当前版本 PCAP → NPM → Baseliner 仍参加普通及定向 Sanitizer 回归。
- 起点 status/diff、988 个文件 SHA256 与允许列表保存在 `/tmp/baseliner-contract-safety-closeout/`。

## 本轮完成证据

- 当前标准配置/全量构建通过，一次完整 CTest 83/83、1050.02s，failures/errors/skipped=0。
- 当前定向 ASan/UBSan/LSan 11/11、144.50s；43 组生产分析与 43 次插件重载恢复通过，源 Schema/内容保持。
- 213 个仪器化翻译单元/202 个源文件、15 个 FlowSQL .so 核验；第三方普通缓存边界明确，LSan 保持开启且无抑制。
- 两项固定 CPU 成本各五轮通过；格式/版权 20 文件，严格 C++17 编译 12 单元与旧公共头/codec/配置兼容核验通过。
- 普通完整回归沿用 WSL 去除 net_admin 条件；沙箱启动失败和 IPv4 超时原日志保留，临时路由已恢复并核对。
- T4 勾选，规格 [归档](archive/feat-baseliner-contract-safety.md)，Backlog 标记完成；NPI 新布局四项实现任务状态保持。
- 本轮仅修改冻结的六个文档路径（含规格归档），所有既有生产源码/CMake/测试保持起点 SHA256；无越界。
- 完整证据 /tmp/baseliner-contract-safety-closeout/；未提交/推送，不自动开始 NPI 或后续任务。
