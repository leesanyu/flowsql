# Sprint 阶段索引

本文用于按阶段和主要能力组织历史 Sprint，便于在迭代数量增加后快速定位资料。当前不移动既有 `sprintN/` 目录，历史引用继续保持原路径。

## 使用方式

- 查某次迭代的计划、设计、评审和回顾：直接进入对应 `sprintN/` 目录。
- 查某类能力的演进脉络：先按下方阶段定位，再查看阶段内的 Sprint 文档。
- 新建 Sprint 时继续放在 `tasks/sprints/` 下，建议目录名采用 `sprintN-能力关键词`，例如 `sprint22-packet-data-plane`。
- 只有当 Sprint 数量继续显著增长、索引已无法满足查找需求时，再考虑物理移动目录做阶段分组。

## 阶段总览

| 阶段 | Sprint 范围 | 主线能力 | 说明 |
|------|-------------|----------|------|
| Phase 1 | Sprint 1-3 | 框架、插件、Python 桥接、数据库闭环 | 建立核心运行框架和基础数据处理能力 |
| Phase 2 | Sprint 4-7 | 多数据库、Web 管理、RouterAgency | 补齐数据库扩展、Web 管理和服务对等化架构 |
| Phase 3 | Sprint 8-11 | Catalog、算子目录、Pipeline、C++ 算子插件 | 统一算子/通道注册管理，增强 Pipeline 和插件扩展 |
| Phase 4 | Sprint 12-18 | 流式架构、任务编排、可视化 | 建立流式执行路径、任务 DAG 和运行态可视化 |
| Phase 5 | Sprint 19-21 | Baseline 插件与算法生命周期 | 从基线算法设计到 BaselineA / BaselineB 实现收口 |
| Phase 6 | Sprint 22+ | Packet 数据面、pcapfile、NPM | 面向网络流量分析的内生数据源与 NPM 能力建设，将以新的开发模式展开，不在此处进行跟踪 |

## Phase 1：框架与基础数据能力

- [Sprint 1](sprint1/)：C++ 框架核心能力，包含基础接口、Pipeline、Arrow 集成和示例插件。
- [Sprint 2](sprint2/)：Python 算子桥接与 Web 管理，建立 C++ / Python 协作路径。
- [Sprint 3](sprint3/)：数据库闭环与架构清理，回归纯插件架构并补齐数据库通道。

## Phase 2：数据库、Web 与路由架构

- [Sprint 4](sprint4/)：多数据库支持与 SQL 增强，补充 MySQL、连接池和 SQL 能力。
- [Sprint 5](sprint5/)：ClickHouse 通道与 Web 数据库通道管理。
- [Sprint 6](sprint6/)：RouterAgency 与服务对等化架构，统一控制面路由。
- [Sprint 7](sprint7/)：Web UI 专业化改造和数据库通道浏览体验。

## Phase 3：Catalog、算子与 Pipeline 增强

- [Sprint 8](sprint8/)：内置通道与算子注册中心。
- [Sprint 9](sprint9/)：算子目录和状态下沉 CatalogPlugin。
- [Sprint 10](sprint10/)：多算子 Pipeline 增强与异步任务能力。
- [Sprint 11](sprint11/)：C++ 算子插件和相关扩展收口。

## Phase 4：流式架构与任务编排

- [Sprint 12](sprint12/)：流式通道、流式算子和 StreamRuntime 基础能力。
- [Sprint 13](sprint13/)：跨进程流式入口、Web 流式管理和 Ring 并发模式补齐。
- [Sprint 14](sprint14/)：内置通道/算子统一加载、Stream Sink 产品化和流式 Group DAG。
- [Sprint 15 Refactoring](sprint15-refactoring/)：任务与调度相关重构。
- [Sprint 16](sprint16/)：历史数据批处理补算等流批协同能力。
- [Sprint 17](sprint17/)：流式 Group DAG 与 Hybrid DAG 编排增强。
- [Sprint 18](sprint18/)：执行实例单画布 DAG 可视化。

## Phase 5：Baseline 插件与算法生命周期

- [Sprint 19 Baseline](sprint19-baseline/)：基线统一算法设计与失败复盘，保留为算法设计和教训入口。
- [Sprint 20 BaselineA](sprint20-baselineA/)：BaselineA 固定历史模型、正式重建和一致性整改。
- [Sprint 21 BaselineB](sprint21-baselineB/)：BaselineB 在线 rolling、Optional Bootstrap、Relation fusion 和批量预测优化。

## Phase 6：网络流量数据面与 NPM

- 此后的规划与实施不再采用sprint模式，规划与任务不再在本文档以及本目录中呈现。

## Sprint 设计文档规范

### 1. 文档职责

- `planning.md` 是 Story/Task 范围、Task ID、依赖、估算、状态和实施顺序的唯一来源。
- `design.md` 是接口、数据结构、状态机、实现方法、测试方法、失败条件和验收门槛的唯一来源。
- `planning.md` 不复制大段设计细节；`design.md` 不另建一套 Task 编号、依赖或工作量。
- 验收 ID 是 planning、design、测试、评审和 Sprint Review 的稳定追踪键。

### 2. 编排原则

`design.md` 必须采用“基础设计与公共依赖 + 按 Task 编排”的结构：

```text
design.md
├── 1. 设计范围与关键决策
├── 2. 基础设计与公共依赖
│   ├── 公共术语和身份模型
│   ├── 公共接口、ABI 和文件归属
│   ├── 生命周期、并发和错误原则
│   └── 公共设计归属表
├── 3. T0：与 planning 完全一致的 Task 名称
├── 4. T1：与 planning 完全一致的 Task 名称
├── ...
└── N. Definition of Done 与文档同步
```

公共设计只接收以下内容：

1. 被 2 个以上 Task 直接依赖的底层契约。
2. 会冻结跨 Task public ABI、身份、生命周期、并发或错误语义的决策。
3. 多个测试 Task 共用的样本协议、证据格式或测试基础设施。

公共设计必须列出 owner Task 和 consumer Task。只服务一个 Task 的技术内容必须放回该 Task 章节，禁止把 `design.md` 再写成按技术主题平铺的参考手册。

### 3. Task 是否需要设计章节

判断标准不是“是否产出业务代码”，而是“是否包含需要在实施前冻结的设计责任”。以下任一成立，就必须建立 Task 设计章节：

- 定义或修改接口、数据结构、状态机、算法、生命周期或错误语义。
- 决定文件/模块职责、ABI、兼容或迁移策略。
- 设计测试样本、fixture、mock、故障注入或回归路径。
- 定义 CTest、Sanitizer、TSan、Fuzz、性能基准的运行方法或通过门槛。
- 定义验收证据结构、自动检查规则或发布门禁。

纯状态更新、勾选完成、汇总已经定义好的证据等行政性 Task 可以不设章节，但 planning 必须显式写明“无独立设计责任”。一旦流程 Task 自己定义验证方法或门槛，就不再适用该例外。

### 4. Task 章节模板

```markdown
## N. Tn：Task 名称

### N.1 设计目标

说明本 Task 要冻结的实现或验证决策，以及明确不承担的职责。

### N.2 对应验收 ID

列出完整验收 ID；没有直接关闭验收时写明原因，不能写含义不清的“全部”。

### N.3 公共依赖

引用基础设计小节，并说明本 Task 消费哪些契约。

### N.4 实现或验证设计

写清接口、数据结构、核心流程、状态转换、测试架构或证据生成方式。

### N.5 错误、边界与并发

说明失败语义、边界输入、资源回收和线程模型；不适用时明确标注。

### N.6 文件落点

列出创建/修改/测试文件及职责，不用模糊的 `相关文件` 代替。

### N.7 测试设计与通过门槛

列出用例、命令、预期结果和失败条件。测试 Task 必须同时说明被测 Task。

### N.8 完成出口

列出该 Task 可以在 planning 中勾选完成的必要条件。
```

简单 Task 可以合并不适用的小节，但“验收 ID、公共依赖、方案、文件落点、测试门槛、完成出口”不能缺失。

### 5. 测试设计归属

- 实现 Task 负责本地单元测试、接口契约测试和本 Task 边界测试。
- 测试 Task 负责跨 Task 集成、生产路径回归、CTest 注册、Sanitizer、TSan、Fuzz 和性能验证。
- 测试 Task 可以再次引用验收 ID，但不能成为某个功能验收 ID 的唯一实现责任方。
- 每个验收 ID 至少要有一个实现/契约 Task 和一个可执行验证落点。
- 测试不是文末附录；承担测试设计责任的 T0/T8/T9 等 Task 必须有正式设计章节。

### 6. 同步与审查清单

Sprint 开工前逐项检查：

- [ ] planning 与 design 中的 Task ID、名称和顺序一致。
- [ ] 每个有设计责任的 Task 都有设计章节。
- [ ] 每个公共设计项都有 owner Task 和 consumer Task。
- [ ] 每个验收 ID 都被实现 Task 和测试目标覆盖。
- [ ] Task 章节引用的文件、测试目标和依赖与 planning 一致。
- [ ] 测试命令可执行，失败条件和通过门槛明确。
- [ ] 非目标、Scheduler/发布门禁和延期范围没有漂移。
- [ ] 不存在依赖文末映射表才能理解的孤立技术章节。

Task 增删、拆分、合并或重编号时，planning 与 design 必须在同一次变更中更新。若 Task 设计需要改变已确认架构，先评审设计，再更新计划和实现。
