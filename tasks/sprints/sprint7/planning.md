# Sprint 7 规划

## Sprint 信息

- **Sprint 周期**：Sprint 7
- **开始日期**：2026-03-18
- **结束日期**：2026-03-19
- **预计工作量**：5-7 天
- **Sprint 目标**：Web UI 专业化改造（Epic 8）——顶部导航布局、深色/浅色主题切换、数据库通道浏览器

---

## Sprint 目标

### 主要目标

1. **UI 布局重构**：废弃侧边栏，改为顶部导航栏（GitHub / Linear 风格），内容区全屏铺开
2. **主题系统**：引入 CSS 变量体系，支持深色/浅色切换，`localStorage` 持久化，Element Plus 组件全响应
3. **数据库通道浏览器**：在通道列表页新增"浏览"入口，支持查看表列表、表结构、数据预览（前 100 条）

### 成功标准

- [ ] 页面铺满全屏，无居中限制
- [ ] 顶部导航三段式：Logo | 菜单 | 状态区（Gateway 状态 + 主题切换 + 用户 + 版本号）
- [ ] 深色/浅色主题切换正常，所有 Element Plus 组件响应，无割裂感
- [ ] 刷新后主题状态保持
- [ ] Dashboard 三个统计卡片等高，图标颜色统一使用 `var(--accent)`
- [ ] 通道列表页数据库通道行显示"浏览"按钮
- [ ] 点击"浏览"打开 Drawer，左侧表列表，右侧表结构/数据预览 Tab
- [ ] 三种数据库（SQLite / MySQL / ClickHouse）浏览功能正常
- [ ] `npm run build` 编译无报错
- [ ] 所有现有功能回归正常

---

## 设计文档

- [UI 改造设计](design_frontend_ui.md)
- [数据库通道浏览器设计](design_db_browser.md)

---

## Story 列表

---

### Story 8.1：全屏布局 + 顶部导航 + 主题切换

**优先级**：P0（基础，8.2 和 8.3 依赖此 Story 的布局）
**工作量估算**：2 天
**依赖**：无

**验收标准**：
- [x] `style.css` 完全重写，移除 Vite 默认居中样式，定义 CSS 变量体系（`:root` 浅色 + `.dark` 深色）
- [x] `.dark` 下覆盖 Element Plus 自身 CSS 变量（`--el-bg-color` 等），组件与页面背景无割裂
- [x] 全局过渡动画：`transition: background-color 0.2s, border-color 0.2s, color 0.15s`
- [x] `App.vue` 重写为上下结构：NavBar（56px）+ 内容区（`calc(100vh - 56px)`，`overflow-y: auto`）
- [x] NavBar 三段式：左 Logo、中 `el-menu` 水平菜单（active 底部 3px 高亮条）、右状态区
- [x] 右侧状态区：Gateway 状态（绿点/红点，30s 轮询 `/api/health`）、主题切换按钮、用户名（admin）、版本号（11px 低对比度）
- [x] 主题切换通过 `localStorage` 持久化，刷新后恢复
- [x] `Sidebar.vue` 物理删除（保留，未来作为二级菜单使用）

**任务分解**：
- [x] 重写 `src/frontend/src/style.css`
- [x] `src/frontend/src/main.js` 引入 `element-plus/theme-chalk/dark/css-vars.css`
- [x] 重写 `src/frontend/src/App.vue`
- [ ] 删除 `src/frontend/src/components/Sidebar.vue`（保留，未来二级菜单）

---

### Story 8.2：各页面适配主题

**优先级**：P0（Story 8.1 完成后立即执行）
**工作量估算**：1 天
**依赖**：Story 8.1

**验收标准**：
- [x] Dashboard / Channels / Operators / Tasks 四个页面移除所有硬编码颜色，改用 CSS 变量
- [x] 移除各页面 `max-width: 1400px` 限制，内容铺满
- [x] Dashboard 三个统计卡片等高（`el-row align-items: stretch`，`el-card height: 100%`）
- [x] Dashboard 统计卡片图标颜色统一改为 `var(--accent)`（移除内联 `color` 属性）
- [x] 深色/浅色模式下四个页面视觉正常，无白块或文字不可见问题

**任务分解**：
- [x] 修改 `src/frontend/src/views/Dashboard.vue`
- [x] 修改 `src/frontend/src/views/Channels.vue`
- [x] 修改 `src/frontend/src/views/Operators.vue`
- [x] 修改 `src/frontend/src/views/Tasks.vue`

---

### Story 8.3：数据库通道浏览器

**优先级**：P1
**工作量估算**：2-3 天
**依赖**：Story 8.1（布局就绪后前端开发更顺畅，但可并行）

**验收标准**：
- [x] `DatabasePlugin` 新增 3 个端点：`POST /channels/database/tables`、`/describe`、`/preview`
- [x] `table` 参数校验：只允许 `^[a-zA-Z0-9_.]+$`，防止 SQL 注入
- [x] SQLite / MySQL / ClickHouse 三种数据库元数据 SQL 分支正确
- [x] 预览固定 `SELECT * FROM <table> LIMIT 100`，不接受用户输入 SQL
- [x] `Channels.vue` 数据库通道行新增"浏览"按钮（仅 sqlite/mysql/clickhouse 显示）
- [x] 点击"浏览"打开 60% 宽 Drawer，左侧 180px 表列表，右侧 Tab（表结构 / 数据预览）
- [x] 默认自动选中第一张表
- [x] curl 直接测试 DatabasePlugin 端口（18803）三个端点返回正确

**任务分解**：
- [x] `src/services/database/database_plugin.h`：新增 3 个 Handler 声明，`EnumRoutes` 追加 3 条路由
- [x] `src/services/database/database_plugin.cpp`：实现 `HandleTables` / `HandleDescribe` / `HandlePreview`
- [x] `src/services/web/web_server.cpp`：Init() 和 EnumApiRoutes() 各补充 3 条代理路由
- [x] `src/frontend/src/api/index.js`：新增 `listDbTables` / `describeDbTable` / `previewDbTable`
- [x] `src/frontend/src/views/Channels.vue`：新增"浏览"按钮 + `el-drawer` 组件

---

## 风险与缓解

| 风险 | 可能性 | 缓解措施 |
|------|--------|---------|
| Element Plus `el-menu` 水平模式样式与自定义 CSS 变量冲突 | 中 | 优先验证 NavBar 渲染，必要时用自定义 `<nav>` 替代 `el-menu` |
| 深色模式下 Element Plus 弹窗/对话框背景未覆盖 | 中 | 逐一检查 `el-dialog`、`el-popover`、`el-drawer`，补充 `--el-*` 变量覆盖 |
| ClickHouse `DESCRIBE` 返回格式与 MySQL 不同 | 低 | 测试时验证，必要时单独处理列名映射 |

---

## Sprint 6 遗留清理（Sprint 7 开始时处理）

- [x] 物理删除 `service_manager.h/cpp` 和 `service_client.h/cpp`（文件仍在磁盘，已从 CMakeLists 排除）
