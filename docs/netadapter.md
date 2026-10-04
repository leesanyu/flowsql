# NetAdapter 采集与联验

`netadapter` 用同一个具名 source 接入一个观测域中的多张 Ethernet 网卡；创建配置选择
`af_packet`、`pfring_classic` 或 `af_xdp_copy_skb`。三后端完整性能矩阵仍按规格 T5 验收。

## 构建与插件

```bash
cmake -B build src
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

在 Scheduler 所在进程加载 NPI、NetAdapter 管理插件和所选后端 provider。普通部署清单需增加以下条目，
并按框架批次完成所有插件的 Option、Load、Start：

```yaml
- name: libflowsql_netadapter.so
  option: '{"db_path":"./meta/netadapter.db"}'
- libflowsql_capture_af_packet.so
```

PF_RING 与 AF_XDP 分别使用 `libflowsql_capture_pfring.so` 和 `libflowsql_capture_af_xdp.so`。
PF_RING 用户态库通过 CMake 的 `FLOWSQL_PFRING_ROOT` 查找，默认主工作区 `.thirdparts_prefix/pfring`；
AF_XDP 通过 pkg-config 查找 libxdp/libbpf。缺库的 provider 明确返回不可用，不切换后端。
PF_RING 还需对应运行内核的模块；AF_XDP 需要可创建 BPF/XSK 的权限，使用 SKB/COPY 模式，
发现并打开全部 RX 队列，拒绝已有 XDP owner 的接口。

PF_RING 的真实 smoke 接受两个接收接口和对应的两个发送 peer，实际注入并核对各输入的标记帧、
48 字节截断/64 字节 wire length、时间戳、可用计数、借用归还、Cancel 和 provider 租约。
在已加载 `pf_ring` 的 Linux 环境可执行以下隔离命令；缺模块、未收包均非零失败。
无参数 CTest 只验证失败诊断，不代表真实收包通过。

```bash
unshare --net bash -c '
set -e
ip link set lo up
ip link add na0 type veth peer name np0
ip link add nb0 type veth peer name nq0
for capture_interface in na0 np0 nb0 nq0; do
    ip link set "$capture_interface" up
done
build/output/test_pfring_backend na0 nb0 np0 nq0
'
```

PF_RING 8.8.0 在当前部署中核查出两处上游生命周期问题，补丁均针对提交
`67804770402ae7a9c6675814501ef86d1cef71c1`，不改变公开 ABI：

- [用户态 close 补丁](../src/channels/netadapter/pfring-8.8.0-close.patch)：将 runtime manager 的环境变量判断
  放入 `HAVE_DL_REDIS` 条件内。原版无 Redis 构建会跳过实际 socket close/munmap，普通关闭和打开失败回滚都留有资源。
- [内核 netns 补丁](../src/channels/netadapter/pfring-8.8.0-netns.patch)：`sk_alloc` 使用调用方的 `kern` 参数，
  使用户态 socket 保留网络空间引用；进程退出的延迟文件释放完成后才回收 netns，避免非空 proc 目录 WARNING。

先在对应 PF_RING 源码根目录应用补丁，再按该部署的构建方式重新构建用户态库及匹配运行内核的模块：

```bash
git apply /path/to/flowSQL/src/channels/netadapter/pfring-8.8.0-close.patch
git apply /path/to/flowSQL/src/channels/netadapter/pfring-8.8.0-netns.patch
```

模块替换前核对 `vermagic`、符号 CRC 与活动 ring/引用，保留原模块；不强制加载或卸载。
当前 WSL2 6.6.87.2 的修复模块完成 140 项导入 CRC 核对，用户态库仍为 8.8.0，无 Redis/ZC/FT/AF_XDP 扩展。
部署其他版本前需重新核查源码，不自动给未知版本应用补丁，也不用 `PF_RING_RUNTIME_MANAGER` 环境变量绕过 close。
provider 明确使用 Classic 短包头，预算预检包含 slot 对齐、FlowSlotInfo、页面/SHMLBA 和 scratch，
打开后仍以实际映射容量二次核对全通道预算。

资源回收回归使用独立命名空间，每次创建两对 veth，正常和不执行用户态 close 的进程退出各运行三次：

```bash
bash src/tests/test_netadapter/run_isolated_pfring_smoke.sh \
  build/output/test_pfring_backend build/netadapter-validation/pfring-lifecycle
```

正常路径逐阶段核对同 netns 的 socket proc 节点、混杂状态、重开、1500 字节 snaplen/16 MiB 打开、
第二接口缺失与非 Ethernet 回滚、预算失败诊断及 provider 租约。异常退出通过 exec 移除保留 netns 的 shell，
等待内核延迟回收后核对 ring=0 和测试前后 dmesg 增量无 WARNING/Oops。历史内核警告不清除，也不混入本轮结果。

## AF_XDP 逐 RX 队列验收

```bash
bash src/tests/test_netadapter/run_isolated_backend_smoke.sh build/output/test_af_xdp_backend
```

脚本创建独立网络和挂载命名空间，临时 sysfs 供后端发现实际队列，并通过临时 TUN 字符设备节点
创建两张非持久的 TAP Ethernet。每张 TAP 的两个注入 fd 对应驱动记录的 RX0/RX1；标记帧经
真实 TAP RX、generic XDP、XSK COPY 和 UMEM 进入后端，测试遍历全部四个 source，逐包核对接口、
队列、轮次、48 字节截断、64 字节 wire length 与时间戳。

每队列核对 600 个已知包，超过该配置的 256 帧 UMEM 两轮，验证 FILL 归还和重复使用。
同时检查实际 COPY 模式、全队列预算、borrow 互斥、逐队列 drop/received unavailable、Cancel、
关闭后同队列重开、混杂模式引用计数恢复、已有 XDP owner 拒绝和替换 owner 保留。替换 owner 测试
可能输出 `Active program does not match expected`；这是 compare-and-detach 拒绝卸载替换程序的预期诊断，
随后断言替换程序仍存在并由测试所有者回收。退出时最后一个 TAP fd 关闭，临时节点和命名空间归还。

当前 WSL2 6.6.87.2 的 veth 转发路径不记录 skb RX queue；实测 TX queue_mapping=1 的规则已命中，
generic XDP 仍从 RX0 接收。因此双 veth 用于通道/生产 SQL 联验，完整逐队列收包由上述双 TAP 测试证明。
这两类证据分别对应真实 Linux 虚拟 Ethernet 输入与通道接线，物理网卡性能仍按 T5 的部署工况验收。

## 通道与生产 SQL

通过既有通道管理入口创建 `type=netadapter`、`role=source`、具名通道，options 为：

```json
{
  "backend": "af_packet",
  "interfaces": ["eth1", "eth2"],
  "promiscuous": true,
  "snaplen": 65535,
  "buffer_mib": 64
}
```

仅 backend/interfaces 必填。buffer_mib 是全通道预算，包含全部输入 ring/UMEM、Arrow 列容量及复制峰值；
释放 block 后仍被共享 owner 保留的采集内存继续计费。分析及结果存储有各自预算。
查询可查看 generation、观测域、网卡/队列映射、生效限额、内存峰值及逐输入计数；不可用计数为 null。
有 reader 或遗留 owner 租约时，修改、删除或卸载返回 busy。

持续 NPM 任务通过后台 batch submit 入口启动，并使用已创建的托管数据库目标：

```sql
SELECT * FROM netadapter.edge_mirror
WHERE wire_len >= 64 AND (transport_protocol = 6 OR transport_protocol = 17)
USING npm.basic WITH output_interval_ns=15000000000 INTO sqlite.npm
```

无需填写 source_domains，Scheduler 从独占 reader 的输入集合自动绑定。窗口按 UTC epoch 对齐；
任一输入积压、进度未知或时钟回拨时，共同安全水位暂停。维护定时器只唤醒工作，不代替采集事件时间。
后台状态提供 runtime_task_id/run_id 和结果关系；Stop 排空正常尾部，Cancel 保留已交付前缀并异常结束。

上例限定 TCP/UDP。采集 ICMP/ICMPv6 时需显式启用相应控制事件模块，无过滤示例为：

```sql
SELECT * FROM netadapter.edge_mirror USING npm.basic
WITH output_interval_ns=15000000000,features='basic,icmp' INTO sqlite.npm
```

Basic 与 ICMP 结果仍写入同一个托管数据库目标。IPv6 版本从线格式首字节高四位读取，
合法 Router Solicitation 即使没有 transport layer 也能作为无端口控制事件进入已启用模块；
它不创建 TCP/UDP 会话。默认仅启用 basic 时拒绝未启用模块的控制输入，原契约保持。

## 隔离真实收包记录

记录工具加载公共生产插件，通过 channel manager、Scheduler submit、npm.basic 和 SQLite history 联验。
脚本在新的网络命名空间创建两对 Ethernet veth，各接口有两个 RX 队列；退出后内核回收临时接口。
需要创建网络命名空间、raw socket 及对应后端资源的权限，不能在缺权限时自动 skip。

```bash
bash src/tests/test_netadapter/run_isolated_validation.sh \
  build/output/netadapter_real_validation \
  sql af_packet 15 47 100 64 stop \
  build/netadapter-validation/af-packet-sql-15s-stop.json
```

参数依次为 executable、capture/sql/sql_all、backend、周期秒数、持续秒数、发送 tick/s、帧字节数、
stop/cancel、JSON 路径，以及可选 profile（默认 dual）。dual 每 tick 从两侧各发一个 TCP 和 UDP 包，
共 4 包；同域双向流量应关联为两个长期会话。capture 还接受 single（只打开/发送第一接口，每 tick 2 包）、
busy_idle（第一方向每 tick 发送，第二方向每 100 tick 发送）、reject（两接口发包，shared reader 精确下推
literal false，全部过滤但仍核查安全进度）。sql/sql_all接受dual；sql_probe接受其他验证profile。
帧长为包含 Ethernet 头的 64 或 1500 字节，不包含 FCS/preamble/IFG。持续时间至少为 `3*period_s+2`。
记录工具显式使用 `snaplen=1500`、全通道 `buffer_mib=16`，JSON 保存该配置；这不是生产通道的默认值。
sql 模式使用上例 TCP/UDP 过滤与 1 秒乱序容忍；sql_all 不使用 WHERE，显式启用 basic,icmp，
保留 1 秒乱序容忍。capture 模式及时归还输入，不运行 NPM 分析。
PF_RING 模式在生产插件退出后额外核对同一 netns 的 ring/proc 节点和两接口混杂状态，保存原始接口 JSON 与 PF_RING info。

判据在启动前固定：窗口首次可见延迟不超过 2000 ms，capture 冷输入服务间隔不超过 100 ms、
共同候选滞后不超过 2000 ms、采集受控容量不超过 16 MiB，reader 丢包与可用的源端丢包计数均为 0。
SQL 在停止前需至少六条完整周期记录；Stop 两条最终记录，Cancel 无最终记录。失败返回非零。

JSON 保存命令、内核/CPU、选定负载、实际发送数和错误数、限额、每 200 ms 的 reader 诊断样本、CPU 时间、
RSS、采集容量峰值、batch 包数/捕获字节分布、共同候选推进次数与不可用 Poll 次数、
Poll 或 status 请求延迟，以及首次可见窗口延迟的 p50/p99/max。分析预算占用与实际维护延迟目前为 null，
仍待专门观测；SQL status 请求延迟不等于维护延迟；
capture 的共同候选滞后不等于完整 SQL 的分析水位。sql_probe已记录真实维护迟到、分析预算与共同候选，
上述null仅描述旧sql/sql_all记录；瞬时积压暂停时测量最近已确认候选的年龄，不能据此推进生产事件时间。
邻居发现等额外报文可能使 capture delivered 大于 sender sent。
JSON 另列受控标记报文的逐输入数量；打开全部 XSK 不等于已对每条 RX 队列实发受控报文。

矩阵入口串行运行真实输入，避免并行工况相互争用资源：

```bash
python3 src/tests/test_netadapter/run_validation_matrix.py \
  --executable build/output/netadapter_real_validation \
  --output build/netadapter-validation/t5 --group all
```

capture 组为三后端 ×64/1500 字节 ×100/500/1000 tick/s ×single/dual，另加各后端/帧长的
busy_idle/reject，共 48 项，每项 5 秒。sql 组为 AF_PACKET/PF_RING 的 15 秒周期，以及
AF_XDP 的 15/30/60 秒周期，共 5 项；每项至少三个周期加 2 秒，交替 Stop/Cancel。
可用 `--group capture` 或 `--group sql` 分别复现。默认失败即停，`--keep-going` 保存失败后继续扫描，
最终仍返回非零；`--resume` 保留相同输出目录和 group 的已尝试案例，不覆盖原始通过或失败证据。
输出包含每项 JSON/log、环境库版本和 summary.json。失败阶梯不属于可持续范围，零丢包但共同候选
停滞也判失败，不提高已冻结的延迟阈值。

2026-10-04 在 WSL2 6.6.87.2、i9-13900H、隔离 veth 环境，48 项采集工况实际执行结果如下。
每项仅持续 5 秒，表中的通过率描述本轮检查点，不代表物理网卡吞吐或长期稳定性承诺。

| 后端 | 通过/执行 | 采集容量峰值（字节） | 最大输入服务间隔（ms） |
| --- | --- | --- | --- |
| AF_PACKET | 12/16 | 8,482,112 | 13.609 |
| PF_RING Classic | 16/16 | 12,846,712 | 24.033 |
| AF_XDP SKB/COPY | 16/16 | 4,308,032 | 16.065 |

AF_PACKET 的四项失败：dual/64 字节/500 tick/s、dual/1500 字节/1000 tick/s，
以及 reject/64 和 1500 字节/1000 tick/s。均为共同候选推进次数=0，而已采样源端/reader 丢包为 0；
冷输入服务与容量达标仍不能代替安全进度活性。完整失败记录保存在上述输出目录，
不据有限离散采样推断连续的可持续速率范围，也不调整 backlog 门禁绕过失败。

同轮无过滤生产 SQL 的五项均通过，负载为双接口 100 tick/s（400 个受控包/s），
每项停止前均有 6 条完整周期记录、2 个长期 TCP/UDP 会话。窗口按采集事件时间封闭，
下表测量从窗口边界到数据库记录首次可见的延迟，包含 1 秒乱序容忍与查询采样延迟。

| 后端 | 周期/运行（秒） | 帧字节 | 终止方式/最终记录 | 窗口可见延迟最大值（ms） |
| --- | --- | --- | --- | --- |
| AF_PACKET | 15/47 | 64 | Stop/2 | 1065.430 |
| PF_RING Classic | 15/47 | 1500 | Cancel/0 | 1074.540 |
| AF_XDP SKB/COPY | 15/47 | 64 | Stop/2 | 1066.290 |
| AF_XDP SKB/COPY | 30/92 | 1500 | Cancel/0 | 1059.480 |
| AF_XDP SKB/COPY | 60/182 | 64 | Stop/2 | 1060.424 |

五项采集容量与可用丢包判据均通过。整组 53 项、49 项通过/4 项失败，矩阵最终退出码=1，
summary.json 的 passed=false 保留失败事实。完整 SQL 的阶梯/忙闲/高拒绝与过载尚未由上述低负载案例证明。

这些记录是 T5 的阶段检查点。慢分析、慢输出、有界过载恢复、实际维护延迟/分析预算、
完整三后端生产 SQL 持续矩阵与任务观测域隔离仍按规格逐项验收，不能据此勾选整个 Feature。

## 生产运行观测与受控慢消费

managed_result.diagnostics 现报告真实分析预算当前值/高水位、tracked/pending限额、
共同安全候选和阻塞输入。tracked_peak_bytes来自同一预算锁内实际同时占用的最大值，
不由各分类峰值相加；input_batch_peak_bytes保留处理后立即释放的瞬时输入预留。
runner在执行线程每200ms发布状态，状态路由只读取快照。结果关系和schema_version=1保持。

测试专用libnetadapter_validation_probe.so通过现有C ABI获取生产算子的纯虚接口，保留库到任务释放后。
sql_probe路径仍经Scheduler和真实SQLite，额外记录维护deadline/pending到OnTime的单调迟到分布，
以及维护调用间隔；两者与HTTP status耗时分别报告。slow_analysis在5～35秒每个ProcessBlock延迟20ms；
slow_output在前35秒每次实际prepared写入前延迟10ms。overload_recovery在5～10秒升至5000 tick/s、
每批延迟50ms，之后恢复100 tick/s；不将测试注入参数加入生产配置。

```bash
python3 src/tests/test_netadapter/run_validation_matrix.py \
  --executable build/output/netadapter_real_validation \
  --output build/netadapter-validation/t5-recovery-next --group recovery --keep-going
```

2026-10-04九项压力联验中，三后端slow_analysis/slow_output共六项均通过，
PF_RING/AF_XDP过载恢复通过；AF_PACKET首次过载记录保留失败，随后独立复测通过。
每项15秒周期/47秒、至少6条完整记录、2会话，Stop2最终/Cancel0最终。

| 后端 | 慢分析/慢输出最大维护迟到（ms） | 已通过过载记录的首个恢复进度（秒） |
| --- | --- | --- |
| AF_PACKET | 3.764 / 6.726 | 0.276 |
| PF_RING Classic | 3.791 / 4.876 | 1.564 |
| AF_XDP SKB/COPY | 4.116 / 3.993 | 0.171 |

上述压力案例tracked峰值不超过22392字节、pending峰值不超过15552字节，
低于本轮冻结16MiB/4MiB限额；压力真实触发安全候选不可用，恢复后进度重新推进。
压力及恢复期间的受影响窗口允许延期，全部真实延迟保留。影响归类按窗口原2000ms可见期限是否与
5～15秒区间相交，覆盖升载前结束但仍等待安全水位的窗口；持续阶段2000ms判据保持。
AF_PACKET最终复测逐窗口原始时间见t5-af-packet-overload-final.json；首次失败未覆盖。

功能矩阵新增single/busy_idle/reject/domain_isolation/input_failure入口（--group functional），
本轮最终实际前缀只有AF_PACKET single/busy_idle两项通过。reject被SQL谓词语法拒绝，
domain_isolation未得到第二任务run_id，input_failure未实际触发EIO，均记录失败且尚待修复测试入口，
不能据这三个入口声称功能验收完成。余下两后端功能案例因时间盒停止仍待验。
STOP文件可让未来矩阵在案例边界退出；输出原始JSON/log和summary，不据中断前缀声称整组完成。

2026-10-05（北京时间）功能联验更新：`t5-functional-sequential/summary.json`实际15/15通过，
三后端各覆盖single/busy_idle/reject/domain_isolation/input_failure，1秒周期、每任务5秒。
全拒绝入口改用现有精确谓词`WHERE wire_len < 0`，0会话/0最终记录且安全候选继续前进。
故障probe在真实provider后注册，实际日志确认第二输入EIO；整任务failed、0最终记录，
reader租约归还、分析tracked/pending占用归零；PF_RING关闭后同netns ring/proc=0、混杂引用=0。

观测域隔离按两个具名通道顺序运行，相同受控双向TCP/UDP帧复用一个managed SQLite目标；
JSON记录`domain_execution=sequential`、`npi_concurrency=1`、两个不同run_id和各自实际发送计数。
三个后端均为两个不同观测域、每run单一域/两会话/两条Stop最终记录，第二任务stopped。
本证据不覆盖两个任务并发占用相同接口或并发写入同一目标。
旧并发失败原始记录保留在`t5-domain-npi-two/`：两个NPI槽位解决runtime创建失败，
AF_PACKET/PF_RING后续任务失败或无结果仍未查明根因；AF_XDP同接口第二reader返回EBUSY。
全拒绝功能验收不替代每100包放行1包的持续高拒绝工况。
AF_PACKET/PF_RING30/60秒、完整持续SQL矩阵与Feature全量回归仍待完成，T5保持未完成。

## 持续SQL与周期结果验收入口

`--group periods`补齐AF_PACKET/PF_RING各30/60秒周期，分别92/182秒，100tick/s双接口，Cancel/Stop。
`--group sustained`串行执行三后端×64/1500字节×single/dual/busy_idle/high_reject，共24项；
每项15秒周期、47秒、100tick/s，single/high_reject使用Cancel，dual/busy_idle使用Stop。
使用sql_probe观测完整生产链路，所有工况使用同一netadapter.live源名称和单一managed SQLite目标。
high_reject每100tick放行一组双向TCP/UDP，其他tick的src_port=41002；
SQL `WHERE src_port < 41002`精确筛选约1%，帧长度始终64/1500不变，实际selected/rejected发送计数写入JSON。
它与全拒绝reject分开，仍要求两长期会话和每会话至少三个完整周期。

结果JSON的result_consistency逐会话核对revision连续递增、UTC周期网格/长度、
每方向interval的已交付前缀和等于累计值、方向字节和等于total，并记录完整周期数和Stop部分尾部。
single只要求一个方向有包，其他工况要求两个方向均有包；Cancel核对已有前缀，无最终记录。
持续判据保持候选/窗口≤2000ms、逐输入服务/维护≤100ms，采集16MiB、tracked16MiB/pending4MiB、
可用source与reader丢包=0。历史高负载停滞案例保留，不属于本轮声明的可持续负载。

```bash
python3 src/tests/test_netadapter/run_validation_matrix.py \
  --executable build/output/netadapter_real_validation \
  --output build/netadapter-validation/t5-periods-25 --group periods --ticks-per-s 25 --keep-going
python3 src/tests/test_netadapter/run_validation_matrix.py \
  --executable build/output/netadapter_real_validation \
  --output build/netadapter-validation/t5-sustained-25 --group sustained --ticks-per-s 25 --keep-going
```

2026-10-05周期联验：加强安全候选观测后，AF_PACKET100tick/s的30/60秒虽结果一致、可用丢包0，
最近已确认安全候选最大滞后仍达5029.983/5495.104ms，均失败；证据`t5-periods-complete/`保留。
该负载不能因完整周期已有输出而声明满足持续进度判据。
重新在启动前固定25tick/s（双向100受控pps）及原阈值，`t5-periods-25/summary.json`实际4/4通过：

| 后端 | 周期/运行（秒） | 候选最大滞后（ms） | 窗口最大可见延迟（ms） | 最大维护迟到（ms） |
| --- | --- | --- | --- | --- |
| AF_PACKET | 30/92 | 586.225 | 1074.914 | 10.784 |
| AF_PACKET | 60/182 | 579.413 | 1081.371 | 18.100 |
| PF_RING Classic | 30/92 | 264.706 | 1072.148 | 9.087 |
| PF_RING Classic | 60/182 | 430.476 | 1050.195 | 13.024 |

四项每会话3完整周期、result_consistency.failures=0，Stop各2部分尾部最终，Cancel0最终；
资源/丢包判据通过，相关CTest7/7（22.45秒）。AF_XDP旧15/30/60原数据库只读一致性复核3/3通过，
原始审计`t5-af-xdp-period-consistency-audit.json`保留。更低负载由`--ticks-per-s 25`显式指定，
命令与JSON完整保存；缺省100保留旧复现，不事后改写失败数据。持续矩阵同样在启动前固定25tick/s。

## T5完成证据与已验证运行范围

2026-10-05三后端持续SQL矩阵24/24实际通过，`t5-sustained-25/summary.json`退出0、passed=true。
每项47秒跨三个15秒周期，两帧长与single/dual/busy_idle/high_reject全覆盖；
各会话revision/UTC网格/interval前缀与累计/双向关联断言0失败。Stop各2条最终记录，Cancel0最终记录。
high_reject实际拒绝率约98.98%，无拒绝端口进入结果、无frame_bytes×packet字节偏差，已交付前缀≤实际放行包。

| 后端 | 持续通过项 | 候选最大滞后（ms） | 窗口最大可见延迟（ms） | 最大输入服务间隔（ms） | 最大维护迟到（ms） | 采集容量峰值（字节） |
| --- | --- | --- | --- | --- | --- | --- |
| AF_PACKET | 8/8 | 603.013 | 1078.569 | 29.182 | 7.103 | 8,412,608 |
| PF_RING Classic | 8/8 | 257.044 | 1072.629 | 32.503 | 4.687 | 12,757,752 |
| AF_XDP SKB/COPY | 8/8 | 247.579 | 1067.526 | 24.728 | 4.153 | 4,276,032 |

持续阶段tracked峰值≤9566字节、pending≤15552字节，采集/分析冻结预算及可用source/reader丢包0判据全通过。
完整DoD：`cmake --build build -j8`通过且无Error/Warning；完整CTest沙箱外48/48（66.83秒），
全部42个Feature改动C++格式、shell/Python语法和diff检查通过。首轮沙箱完整CTest44/48的
socket/数据库权限失败日志保留在`t5-final-full-ctest.log`，获准升级后完整复验日志`t5-final-full-ctest-unrestricted.log`。

最终证据清单`build/netadapter-validation/t5-completion-evidence.json`核查原始文件、选定矩阵数量、
通过状态和工具/二进制SHA256；完整构建/格式/CTest日志与各项JSON/log保留在同目录。
T5与NetAdapter Feature已完成，规格归档于`tasks/archive/feat-npm-linux-capture-backends.md`。
已验证持续负载为本WSL2隔离Ethernet环境25tick/s：single约50受控pps，dual/high_reject约100受控pps，
busy_idle约50.5受控pps；这是指定持续时间与离散工况的实测范围，物理NIC更高速能力需目标环境重测。
AF_XDP四RX队列逐队列实发/回收引用T4双TAP证据，veth不代替四队列实发。
观测域隔离按顺序任务验证，不承诺同接口/同目标并发；旧并发失败原始记录保留。
AF_PACKET高负载停滞及压力阶段真实丢包同样保留，不将它们计入零丢包可持续能力，不放宽安全进度门禁。
