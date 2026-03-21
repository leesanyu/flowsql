# Sprint 7 评审

## Sprint 信息

- **Sprint 周期**：Sprint 7
- **开始日期**：2026-03-18
- **结束日期**：2026-03-19
- **Sprint 目标**：Web UI 专业化改造（Epic 8）

---

## 已完成 Story

### Story 8.1：全屏布局 + 顶部导航 + 主题切换 ✅

**验收结果**：通过

- `style.css` 完全重写，CSS 变量体系（`:root` 浅色 + `.dark` 深色）
- `App.vue` 改为顶部导航栏布局（NavBar 56px + 内容区 `calc(100vh - 56px)`）
- NavBar 三段式：Logo | `el-menu` 水平菜单 | 状态区（Gateway 状态 + 主题切换 + 用户名 + 版本号）
- 主题切换通过 `localStorage` 持久化，刷新后恢复
- Element Plus 组件随主题自动响应，无割裂感

---

### Story 8.2：各页面适配主题 ✅

**验收结果**：通过

- Dashboard / Channels / Operators / Tasks 四个页面移除硬编码颜色，改用 CSS 变量
- 移除各页面 `max-width` 限制，内容铺满全屏
- Dashboard 统计卡片等高，图标颜色统一使用 `var(--accent)`
- 深色/浅色模式下四个页面视觉正常

---

### Story 8.3：数据库通道浏览器 ✅

**验收结果**：通过

- `DatabasePlugin` 新增 3 个端点：`POST /channels/database/tables`、`/describe`、`/preview`
- `table` 参数校验：`^[a-zA-Z0-9_.]+$`，防止 SQL 注入
- SQLite / MySQL / ClickHouse 三种数据库元数据 SQL 分支正确
- 预览固定 `SELECT * FROM <table> LIMIT 100`
- `Channels.vue` 数据库通道行新增"浏览"按钮（仅 sqlite/mysql/clickhouse 显示）
- 点击"浏览"打开 60% 宽 Drawer，左侧 180px 表列表，右侧 Tab（表结构 / 数据预览）
- 默认自动选中第一张表
- `web_server.cpp` 双通道注册（Init() httplib 代理 + EnumApiRoutes() IRouterHandle 代理）

---

## Sprint 6 遗留清理 ✅

- 物理删除 `service_manager.h/cpp` 和 `service_client.h/cpp`

---

## 技术债务识别

| 债务 | 优先级 | 说明 |
|------|--------|------|
| `Sidebar.vue` 保留未删除 | 低 | 规划作为未来二级菜单使用，暂不处理 |
| KeepAlive 集成测试缺失 | 中 | 需要真实 Gateway 进程，单元测试无法覆盖 |
| 数据库浏览器无分页 | 低 | 当前固定 LIMIT 100，大表预览受限 |

---

## 产品待办更新

- Epic 8 状态更新为 ✅ 已完成
- Story 8.1 / 8.2 / 8.3 全部标记为 ✅ 已完成

---

## 下一个 Sprint 候选

- Epic 9：Pipeline 增强与异步任务（Story 9.1 多算子 Pipeline、Story 9.2 异步任务执行）
- Story 4.2：PostgreSQL 驱动支持
- Story 11.1：用户认证与权限
