# Active Task

Feature：README SQL 过滤能力文档
原子任务：D1 补充阶段化 WHERE 与 pcapfile 过滤示例
状态：已完成

## 业务意图

- 在 `README.md` 中说明当前已经实现的 SQL 阶段化过滤能力，使用户能判断 `WHERE` 绑定到哪个阶段。
- 列出通用谓词语法、SQL NULL 语义、pcapfile 的时间/地址/端口/双向 TCP/UDP 过滤，并提供可复制示例。

## Non-Goals

- 不修改 SQL parser、Scheduler、pcapfile、公共接口或任何运行时代码。
- 不新增语法、字段、函数或 API，不将未来 `npm.basic` 能力描述为已经可用。
- 不修改已归档规格和 product backlog，不进入后续 Feature。
- 不读取或修改 `tasks/sprints/**`，不执行 commit/push。

## 允许修改的文件

- `tasks/active_task.md`
- `README.md`

现有 `npm-offline-filter` 累计代码、测试、归档和 backlog diff 只保留，不修改。

## 验收标准与命令

- README 明确 source-stage `WHERE`、operator-stage `WHERE` 及其 stage 绑定规则。
- README 列出当前通用运算符、字面量、三值逻辑和明确不支持的写法。
- README 说明 pcapfile 可过滤的 header/decoded 字段，以及 `TIMESTAMP`、`mac`、`ip`、`port`、`tcp`、
  `udp` 的参数和方向语义。
- README 至少提供通用字段、时间窗口、方向中立地址/端口和 TCP/UDP endpoint pair 示例。
- 示例仅使用当前实现和测试已经验证的 SQL 形式，不承诺协议识别、payload/BPF/正则或 TCP stream 语义。
- `git diff --check -- README.md tasks/active_task.md`
- `rg -n 'SQL 过滤能力|WHERE|TIMESTAMP|mac\(|ip\(|port\(|tcp\(|udp\(' README.md`
- `git diff --name-only`
- `git status --short --untracked-files=all`

## 时间盒与停止条件

- 时间盒：20 分钟。
- README 内容与当前实现/测试一致，文档 diff 检查通过后，将工作台标记为已完成并立即停止。
- 若发现文档需求依赖尚未实现的行为，只记录边界，不修改生产代码或扩大范围。

## 完成证据

- `README.md` 新增“SQL 过滤能力”，说明 source/operator stage 绑定规则和当前 block source/block transform
  接入范围；明确旧 DataFrame、database、stream 及传统 operator 路径仍受各自能力约束。
- 文档列出逻辑、比较、`IN`、`BETWEEN`、NULL、boolean、普通/类型化字面量和领域函数，并说明 SQL 三值
  逻辑、精确下推与 Arrow residual 的结果等价性。
- pcapfile 文档覆盖 packet header/decoded 常用字段、显式时区纳秒 `TIMESTAMP`、`mac`/`ip`/`port` 和双向
  `tcp`/`udp` endpoint pair；包含元数据、时间窗口、方向中立组合、TCP 和 UDP 五组 SQL 示例。
- 文档明确不支持的 `==`、`field = NULL`、位运算/BPF、比较链、payload/正则、应用识别和 TCP stream 边界。
- `git diff --check -- README.md tasks/active_task.md`：通过。
- README 关键标题/语法/函数检索：通过；Markdown 代码围栏共 50 个且成对；尾随空白检查通过。
- 本任务只修改 `README.md` 和 `tasks/active_task.md`；既有 Feature 累计 diff 未被修改。
- 纯文档任务未重复执行构建或测试，未读取/修改 `tasks/sprints/**`，未执行 commit/push。
