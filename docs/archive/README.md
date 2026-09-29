# 归档文档说明

本目录存放历史设计文档和阶段性分析报告，作为参考保留。分析报告的归档不代表其中建议已经成为产品决策。

## 归档文件列表

### 产品定位分析

- [FlowSQL 产品定位与近期投入方向分析报告（2026-09-29）](2026-09-29-flowsql-product-positioning-analysis.md)：比较托管离线平台与本地实时系统，记录条件化建议和验证方法；定位尚未确定。

### Stage 设计文档（已被 Scrum 体系替代）
- **stage1.md** - Sprint 1 设计文档（C++ 框架核心能力）
- **stage2.md** - Sprint 2 设计文档（Python 桥接 + Web 管理）
- **stage3.md** - Sprint 3 设计文档（数据库闭环 + 架构清理）

**替代文档**: `tasks/product_backlog.md` 和 `tasks/sprints/sprintN/` 目录

---

### 详细设计文档（功能已实现）
- **design_database_channel.md** - DatabaseChannel 详细设计文档（Phase 1-7）

**相关代码**: `src/services/database/`

---

### 操作指南（功能已稳定）
- **frontend_verification.md** - Vue.js 前端验证指南
- **operator_editor_guide.md** - 在线编写 Python 算子功能说明
- **operator_editor_implementation.md** - 算子编辑器实现细节

**相关代码**: `src/frontend/`

---

## 历史设计文档归档时间

2026-03-05

## 历史设计文档归档原因

建立 Scrum 任务管理体系后，历史设计文档已被结构化的 Sprint 文档替代。为保持文档目录清晰，将历史文档归档。

## 如何查找信息

### 查找历史功能设计
1. 先查看 `tasks/product_backlog.md` 了解功能概览
2. 再查看 `tasks/sprints/sprintN/planning.md` 了解详细设计
3. 如需更详细的设计细节，查看本目录的归档文档

### 查找当前架构
- 查看 `docs/framework.md`（当前架构文档）
- 查看 `docs/design.md`（项目愿景文档）

### 查找操作指南
- 前端操作：查看本目录的 `frontend_verification.md`
- 算子编写：查看本目录的 `operator_editor_guide.md`
