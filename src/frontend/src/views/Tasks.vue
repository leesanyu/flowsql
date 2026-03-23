<template>
  <div class="tasks">
    <h1 class="page-title">SQL 工作台</h1>

    <!-- SQL 编辑器 -->
    <el-card class="sql-editor-card">
      <template #header>
        <div class="card-header">
          <span>SQL 编辑器</span>
          <div class="header-actions">
            <el-radio-group v-model="executeMode" size="small">
              <el-radio-button label="sync">同步</el-radio-button>
              <el-radio-button label="async">异步</el-radio-button>
            </el-radio-group>
            <el-button type="primary" @click="executeSQL" :loading="executing">
              <el-icon><CaretRight /></el-icon>
              执行
            </el-button>
          </div>
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
        <div v-else-if="currentResult.message">
          <el-alert type="success" :title="currentResult.message" :closable="false" />
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
        <el-table-column prop="id" label="任务ID" min-width="260" show-overflow-tooltip />
        <el-table-column prop="sql_text" label="SQL" show-overflow-tooltip min-width="300" />
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag :type="scope.row.status === 'completed' ? 'success' : 'danger'">
              {{ scope.row.status }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="created_at" label="创建时间" width="180" />
        <el-table-column label="操作" width="200">
          <template #default="scope">
            <el-button
              type="primary"
              size="small"
              :disabled="!isTerminal(scope.row.status)"
              :loading="resultLoadingId === (scope.row.id || scope.row.task_id)"
              @click="viewResult(scope.row.id || scope.row.task_id)"
            >
              查看结果
            </el-button>
            <el-button
              type="danger"
              size="small"
              :disabled="!isTerminal(scope.row.status)"
              @click="deleteTask(scope.row)"
            >
              删除
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
        <div v-else-if="dialogResult.message">
          <el-alert type="success" :title="dialogResult.message" :closable="false" />
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
import { ref, onMounted, onUnmounted } from 'vue'
import { CaretRight, Refresh } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage, ElMessageBox } from 'element-plus'

const sqlText = ref('')
const executeMode = ref('sync')
const executing = ref(false)
const currentResult = ref(null)
const tasks = ref([])
const loading = ref(false)
const resultDialogVisible = ref(false)
const dialogResult = ref(null)
const resultLoadingId = ref('')
const currentTaskId = ref('')
let pollTimer = null

const isTerminal = (status) => ['completed', 'failed', 'cancelled', 'timeout'].includes(status)

const formatTaskResult = (result, runningMessage = '') => {
  if (Array.isArray(result.data)) {
    if (result.status === 'completed' && result.data.length === 0) {
      return {
        columns: [],
        rows: [],
        message: `执行完成（${result.rows || 0} 行，${result.cols || 0} 列）`
      }
    }
    const rows = result.data
    const columns = rows.length > 0 && typeof rows[0] === 'object' && rows[0] !== null
      ? Object.keys(rows[0])
      : []
    return { columns, rows }
  }
  if (result.status === 'completed' && result.data) {
    if (result.data.columns && result.data.data) {
      const rows = result.data.data.map(row => {
        const obj = {}
        result.data.columns.forEach((col, idx) => {
          obj[col] = row[idx]
        })
        return obj
      })
      return {
        columns: result.data.columns,
        rows
      }
    }
    return {
      columns: [],
      rows: [],
      message: `执行完成（${result.rows || 0} 行已写入）`
    }
  }
  if (result.status === 'failed') {
    return { error: result.error || '执行失败' }
  }
  if (result.status === 'pending' || result.status === 'running') {
    return {
      columns: [],
      rows: [],
      message: runningMessage || `任务执行中（${result.status}）`
    }
  }
  return {
    columns: [],
    rows: [],
    message: `任务状态：${result.status || 'unknown'}`
  }
}

const executeSQL = async () => {
  if (!sqlText.value.trim()) {
    ElMessage.warning('请输入 SQL 语句')
    return
  }

  executing.value = true
  currentResult.value = null

  try {
    const res = await api.createTask(sqlText.value, executeMode.value)
    const submit = res.data || {}
    const taskId = submit.task_id

    if (executeMode.value === 'sync') {
      currentTaskId.value = ''
      currentResult.value = formatTaskResult(submit)
      if (submit.status === 'failed') {
        ElMessage.error('同步执行失败')
      } else {
        ElMessage.success('同步执行完成')
      }
      await loadTasks()
      return
    }

    currentTaskId.value = taskId
    ElMessage.success(`任务已提交 (ID: ${taskId})`)
    currentResult.value = {
      columns: [],
      rows: [],
      message: `任务 ${taskId} 已提交，正在异步执行`
    }

    // 刷新任务列表
    await loadTasks()
  } catch (error) {
    // 提取后端返回的具体错误信息
    const detail = error.response?.data?.error || error.message || '未知错误'
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
    const payload = res.data || {}
    tasks.value = Array.isArray(payload) ? payload : (payload.items || [])
    if (currentTaskId.value) {
      const current = tasks.value.find(t => (t.id || t.task_id) === currentTaskId.value)
      if (current && isTerminal(current.status)) {
        const resultRes = await api.getTaskResult(currentTaskId.value)
        currentResult.value = formatTaskResult(resultRes.data)
        currentTaskId.value = ''
      }
    }
  } catch (error) {
    ElMessage.error('加载任务列表失败: ' + (error.message || '未知错误'))
  } finally {
    loading.value = false
  }
}

const viewResult = async (taskId) => {
  resultLoadingId.value = String(taskId || '')
  try {
    const res = await api.getTaskResult(taskId)
    const result = res.data
    dialogResult.value = formatTaskResult(result)

    resultDialogVisible.value = true
  } catch (error) {
    ElMessage.error('加载结果失败: ' + (error.message || '未知错误'))
  } finally {
    resultLoadingId.value = ''
  }
}

const deleteTask = async (row) => {
  const taskId = row.id || row.task_id
  if (!taskId) return
  try {
    await ElMessageBox.confirm(`确认删除任务 ${taskId}？`, '删除任务', {
      type: 'warning',
      confirmButtonText: '删除',
      cancelButtonText: '取消'
    })
    await api.deleteTask(taskId)
    ElMessage.success('任务已删除')
    await loadTasks()
  } catch (error) {
    if (error === 'cancel') return
    const detail = error.response?.data?.error || error.message || '未知错误'
    ElMessage.error('删除任务失败: ' + detail)
  }
}

onMounted(() => {
  loadTasks()
  pollTimer = setInterval(loadTasks, 3000)
})

onUnmounted(() => {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
})
</script>

<style scoped>
.tasks {
}

.page-title {
  font-size: 24px;
  font-weight: 600;
  margin-bottom: 20px;
  color: var(--text-primary);
}

.sql-editor-card {
  margin-bottom: 20px;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.header-actions {
  display: flex;
  align-items: center;
  gap: 8px;
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
