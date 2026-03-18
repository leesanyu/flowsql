# 前端 UI 改造设计文档

## 背景与目标

### 当前问题

1. **页面居中**：`style.css` 继承了 Vite 默认模板的 `body { place-items: center }` 和 `#app { max-width: 1280px; margin: 0 auto; text-align: center }`，导致页面只占屏幕中间一块区域
2. **颜色硬编码**：侧边栏 `#304156`、文字 `#303133`、图标 `#409eff` 等颜色散落在各组件，无法统一切换主题
3. **风格单薄**：直接使用 Element Plus 默认样式，缺乏专业感
4. **布局浪费**：左右结构中 Header 区域空洞，侧边栏占用水平空间，内容区纵向高度受限

### 目标

- 顶部导航栏布局（参考 GitHub / Linear 风格），内容区获得完整垂直高度
- 提供深色 / 浅色两套主题，随时可切换，刷新后保持
- 不改变任何业务逻辑，只改样式和布局

---

## 设计决策

| 问题 | 决策 |
|------|------|
| 整体布局 | **顶部导航栏 + 内容区**（上下结构），废弃侧边栏 |
| 导航菜单位置 | Header 中段，水平排列 4 个菜单项 |
| 主题切换按钮 | Header 右侧（月亮/太阳图标） |
| 状态信息 | Header 右侧：Gateway 连接状态（绿点/红点）+ 用户名（暂时固定 admin） |
| 版本号 | Header 右侧最末，最低视觉权重 |

---

## 技术方案

### 主题切换机制

利用 **Element Plus 内置暗色模式**：

- `main.js` 引入 `element-plus/theme-chalk/dark/css-vars.css`
- 切换时在 `<html>` 上 toggle `class="dark"`
- Element Plus 所有组件自动响应，无需逐个修改

自定义颜色通过 CSS 变量覆盖，在 `:root`（浅色）和 `.dark`（深色）下分别定义。同时覆盖 Element Plus 自身的 CSS 变量（`--el-bg-color` 等），避免组件与页面背景割裂。

### 颜色系统

| CSS 变量 | 浅色值 | 深色值 | 用途 |
|---------|--------|--------|------|
| `--nav-bg` | `#ffffff` | `#161b22` | 顶部导航背景 |
| `--nav-border` | `#d0d7de` | `#30363d` | 导航底部边框 |
| `--nav-text` | `#656d76` | `#8b949e` | 导航菜单文字 |
| `--nav-active` | `#1f2328` | `#e6edf3` | 当前页菜单文字 |
| `--nav-active-bar` | `#0969da` | `#58a6ff` | 当前页底部高亮条 |
| `--bg-page` | `#f6f8fa` | `#0d1117` | 页面背景 |
| `--bg-card` | `#ffffff` | `#161b22` | 卡片背景 |
| `--border-color` | `#d0d7de` | `#30363d` | 分割线、卡片边框 |
| `--text-primary` | `#1f2328` | `#e6edf3` | 主要文字 |
| `--text-secondary` | `#656d76` | `#8b949e` | 次要文字 |
| `--accent` | `#0969da` | `#58a6ff` | 链接、高亮 |

**Element Plus 变量覆盖**（`.dark` 下补充）：
```css
.dark {
  --el-bg-color: #161b22;
  --el-bg-color-page: #0d1117;
  --el-border-color: #30363d;
  --el-border-color-light: #21262d;
  --el-text-color-primary: #e6edf3;
  --el-text-color-regular: #8b949e;
}
```

### 布局结构

```
┌──────────────────────────────────────────────────────────────┐
│  ● FlowSQL  │ 仪表盘  通道列表  算子管理  SQL工作台  │ ●在线 🌙 👤 admin  v0.0.0 │
│             └─── 当前页底部 3px 蓝色高亮条 ───┘                              │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Main Content                                                                │
│  padding: 24px 32px                                                          │
│  height: calc(100vh - 56px)                                                  │
│  overflow-y: auto                                                            │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Header 三段式（高度 56px）：**
- 左：Logo（圆点图标 + "FlowSQL" 文字，16px bold）
- 中：`el-menu` 水平模式，4 个菜单项，active 状态底部 3px 高亮条
- 右：Gateway 状态（绿点/红点 + 文字）、主题切换按钮、用户名、版本号（11px，最低对比度）

**主题切换过渡**（`style.css` 全局）：
```css
*, *::before, *::after {
  transition: background-color 0.2s, border-color 0.2s, color 0.15s;
}
```

---

## 涉及文件

| 文件 | 变更 |
|------|------|
| `src/frontend/src/main.js` | 新增 1 行：引入 Element Plus 暗色 CSS |
| `src/frontend/src/style.css` | 完全重写：CSS 变量体系 + 全局重置 + 过渡动画 |
| `src/frontend/src/App.vue` | 重写：上下结构，顶部 NavBar + 内容区 |
| `src/frontend/src/components/Sidebar.vue` | **删除**，逻辑迁移到 App.vue 的 NavBar |
| `src/frontend/src/views/Dashboard.vue` | 移除硬编码颜色（含 `el-icon color` 属性）、移除 `max-width` |
| `src/frontend/src/views/Channels.vue` | 移除硬编码颜色、移除 `max-width` |
| `src/frontend/src/views/Operators.vue` | 移除硬编码颜色、移除 `max-width` |
| `src/frontend/src/views/Tasks.vue` | 移除硬编码颜色、移除 `max-width` |

---

## 实施步骤

### Step 1：`src/frontend/src/main.js`

在 `import 'element-plus/dist/index.css'` 后添加：
```js
import 'element-plus/theme-chalk/dark/css-vars.css'
```

### Step 2：`src/frontend/src/style.css`

完全重写：
- 移除 Vite 默认模板的居中样式
- 定义 CSS 变量（`:root` 浅色 + `.dark` 深色，含 Element Plus 变量覆盖）
- `html, body, #app { height: 100%; margin: 0 }`
- 全局过渡动画（`transition: background-color 0.2s, border-color 0.2s, color 0.15s`）

### Step 3：`src/frontend/src/App.vue`

重写为上下结构：
- `<script setup>`：主题初始化（读 `localStorage`）、切换逻辑、Gateway 状态轮询（30s）
- NavBar（56px）：三段式布局，`el-menu` 水平模式，右侧状态区
- 内容区：`height: calc(100vh - 56px); overflow-y: auto; padding: 24px 32px`
- 背景色使用 `var(--bg-page)`

### Step 4：删除 `src/frontend/src/components/Sidebar.vue`

> 需用户确认后执行物理删除

### Step 5：各 View 文件（Dashboard / Channels / Operators / Tasks）

- 移除 `.page-title { color: #303133 }` 等硬编码，改用 `var(--text-primary)`
- 移除 `max-width: 1400px` 限制

**Dashboard.vue 额外处理：**
- 三个统计卡片的 `el-icon` 内联 `color="#409eff"` / `color="#67c23a"` / `color="#e6a23c"` 全部移除，改为 CSS class，颜色统一使用 `var(--accent)`
- 三个卡片必须等高：`el-col` 使用 `:style="{ height: '100%' }"`，`el-card` 加 `height: 100%`，`stat-content` 加 `height: 100%; align-items: center`，确保内容多少不影响卡片高度一致
