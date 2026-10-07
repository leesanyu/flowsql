# NPM 运行时检视修复

## 业务意图

修复 2026-10-07 检视中动态复现的四项缺陷，恢复离线截止语义、默认输出容量、周期维护成本与标签预算生命周期保证。用户已明确授权本轮依次完成四项；顺序执行，每片重新冻结工作台。

## Non-Goals

不调整公共插件 ABI、结果 Schema、默认预算或协议范围；不重构数据库消费者、测试架构或模块布局；不修本次四项以外的问题；不提交或推送。

## 核心契约与主链路

- 离线：校验当前包 → 按 `max(timestamp)-tolerance` 处理 `deadline <= watermark` 的旧状态 → 当前包会话准入/协议关联；标签预演保持相同顺序。迟到仍按 `timestamp < watermark` 拒绝。当前包导致的关闭仍正常排空。
- 实时：包处理、消费者交付及 ReleaseBlock 成功后提交采集事实；本次不提前实时 watermark。
- 输出：现有 RecordBatch/Schema 和同步 Consume 契约不变；观察实体在任务预算内紧凑累积，Drain 按原顺序交付，消费者失败仅保留已接受前缀；外部 Arrow buffer 保留预算所有者。
- 周期：UTC 网格与乱序/终态语义不变；未到最早截止时跳过全会话维护。
- 标签：matcher 与其准确预留额度同生命周期；正常、失败、取消均只归还自身额度一次。

## Feature Tasks

- [x] T1：交付一致的离线截止顺序，使 TCP/UDP 与 ICMP 超时不再受无关背景包影响，并保留实时采集安全边界。
- [x] T2：交付紧凑的观察结果累积，使默认预算下三万会话 EOF/Stop/集中到期成功交付，并保持消费失败与 Arrow 生命周期契约。
- [x] T3：交付无到期时常数成本的 Basic 周期维护，保持乱序、网格窗口和终态结果正确。
- [x] T4：交付与 matcher 所有权绑定的预算归还，使终态与创建失败的标签预算准确回零。

## 验收

各片运行 test_npm_basic 对应回归和相关协议测试；最后全量构建与 CTest。临时复现程序作修复前后对照，不能替代正式断言。格式检查仅覆盖本次修改行，保留原有 Backlog 修改。

## 完成证据

- T1：TCP/UDP 与 ICMP 的截止前、等于、之后，容忍乱序、标签开关、背景包和跨 batch 矩阵通过；新增断言在修复前复现失败。共享 DNS/HTTP/TLS、实时事实交付及 ReleaseBlock 既有回归通过。
- T2：默认预算下三万会话 final EOF、periodic EOF、实时正常 Flush、周期集中到期均成功，前三场景 pending 峰值 7,421,952 字节，周期集中到期 7,290,816 字节。Schema 身份、空值/切片、顺序/revision、消费者失败前缀与外部 Arrow 生命周期断言通过。
- T3：最早周期/终态截止门槛跳过无到期扫描；乱序提前首窗口、RST 提前终态、EOF 与周期结果回归通过。中断前相同未优化构建的诊断：五千会话、两千无到期包，periodic 约 218 ms → 16 ms；该历史临时日志已随环境重建丢失，数字不作为正式性能验收或生产吞吐结论。
- T4：精确预留额度跟随 matcher 所有权归还；0、3 MiB、64 MiB × 正常 EOF、重复 Cancel、早期 Schema 失败、pipeline 获取失败、consumer factory 失败、运行失败矩阵通过。每个 matcher Release 一次，其他模块的 128 字节预留保留；外部 Arrow 输出继续保有其真实 pending 预算。
- 本轮恢复后全仓构建通过，日志 warning/error 均为零；完整 CTest **49/49** 通过，包含真实 DPDK 标签插件、SQLite 与 MySQL/PostgreSQL/ClickHouse 后端、Scheduler/Web 集成。
- 恢复后重新构建并执行 ASan+UBSan **5/5** 通过：test_npm_basic、periodic runtime、TCP stream runtime、periodic contract、periodic stats。使用 ASAN_OPTIONS=detect_leaks=0、UBSAN_OPTIONS=halt_on_error=1；不宣称 LeakSanitizer 验收。
- 修改行 clang-format-18 零替换，统一版权检查和 git diff --check 通过。生产代码新增 144 行、删除 158 行；无公共插件 ABI/结果 Schema/预算上限改动。
- 中断前曾出现真实 DPDK 路径段错误与 MySQL 连接失败；恢复后 GDB 完整执行正常退出、完整 CTest 通过，未再复现，没有将未定位的历史异常宣称为已修复缺陷。
- 原有 Backlog 两处采集通道目标修改完整保留；恢复前后运行配置 SHA256 一致；未提交或推送。
- 可复核日志保存在 build/npm-review-evidence：full-build.log、full-ctest.log、full-ctest-details.log、asan-build.log、asan-ctest.log、asan-details.log、diff-audit.json。旧 /tmp 日志不再作为当前证据链接。

完成时间：2026-10-07 06:36 UTC。
