<template>
  <div class="sidebar">
    <div class="logo">
      <h2>FlowSQL</h2>
    </div>
    <el-menu
      :default-active="activeMenu"
      class="sidebar-menu"
      background-color="#304156"
      text-color="#bfcbd9"
      active-text-color="#409eff"
      router
    >
      <el-menu-item index="/dashboard">
        <el-icon><DataAnalysis /></el-icon>
        <span>仪表盘</span>
      </el-menu-item>
      <el-menu-item index="/channels">
        <el-icon><Connection /></el-icon>
        <span>通道列表</span>
      </el-menu-item>
      <el-menu-item index="/operators">
        <el-icon><Operation /></el-icon>
        <span>算子管理</span>
      </el-menu-item>
      <el-menu-item index="/tasks">
        <el-icon><Document /></el-icon>
        <span>SQL 工作台</span>
      </el-menu-item>
    </el-menu>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import { DataAnalysis, Connection, Operation, Document } from '@element-plus/icons-vue'

const route = useRoute()
const activeMenu = computed(() => route.path)
</script>

<style scoped>
.sidebar {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.logo {
  height: 60px;
  display: flex;
  align-items: center;
  justify-content: center;
  background-color: #263445;
  color: #fff;
}

.logo h2 {
  font-size: 20px;
  font-weight: 600;
}

.sidebar-menu {
  flex: 1;
  border: none;
}

.el-menu-item {
  height: 50px;
  line-height: 50px;
}
</style>
