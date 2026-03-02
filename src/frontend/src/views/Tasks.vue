<template>
  <div class="tasks">
    <h1 class="page-title">SQL 工作台</h1>

    <!-- SQL 编辑器 -->
    <el-card class="sql-editor-card">
      <template #header>
        <div class="card-header">
          <span>SQL 编辑器</span>
          <el-button type="primary" @click="executeSQL" :loading="executing">
            <el-icon><CaretRight /></el-icon>
            执行
          </el-button>
        </div>
      </template>

      <el-input
        v-model="sqlText"
        type="textarea"
        :rows="8"
        placeholder="输入 SQL 查询，例如：SELECT * FROM test_data USING explore.chisquare WITH threshold=0.05"
        class="sql-textarea"
      />

      <!-- 执行结果 -->
      <div v-if="currentResult" class="result-section">
        <el-divider content-position="left">执行结果</el-divider>
        <div v-if="currentResult.error" class="error-message">
          <el-alert type="error" :title="currentResult.error" :closable="false" />
        </div>
        <div v-else>
          <div class="result-meta">
            <el-tag>{{ currentResult.rows?.length || 0 }} 行</el-tag>
            <el-tag type="info">{{ currentResult.columns?.length || 0 }} 列</el-tag>
          </div>
          <el-table
            :data="currentResult.rows"
            style="width: 100%; margin-top: 10px"
            max-height="400"
            border
          >
            <el-table-column
              v-for="col in currentResult.columns"
              :key="col"
              :prop="col"
              :label="col"
              min-width="120"
              show-overflow-tooltip
            />
          </el-table>
        </div>
      </div>
    </el-card>

    <!-- 任务历史 -->
    <el-card class="tasks-history">
      <template #header>
        <div class="card-header">
          <span>任务历史</span>
          <el-button @click="loadTasks" :icon="Refresh" circle />
        </div>
      </template>

      <el-table :data="tasks" style="width: 100%" v-loading="loading">
        <el-table-column prop="id" label="ID" width="80" />
        <el-table-column prop="sql_text" label="SQL" show-overflow-tooltip min-width="300" />
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag :type="scope.row.status === 'completed' ? 'success' : 'danger'">
              {{ scope.row.status }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="created_at" label="创建时间" width="180" />
        <el-table-column label="操作" width="120">
          <template #default="scope">
            <el-button
              type="primary"
              size="small"
              @click="viewResult(scope.row.id)"
            >
              查看结果
            </el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 结果查看对话框 -->
    <el-dialog v-model="resultDialogVisible" title="任务结果" width="80%">
      <div v-if="dialogResult">
        <div v-if="dialogResult.error" class="error-message">
          <el-alert type="error" :title="dialogResult.error" :closable="false" />
        </div>
        <div v-else>
          <div class="result-meta">
            <el-tag>{{ dialogResult.rows?.length || 0 }} 行</el-tag>
            <el-tag type="info">{{ dialogResult.columns?.length || 0 }} 列</el-tag>
          </div>
          <el-table
            :data="dialogResult.rows"
            style="width: 100%; margin-top: 10px"
            max-height="500"
            border
          >
            <el-table-column
              v-for="col in dialogResult.columns"
              :key="col"
              :prop="col"
              :label="col"
              min-width="120"
              show-overflow-tooltip
            />
          </el-table>
        </div>
      </div>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { CaretRight, Refresh } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const sqlText = ref('')
const executing = ref(false)
const currentResult = ref(null)
const tasks = ref([])
const loading = ref(false)
const resultDialogVisible = ref(false)
const dialogResult = ref(null)

const executeSQL = async () => {
  if (!sqlText.value.trim()) {
    ElMessage.warning('请输入 SQL 语句')
    return
  }

  executing.value = true
  currentResult.value = null

  try {
    const res = await api.createTask(sqlText.value)
    const taskId = res.data.task_id
    ElMessage.success(`任务已提交 (ID: ${taskId})`)

    // 获取结果
    const resultRes = await api.getTaskResult(taskId)
    const result = resultRes.data

    if (result.status === 'completed' && result.data) {
      // 转换数据格式：将 data.data 数组转换为对象数组
      const rows = result.data.data.map(row => {
        const obj = {}
        result.data.columns.forEach((col, idx) => {
          obj[col] = row[idx]
        })
        return obj
      })
      currentResult.value = {
        columns: result.data.columns,
        rows: rows
      }
    } else if (result.status === 'failed') {
      currentResult.value = { error: result.error || '执行失败' }
    }

    // 刷新任务列表
    await loadTasks()
  } catch (error) {
    // 提取后端返回的具体错误信息
    const detail = error.response?.data?.error || error.message
    ElMessage.error('执行失败: ' + detail)
    currentResult.value = { error: detail }
  } finally {
    executing.value = false
  }
}

const loadTasks = async () => {
  loading.value = true
  try {
    const res = await api.getTasks()
    tasks.value = res.data
  } catch (error) {
    ElMessage.error('加载任务列表失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

const viewResult = async (taskId) => {
  try {
    const res = await api.getTaskResult(taskId)
    const result = res.data

    if (result.status === 'completed' && result.data) {
      // 转换数据格式
      const rows = result.data.data.map(row => {
        const obj = {}
        result.data.columns.forEach((col, idx) => {
          obj[col] = row[idx]
        })
        return obj
      })
      dialogResult.value = {
        columns: result.data.columns,
        rows: rows
      }
    } else if (result.status === 'failed') {
      dialogResult.value = { error: result.error || '执行失败' }
    }

    resultDialogVisible.value = true
  } catch (error) {
    ElMessage.error('加载结果失败: ' + error.message)
  }
}

onMounted(() => {
  loadTasks()
})
</script>

<style scoped>
.tasks {
  max-width: 1400px;
}

.page-title {
  font-size: 24px;
  font-weight: 600;
  margin-bottom: 20px;
  color: #303133;
}

.sql-editor-card {
  margin-bottom: 20px;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.sql-textarea {
  font-family: 'Courier New', monospace;
  font-size: 14px;
}

.result-section {
  margin-top: 20px;
}

.result-meta {
  display: flex;
  gap: 10px;
  margin-bottom: 10px;
}

.error-message {
  margin: 10px 0;
}

.tasks-history {
  margin-top: 20px;
}
</style>
