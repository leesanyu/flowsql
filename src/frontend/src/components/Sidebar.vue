<template>
  <aside class="sidebar">
    <!-- Logo -->
    <div class="sidebar-logo">
      <span class="logo-dot"></span>
      <span class="logo-text">FlowSQL</span>
    </div>

    <!-- 导航菜单 -->
    <nav class="sidebar-nav">
      <router-link
        v-for="item in navItems"
        :key="item.path"
        :to="item.path"
        class="nav-item"
        :class="{ active: route.path === item.path }"
      >
        <el-icon><component :is="item.icon" /></el-icon>
        <span>{{ item.label }}</span>
      </router-link>
    </nav>

    <!-- 底部信息 -->
    <div class="sidebar-footer">
      <div class="footer-row">
        <el-icon size="14"><User /></el-icon>
        <span>admin</span>
      </div>
      <div class="footer-row">
        <span class="status-dot" :class="gatewayOnline ? 'online' : 'offline'"></span>
        <span>Gateway {{ gatewayOnline ? '在线' : '离线' }}</span>
      </div>
      <div class="footer-row version">v{{ version }}</div>
    </div>
  </aside>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue'
import { useRoute } from 'vue-router'
import { DataAnalysis, Connection, Operation, Document, User } from '@element-plus/icons-vue'
import pkg from '../../package.json'

const route = useRoute()
const version = pkg.version
const gatewayOnline = ref(false)

const navItems = [
  { path: '/dashboard', label: '仪表盘', icon: DataAnalysis },
  { path: '/channels',  label: '通道列表', icon: Connection },
  { path: '/operators', label: '算子管理', icon: Operation },
  { path: '/tasks',     label: 'SQL 工作台', icon: Document },
]

const checkGateway = async () => {
  try {
    const res = await fetch('/api/health', { signal: AbortSignal.timeout(3000) })
    gatewayOnline.value = res.ok
  } catch {
    gatewayOnline.value = false
  }
}

let timer = null
onMounted(() => {
  checkGateway()
  timer = setInterval(checkGateway, 30000)
})
onUnmounted(() => clearInterval(timer))
</script>

<style scoped>
.sidebar {
  width: 220px;
  flex-shrink: 0;
  background-color: var(--sidebar-bg);
  border-right: 1px solid var(--sidebar-border);
  display: flex;
  flex-direction: column;
  height: 100%;
}

.sidebar-logo {
  height: 48px;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 0 20px;
  border-bottom: 1px solid var(--sidebar-border);
}

.logo-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background-color: var(--sidebar-active-text);
  flex-shrink: 0;
}

.logo-text {
  font-size: 16px;
  font-weight: 700;
  color: var(--text-primary);
  letter-spacing: 0.5px;
}

.sidebar-nav {
  flex: 1;
  padding: 12px 0;
}

.nav-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 16px;
  margin: 2px 8px;
  border-radius: 6px;
  color: var(--sidebar-text);
  text-decoration: none;
  font-size: 14px;
  transition: background-color 0.15s, color 0.15s;
  position: relative;
}

.nav-item:hover {
  background-color: var(--sidebar-hover-bg);
  color: var(--text-primary);
}

.nav-item.active {
  color: var(--sidebar-active-text);
  background-color: var(--sidebar-active-bg);
  font-weight: 600;
}

.sidebar-footer {
  padding: 16px 20px;
  border-top: 1px solid var(--sidebar-border);
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.footer-row {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  color: var(--sidebar-text);
}

.version {
  color: var(--text-secondary);
  font-size: 11px;
}

.status-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  flex-shrink: 0;
}

.status-dot.online  { background-color: #1a7f37; }
.status-dot.offline { background-color: #cf222e; }
</style>
