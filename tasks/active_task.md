# 即时工作台

WIP=0：Codex CLI 误判已修复，进程识别回归与静态检查通过；实际目录迁移和新路径验收仍待用户手动执行，整体维护任务未完成。

## 意图与边界

- 用户授权：项目目录 flowSQL 调整为 flowsql；涉及路径全部修改；2026-10-26 明确历史材料旧目录路径不处理、保留原状。
- 当前 Atomic Slice：交付小写项目根目录及可继续使用的构建、启动、调试路径，更新当前有效说明。
- 业务意图：开发与部署从 `/mnt/d/working/flowsql` 使用项目，避免目录大小写和路径约定不一致。
- Non-Goals：不改主 Web、探索前端、C++/Python 展示，不改 CMake 目标名、凭证、协议、ABI 或业务逻辑；不改历史材料、历史日志、evidence 既有记录及 Git 历史；不提交/推送，不停止或重启用户服务。
- 时间盒：30 分钟；2026-10-26 06:51 UTC 开始；到期记录检查点，未完成时沿用本维护任务。
- 停止条件：根目录项为 flowsql；当前有效路径无旧根名；新路径配置/相关主产物构建/冻结测试通过；历史与展示文件摘要不变；WIP=0。

## 允许修改文件与操作

- `tasks/active_task.md`。
- 当前规则/用户说明：`AGENTS.md`、`CLAUDE.md`、`README.md`、`docs/framework.md`、`docs/kafka.md`、`docs/netadapter.md`；仅目录名称和路径字面量。
- 本地忽略配置：`.vscode/settings.json`、`.vscode/launch.json`；仅项目根路径。
- 本地构建与依赖元数据：`build/`、`.thirdparts_prefix/`、`.thirdparts_installed/` 内含旧根路径的生成配置/脚本/链接元数据；只迁移精确根路径，不改依赖源码和二进制，不删除目录。迁移清单记录在 `evidence/project-path-lowercase/`。
- 如发现当前运行配置内有旧根路径，先列明文件与影响并更新工作台；不直接修改数据库或持久业务文件。
- 根目录改名：从父目录经唯一临时目录项将 `flowSQL` 改为 `flowsql`；不删除目录；之后所有命令显式使用新目录。
- 本地忽略证据：`evidence/project-path-lowercase/`；不入库。
- 初始工作区干净、索引为空；HEAD 与全部跟踪文件摘要在 before.json。每次 patch 后核对 git diff --name-only。

## 冻结契约与验收

- 只调整项目根路径段，保留 FlowSQL/flowSQL 展示、凭证、内部标识及历史材料字节；既有生成二进制的调试历史路径不做二进制替换，新构建采用新路径。
- 目录迁移不重置 `.thirdparts_installed` / `.thirdparts_prefix`，不迁移至另一份 worktree、不改远端。
- 配置：`cmake -B /mnt/d/working/flowsql/build /mnt/d/working/flowsql/src`；显式使用小写绝对路径，实际目录项迁移另行核验。
- 构建：`cmake --build build --target flowsql flowsql_catalog flowsql_scheduler flowsql_web flowsql_visualization test_native_deploy_config -j2`；覆盖默认启动主产物和路径敏感配置，复用既有依赖缓存。
- 验收：`tools/test.sh all --location native --filter '^(test_native_deploy_config|test_product_version_runtime|test_visualization_dataframe_native)$' --jobs 1`。
- 选择依据：原生配置/启动脚本/产物、C++/Python CLI 与 HTTP 版本入口、含 Scheduler 的主产物 DataFrame 恢复/SQL/快照路径。界面和业务源码未修改，不增加无关 DB/Kafka/Docker/全量。
- 依赖：新根目录的主 build/output、本地 Python、隔离 SQLite/CSV/随机 loopback 端口与现有 services=[] fixture；统一 environment.json/dependencies.json，不需外部服务或凭证。
- 必需 HTTP 测试若沙箱禁止监听，保留完整清单并记录未执行；当前环境自动审批拒绝外层执行，不减少断言或跳过。
- 核对 selected=executed=passed，failed=skipped=not_executed=0；相关源码、历史材料、展示文件保持不变；检查目录项、缓存/编译数据库、IDE 路径、主产物路径及 git diff --check。

## 当前状态

- 已完成只读影响分析；用户确认历史材料完全保留，展示保持原样。
- 沙箱隔离了进程视图，沙箱内未见服务不能证明宿主服务已停止；两次宿主只读核验申请被自动审批拒绝（策略禁止 require_escalated），不据此移动目录或停止/恢复服务。
- 根目录改名需要父目录写权限；当前仅仓库与 /tmp 可写。先完成仓库内修改、生成元数据及可运行验证，真实目录项迁移保留为环境阻塞。
- 已建立 4078 个生成元数据文件、119111 处根路径替换的精确清单。小写路径配置遇 EROFS 后，已按原 SHA256 恢复全部生成文件，保持现有构建可用；清单在 metadata-migration.json，恢复结果在 metadata-restoration.json。
- 小写路径配置失败：沙箱仅授权旧大小写路径，新路径写 build-info.json/CMakeCache.txt 返回只读文件系统；日志 configure.log。
- 本地 loopback bind 返回 EPERM，必需 HTTP 验收无法在当前沙箱执行；宿主核验提权申请被自动审批拒绝，不重复申请或绕过限制。
- 保持完整验收门槛；环境阻塞期间仅执行现有路径的原生配置及无监听 CLI 检查点，不将其等同于新目录验收。

## 环境受限检查点

- 必需的目录迁移、新路径配置构建及 HTTP 验收仍未完成，不标记整个维护任务完成。
- 临时检查：`cmake --build build --target test_native_deploy_config -j2` 与 `tools/test.sh unit --location native --filter '^test_native_deploy_config$' --jobs 1`，仅证明现有路径的部署契约未变。
- C++ 与 Python `--version` 无监听检查，必须保留原 FlowSQL 展示；不改用户服务。

- 本地权限实测：父目录 `/mnt/d/working` 不可写，旧根可写，小写根不可写；未执行目录移动。

## 本轮最终证据与继续条件

- 当前有效路径已更新：6 个跟踪规则/说明文件和 2 个忽略的 IDE 配置；仅修改路径，未改凭证、展示或内部标识。
- 162 个历史材料文件（archive/sprints/docs archive）及全部 1002 个 src 文件 SHA256 保持不变；初始工作区无其他修改。
- 本地生成元数据迁移试验已全部按原 SHA256 恢复；保留依赖缓存，不删除目录。现有路径重新配置/构建 test_native_deploy_config 成功。
- 首轮检查点测试与构建重叠被 runner 判为 artifact_changed，未计通过；构建结束后复测通过，最后以显式 native 位置再次核对。
- 最终原生单测：evidence/test-environment/20261026T070824Z-b60e5d58/summary.json，selected=executed=passed=1，failed=skipped=not_executed=0。
- C++ flowsql、flowsql-worker、python3 -S -m flowsql.worker 三入口版本输出检查通过，保留 FlowSQL 品牌；cli-checkpoint.json。
- 尚未执行根目录改名、新根路径配置构建、test_product_version_runtime 和 test_visualization_dataframe_native；完整迁移验收未通过，不标记完成。
- 环境需要允许父目录写入以改名、允许 /mnt/d/working/flowsql 写入，以及本地 HTTP/loopback 验收。当前自动审批拒绝 require_escalated，未绕过。
- 当前实际目录项仍为原大小写；不停止/重启用户服务，不提交/推送。后续沿用本维护任务并先核验宿主运行状态，再完成冻结迁移与验收。

## 继续执行切片

- 用户再次授权：继续执行根目录改名及既定路径迁移，范围沿用原冻结清单；不新增业务改动、不提交/推送。
- 时间盒：30 分钟；2026-10-26 07:12 UTC 开始。
- 先申请父目录/新根路径执行权限，核验宿主是否存在服务，再经临时名称完成大小写改名；成功后迁移当前生成元数据，并在新路径执行原冻结配置、构建和验收。
- 当前无权限修改父目录、新根路径或监听端口；未取得执行权限前不再迁移生成元数据，避免重复上轮回退。

## 继续执行结果

- 用户“继续执行”后重新核验：父目录与小写根路径依旧不可写，loopback bind 依旧 EPERM。
- 为已明确授权的目录改名提出精确执行申请（先检查宿主服务、无服务才临时名中转，失败回滚），自动审批在创建进程前拒绝；实际改名脚本未执行。
- 审批明确认可用户授权足够，拒绝原因是 require_escalated 与父目录写入不符合当前环境硬限制；不能用替代路径绕过。
- 本轮未重新迁移生成元数据、未重复构建/测试；保留前轮现有路径检查点，未将其计为新路径验收。
- 等待执行环境允许 /mnt/d/working 的目录改名、/mnt/d/working/flowsql 写入及本地监听后继续；无需再次确认业务范围。

## 手动执行脚本准备切片

- 用户要求：提供所有需要执行的命令，由用户在普通 WSL 终端手动执行；不再申请提权或尝试目录改名。
- 关联任务：沿用本项目目录小写维护任务，不新增 Feature Task。
- 当前 Atomic Slice / 业务意图：交付可审查、可重入的迁移脚本，让用户完成剩余目录改名、生成路径迁移和新路径验收。
- Non-Goals：沿用原范围；不执行脚本的 apply 模式，不停止用户服务，不删除构建/依赖缓存，不修改历史、源码、凭证或展示，不提交/推送。
- 允许修改：仅 `tasks/active_task.md` 与忽略目录 `evidence/project-path-lowercase/` 内新增手工脚本、说明及静态验证结果；保留先前六个规则/说明文件的路径修改及两个 IDE 修改。
- 脚本契约：检查宿主服务并在有服务时停止操作；经唯一临时名改目录；修改前保存元数据原字节；精确迁移旧根路径；顺序配置、构建、测试；核对真实目录项、选中/执行/通过集合与历史/源码摘要；错误非零退出并保留日志，不自动标记维护完成。
- 用户手动构建清单：在原清单基础上补 `flowsql_builtin flowsql_database flowsql_router`，它们由已冻结的 DataFrame 与版本 HTTP 验收实际加载；不扩大测试边界。
- 手工元数据清单：在原 4078 条清单上补入后续检查点构建新生成的 8 个 `compiler_depend.make/internal`；另存 `manual-generated-paths.json`，不覆盖既有历史证据。
- 用户手动验收仍为原三项测试及 CTest 必需 fixture；从 native 位置执行，使用现有统一测试配置，无外部 DB/Kafka/Docker 依赖。
- 本切片验收：Python AST/`--help`、只读计划检查、`git diff --check` 与范围核对；不把静态检查计为真实迁移验收。
- 时间盒：30 分钟，2026-10-26 07:19 UTC 起；停止条件：脚本及完整命令交付、静态检查通过，工作台记录 WIP=0/等待用户执行。

## 手动执行脚本交付结果

- 本切片已完成：`evidence/project-path-lowercase/manual-migrate.sh` / `manual-migrate.py` / `MANUAL.md`，均为本地忽略文件，不入库。
- Python AST、Shell 语法、`--help` 与无参数计划模式通过；没有调用 `--apply`，没有目录移动、生成元数据替换或新路径测试。
- 只读核对 1248 个跟踪文件：除原冻结规则/说明与工作台以外均匹配初始摘要；历史材料、全部 src 文件保持不变。
- 手工清单共 4086 条，其中 4078 个文件当前存在，8 个已由 CMake 移除的临时依赖文件记录为不存在，不作为测试跳过。
- 当前生成元数据补扫发现的 8 个新增依赖文件全部已纳入手工清单；备份和精确路径替换只在用户执行 apply 时发生。
- 复用标准 runner 只读选择，冻结三项业务测试与 `test_visualization_dataframe_native__environment`；此处只核对清单，不计为执行或通过。
- 静态结果：`manual-static-validation.json`；当前补扫：`manual-metadata-scan.json`。整体迁移必须等待用户执行后的 `manual-*/result.json` 与标准测试 summary 才能收口。
- 交付命令从 `/mnt/d/working` 执行；用户先保存编辑内容并停止项目服务。成功后从 `/mnt/d/working/flowsql` 重新打开工作区；没有提交/推送、没有启动常驻服务。

## Codex 进程误判修复切片

- 事实依据：用户宿主输出确认 PID 98583 为 `/root/.nvm/versions/node/v25.6.1/bin/node`，工作目录是旧根，入口为同前缀的 `bin/codex`。原检测将所有项目工作目录内的 Node 进程列为服务，导致执行在任何迁移之前停止。
- 关联任务 / 业务意图：沿用项目目录小写维护；让用户在保持 Codex CLI 运行时完成手动迁移，继续阻止真实项目服务和构建并发。
- 当前 Atomic Slice：仅修复已确认的 Codex CLI 误判，复验进程检查和静态入口。
- Non-Goals：不取消进程检查、不按 PID 硬编码放行、不忽略全部 Node、不停止服务、不运行 apply、不改源码/历史/构建元数据、不提交/推送。
- 允许文件：`tasks/active_task.md`、`evidence/project-path-lowercase/manual-migrate.py`、`evidence/project-path-lowercase/MANUAL.md`，以及新增 `manual-process-validation.json` 本地证据；其余既有 diff 保持原样。
- 最小方案与风险：只放行通过 Node 启动、入口名为 codex 且真实入口位于项目根之外的 CLI；项目内同名入口继续拦截。风险边界是入口识别，使用用户复现与正反场景验证，其他进程规则不变。
- 冻结验收：以模拟 `/proc` 记录复现用户场景，并验证项目 Node/Vite、npm、CMake、C++ 服务、Python Worker 仍拦截，以及无关 Node/项目内同名 codex 边界；Python AST/`--help`、Shell 语法与默认计划、`git diff --check`/范围核对。不需要 CMake 或外部服务，不运行真实迁移。
- 时间盒：20 分钟，2026-10-26 07:35 UTC 起；停止条件：上述回归与静态检查通过，WIP=0，交付相同手工重跑命令。

## Codex 进程误判修复结果

- 先以模拟 `/proc` 复现用户报告：原实现误拦截普通 Codex CLI 和带 Node 参数的 Codex CLI；其余 9 个正反场景符合预期。
- 修复仅位于 Node 入口识别：入口名必须为 codex、绝对入口路径与真实文件均在项目根之外；其他 Node、项目内同名入口、实际构建与服务进程规则不变，不按 PID 豁免。
- 修复后 11 个进程识别回归场景全部通过，包括用户复现、Node 参数、项目 Vite/npm/CMake/FlowSQL/Python Worker、无关 Node、项目内 codex 和把 codex 当普通参数的边界。
- Python AST、`--help`、Shell 语法和默认计划模式通过；`git diff --check` 与允许范围检查通过，索引保持为空。
- 证据：`evidence/project-path-lowercase/manual-process-validation.json` 保留修复前后结果；这是进程识别回归，不是整体迁移验收。
- 未执行 apply、未移动目录、未修改生成元数据、未结束任何进程、未提交/推送；用户可以保持本 Codex CLI 运行，重跑原迁移脚本。
