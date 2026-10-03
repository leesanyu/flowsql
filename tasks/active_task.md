# 即时工作台

事项：依次修复 npm-basic-periodic-stats 检视确认的两个 P2；用户已授权连续执行两个独立切片。
关联 Feature Task：T1 迟到失败诊断、T4 生产链路回归；规格：[归档](archive/feat-npm-basic-periodic-stats.md)。
当前 Atomic Slice：公开 task 保留迟到来源、报文时间、封闭边界；已完成，WIP=0。

## 业务意图

迟到报文触发失败时，公开 task 与 Scheduler SQL 返回可定位的原始诊断，用户能识别来源与时间边界，已交付周期不被修改。

## Non-Goals

- 不修改迟到判定、水位、预算、统计数据、公共接口或错误码。
- 不重新调整已完成的 observed_at 和 Stop 修复；保留先前改动并回归。
- 按用户最新指令本地提交本次检视的三项修复；不推送，不纳入独立采集规划与 Backlog，不混入 NPI 生命周期修复。

## 冻结契约与主链路

- 复用 LastError 的 string 返回与 runtime 已有诊断，无新增公共结构或 ABI。
- ProcessBlock 失败 → task 终态维持原错误码 → LastError 对通用处理错误返回 runtime 详情 → runner/Scheduler 交付完整信息。
- task 的配置/前置条件与 Cancel 错误保持原优先级，后续 Process/Flush/Cancel 不替换第一次失败原因。

## 允许修改文件

- tasks/active_task.md
- tasks/archive/feat-npm-basic-periodic-stats.md
- src/operators/npm_basic/npm_basic_operator.cpp
- src/tests/test_npm_basic/test_npm_basic.cpp
- src/tests/test_scheduler_e2e/test_scheduler_e2e.cpp

基线已有 P1 和 observed_at 修复；保持原样。另有独立 product_backlog.md 和未跟踪 Linux 采集规格，不触碰。

## 本轮步骤与验收

先补公开 task 与 Scheduler PCAP SQL 的迟到诊断断言并确认修复前失败；调整通用处理错误的公开诊断选择，完成局部验证后全量回归并补复检证据。

```bash
cmake --build build --target test_npm_basic test_scheduler_e2e -j8
ctest --test-dir build -R '^test_(npm_basic|npm_periodic_runtime|scheduler_e2e)$' --output-on-failure
cmake --build build -j8
ctest --test-dir build --output-on-failure
cmake --build /tmp/flowsql-npm-periodic-asan --target test_npm_basic -j8
ctest --test-dir /tmp/flowsql-npm-periodic-asan -R '^test_npm_periodic_(contract|stats|runtime)$' --output-on-failure
python3 /tmp/npm-periodic-format.py --check
git diff --check
git diff --name-only
git status --short
```

## 时间盒与停止条件

- 30 分钟；迟到诊断回归先失败后通过、全量构建/CTest/周期 Sanitizer 与格式和范围检查通过即完成并停止。
- 不增加其他任务；出现错误沿用当前边界自主修复。

## 完成证据

- 先前 P1 已完成：Stop 的 pending 事实按序应用，公开 task 与 Scheduler 的 EOF/Stop 等价及 Cancel 回归通过；对应 CTest 4/4，43.76 秒。详见 /tmp/npm-stop-fix-workbench-evidence.md。
- 第一个 P2 已完成：公开 task 的生成时间断言修复前失败；收集器两个 final 投影出口使用实际系统时间，覆盖 router 与直接 Drain。
- 回归：RST、EOF、idle、tuple reuse 验证 Basic 生成时间范围；final 周期列 NULL、first/last 与包/字节计数正确；Basic/Session 共存时 Session 保留原时间参数，配置形式等价比较独立生成时间。
- 编译 test_npm_basic、test_scheduler_e2e 通过，无 Error/Warning；Basic 与周期 runtime 2/2 通过，1.09 秒；Scheduler E2E 1/1 通过，22.30 秒。
- 修改区域 clang-format-18 与 git diff --check 通过。
- 日志：/tmp/npm-p2-time-test-before.log、/tmp/npm-p2-time-build-after.log、/tmp/npm-p2-time-build-final.log、/tmp/npm-p2-time-test-final.log、/tmp/npm-p2-time-scheduler.log。

- 第二个 P2 已完成：公开 task 与 Scheduler SQL 的迟到详情断言修复前均失败；LastError 对通用 runtime 处理错误返回 runtime 详情，其余 task 错误保持原优先级。
- 公开 task 验证 source=7、timestamp_ns=1、closed_boundary_ns=20000000，后续 Process/Flush/Cancel 保留首次错误；已交付周期前缀不改写、不补终态，协议上下文归还一次。Scheduler 实际 PCAP SQL 验证 source=0 与相同时间/边界穿透到响应。
- 局部 Basic/周期 runtime/Scheduler CTest 3/3 通过，23.33 秒；全量 cmake --build build -j8 通过，无 Error/Warning。
- 沙箱外完整 CTest 43/43 通过，74.89 秒，包含先前 Stop 回归及四后端读写、Framework、Scheduler、其他模块。
- 周期 ASan/UBSan/LeakSanitizer 3/3 通过，0.39 秒；重新构建 test_npm_basic 后执行，包含新生成时间、迟到诊断和 Stop 回归。沙箱内首次受 ptrace 限制，沙箱外重跑通过，未关闭泄漏检查。
- 修改区域 clang-format-18 与 git diff --check 通过；允许范围核查通过，独立 Backlog/采集规划保持原样。
- 日志：/tmp/npm-p2-diagnostic-test-before.log、/tmp/npm-p2-diagnostic-test-after.log、/tmp/npm-p2-full-build.log、/tmp/npm-p2-full-ctest.log、/tmp/npm-p2-asan-build.log、/tmp/npm-p2-asan-test.log。
- 两个 P2 连续授权范围已完成并停止；本次检视确认的三项问题均已修复并回归，按用户最新指令本地提交，不推送。
