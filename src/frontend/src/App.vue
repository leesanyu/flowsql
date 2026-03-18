<template>
  <div class="app-layout">
    <!-- 顶部导航栏 -->
    <header class="app-nav">
      <!-- 左：Logo -->
      <div class="nav-logo">
        <span class="logo-dot"></span>
        <span class="logo-text">FlowSQL</span>
      </div>

      <!-- 中：导航菜单 -->
      <el-menu
        :default-active="route.path"
        mode="horizontal"
        :ellipsis="false"
        router
        class="nav-menu"
      >
        <el-menu-item index="/dashboard">仪表盘</el-menu-item>
        <el-menu-item index="/channels">通道列表</el-menu-item>
        <el-menu-item index="/operators">算子管理</el-menu-item>
        <el-menu-item index="/tasks">SQL 工作台</el-menu-item>
      </el-menu>

      <!-- 右：状态区 -->
      <div class="nav-right">
        <span class="gateway-status">
          <span class="status-dot" :class="gatewayOnline ? 'online' : 'offline'"></span>
          <span class="status-text">{{ gatewayOnline ? '在线' : '离线' }}</span>
        </span>
        <el-button
          :icon="isDark ? Sunny : Moon"
          circle
          size="small"
          @click="toggleTheme"
          title="切换主题"
        />
        <span class="nav-user">
          <el-icon size="13"><User /></el-icon>
          admin
        </span>
        <span class="nav-version">v{{ version }}</span>
      </div>
    </header>

    <!-- 内容区 -->
    <main class="app-main">
      <router-view />
    </main>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { useRoute } from 'vue-router'
import { Moon, Sunny, User } from '@element-plus/icons-vue'
import pkg from '../package.json'

const route = useRoute()
const version = pkg.version

// 主题初始化
const savedTheme = localStorage.getItem('theme')
const isDark = ref(savedTheme === 'dark')
if (isDark.value) document.documentElement.classList.add('dark')

const toggleTheme = () => {
  isDark.value = !isDark.value
  if (isDark.value) {
    document.documentElement.classList.add('dark')
    localStorage.setItem('theme', 'dark')
  } else {
    document.documentElement.classList.remove('dark')
    localStorage.setItem('theme', 'light')
  }
}

// Gateway 状态轮询
const gatewayOnline = ref(false)
const checkGateway = async () => {
  try {
    const res = await fetch('/api/health', { signal: AbortSignal.timeout(3000) })
    gatewayOnline.value = res.ok
  } catch {
    gatewayOnline.value = false
  }
}
let timer = null
onMounted(() => { checkGateway(); timer = setInterval(checkGateway, 30000) })
onUnmounted(() => clearInterval(timer))
</script>

<style>
.app-layout {
  display: flex;
  flex-direction: column;
  height: 100%;
}

/* 顶部导航栏 */
.app-nav {
  height: 56px;
  display: flex;
  align-items: center;
  padding: 0 24px;
  gap: 32px;
  background-color: var(--nav-bg);
  border-bottom: 1px solid var(--nav-border);
  flex-shrink: 0;
}

.nav-logo {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.logo-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background-color: var(--accent);
}

.logo-text {
  font-size: 16px;
  font-weight: 700;
  color: var(--text-primary);
  letter-spacing: 0.5px;
}

/* el-menu 水平模式覆盖 */
.nav-menu {
  flex: 1;
  border-bottom: none !important;
  background-color: transparent !important;
  height: 56px;
}

.nav-menu .el-menu-item {
  height: 56px;
  line-height: 56px;
  font-size: 14px;
  color: var(--nav-text) !important;
  border-bottom: 3px solid transparent !important;
  padding: 0 16px;
}

.nav-menu .el-menu-item:hover {
  background-color: transparent !important;
  color: var(--nav-active) !important;
}

.nav-menu .el-menu-item.is-active {
  color: var(--nav-active) !important;
  border-bottom-color: var(--nav-active-bar) !important;
  background-color: transparent !important;
  font-weight: 600;
}

/* 右侧状态区 */
.nav-right {
  display: flex;
  align-items: center;
  gap: 16px;
  flex-shrink: 0;
}

.gateway-status {
  display: flex;
  align-items: center;
  gap: 6px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.status-dot.online  { background-color: #1a7f37; }
.status-dot.offline { background-color: #cf222e; }

.status-text {
  font-size: 13px;
  color: var(--text-secondary);
}

.nav-user {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 13px;
  color: var(--text-secondary);
}

.nav-version {
  font-size: 11px;
  color: var(--text-secondary);
  opacity: 0.6;
}

/* 内容区 */
.app-main {
  flex: 1;
  overflow-y: auto;
  padding: 24px 32px;
  background-color: var(--bg-page);
}
</style>
