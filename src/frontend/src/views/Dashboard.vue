<template>
  <div class="dashboard">
    <h1 class="page-title">仪表盘</h1>

    <el-row :gutter="20" class="stats-row">
      <el-col :span="8">
        <el-card class="stat-card">
          <div class="stat-content">
            <el-icon class="stat-icon" color="#409eff"><Connection /></el-icon>
            <div class="stat-info">
              <div class="stat-value">{{ stats.channels }}</div>
              <div class="stat-label">通道总数</div>
            </div>
          </div>
        </el-card>
      </el-col>
      <el-col :span="8">
        <el-card class="stat-card">
          <div class="stat-content">
            <el-icon class="stat-icon" color="#67c23a"><Operation /></el-icon>
            <div class="stat-info">
              <div class="stat-value">{{ stats.activeOperators }} / {{ stats.totalOperators }}</div>
              <div class="stat-label">活跃算子 / 总数</div>
            </div>
          </div>
        </el-card>
      </el-col>
      <el-col :span="8">
        <el-card class="stat-card">
          <div class="stat-content">
            <el-icon class="stat-icon" color="#e6a23c"><Document /></el-icon>
            <div class="stat-info">
              <div class="stat-value">{{ stats.successTasks }} / {{ stats.totalTasks }}</div>
              <div class="stat-label">成功任务 / 总数</div>
            </div>
          </div>
        </el-card>
      </el-col>
    </el-row>

    <el-card class="recent-tasks">
      <template #header>
        <div class="card-header">
          <span>最近任务</span>
        </div>
      </template>
      <el-table :data="recentTasks" style="width: 100%">
        <el-table-column prop="id" label="ID" width="80" />
        <el-table-column prop="sql_text" label="SQL" show-overflow-tooltip />
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag :type="scope.row.status === 'completed' ? 'success' : 'danger'">
              {{ scope.row.status }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="created_at" label="创建时间" width="180" />
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { Connection, Operation, Document } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const stats = ref({
  channels: 0,
  activeOperators: 0,
  totalOperators: 0,
  successTasks: 0,
  totalTasks: 0
})

const recentTasks = ref([])

const loadData = async () => {
  try {
    const [channelsRes, operatorsRes, tasksRes] = await Promise.all([
      api.getChannels(),
      api.getOperators(),
      api.getTasks()
    ])

    const channels = channelsRes.data
    const operators = operatorsRes.data
    const tasks = tasksRes.data

    stats.value.channels = channels.length
    stats.value.totalOperators = operators.length
    stats.value.activeOperators = operators.filter(op => op.active == '1').length

    stats.value.totalTasks = tasks.length
    stats.value.successTasks = tasks.filter(t => t.status === 'completed').length

    recentTasks.value = tasks.slice(0, 5)
  } catch (error) {
    ElMessage.error('加载数据失败: ' + error.message)
  }
}

onMounted(() => {
  loadData()
})
</script>

<style scoped>
.dashboard {
  max-width: 1400px;
}

.page-title {
  font-size: 24px;
  font-weight: 600;
  margin-bottom: 20px;
  color: #303133;
}

.stats-row {
  margin-bottom: 20px;
}

.stat-card {
  cursor: pointer;
  transition: transform 0.2s;
}

.stat-card:hover {
  transform: translateY(-4px);
}

.stat-content {
  display: flex;
  align-items: center;
  padding: 10px;
}

.stat-icon {
  font-size: 48px;
  margin-right: 20px;
}

.stat-info {
  flex: 1;
}

.stat-value {
  font-size: 28px;
  font-weight: 600;
  color: #303133;
  margin-bottom: 5px;
}

.stat-label {
  font-size: 14px;
  color: #909399;
}

.recent-tasks {
  margin-top: 20px;
}

.card-header {
  font-size: 16px;
  font-weight: 600;
}
</style>
