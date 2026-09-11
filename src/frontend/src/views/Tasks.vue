<template>
  <div class="tasks">
    <h1 class="page-title">SQL 工作台</h1>

    <!-- SQL 编辑器 -->
    <el-card class="sql-editor-card">
      <template #header>
        <div class="card-header">
          <span>SQL 编辑器</span>
          <div class="header-actions">
            <el-button @click="fillStreamDemoSql">流式示例 SQL</el-button>
            <el-tag v-if="isMultiSql" type="warning">多 SQL（仅异步）</el-tag>
            <el-tag v-else-if="sqlTaskKind === 'stream'" type="warning">流式 SQL（仅异步）</el-tag>
            <el-tag v-else-if="sqlTaskKind === 'mixed'" type="warning">混合 SQL（仅异步）</el-tag>
            <el-tag v-else-if="sqlTaskKind === 'batch'" type="info">批任务 SQL</el-tag>
            <el-radio-group v-model="executeMode" size="small">
              <el-radio-button v-if="allowSyncMode" label="sync">同步</el-radio-button>
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
        placeholder="多SQL采用分号(;)分隔"
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
        <el-table-column prop="task_kind" label="任务类型" width="110">
          <template #default="scope">
            <el-tag :type="taskKindTag(scope.row.task_kind)">
              {{ formatTaskKind(scope.row.task_kind) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag :type="taskStatusTag(scope.row.status)">
              {{ scope.row.status }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="created_at" label="创建时间" width="180" />
        <el-table-column label="操作" width="360">
          <template #default="scope">
              <el-button
                type="primary"
                size="small"
                :disabled="!isTerminal(scope.row.status)"
                :loading="resultLoadingId === (scope.row.id || scope.row.task_id)"
                @click="viewResult(scope.row)"
              >
                查看结果
              </el-button>
              <el-button
                type="warning"
                size="small"
                :disabled="isTerminal(scope.row.status)"
                @click="cancelTaskAction(scope.row)"
              >
                取消
              </el-button>
              <el-button
                size="small"
                :disabled="!canOpenRuntimeGraph(scope.row)"
                @click="openRuntimeGraph(scope.row)"
              >
                可视化
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
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { CaretRight, Refresh } from '@element-plus/icons-vue'
import { useRouter } from 'vue-router'
import api from '../api'
import { formatTaskResult } from '../utils/taskResult.js'
import { ElMessage, ElMessageBox } from 'element-plus'

const router = useRouter()
const sqlText = ref('')
const executeMode = ref('sync')
const executing = ref(false)
const sqlTaskKind = ref('unknown')
const currentResult = ref(null)
const tasks = ref([])
const loading = ref(false)
const resultDialogVisible = ref(false)
const dialogResult = ref(null)
const resultLoadingId = ref('')
const currentTaskId = ref('')
let pollTimer = null
let analyzeTimer = null
let analyzeSeq = 0
const sqlAnalyze = ref({
  statement_count: 0,
  statements: [],
  statement_kinds: [],
  task_kind: 'unknown'
})
const POLL_INTERVAL_MS = 2000
const STREAM_DEMO_SQL = `SELECT * FROM tcp_session_mock.tcp_src
USING builtin.tcp_service_merge_stream
INTO dataframe.serviceaccess`

const resetSqlAnalyze = () => {
  sqlAnalyze.value = {
    statement_count: 0,
    statements: [],
    statement_kinds: [],
    task_kind: 'unknown'
  }
}

const normalizeAnalyzePayload = (payload) => {
  const statements = Array.isArray(payload?.statements) ? payload.statements.filter(it => typeof it === 'string') : []
  const statementKinds = Array.isArray(payload?.statement_kinds)
    ? payload.statement_kinds.filter(it => typeof it === 'string')
    : []
  const statementCount = Number.isInteger(payload?.statement_count)
    ? payload.statement_count
    : statements.length
  const taskKind = typeof payload?.task_kind === 'string' ? payload.task_kind : 'unknown'
  return {
    statement_count: statementCount,
    statements,
    statement_kinds: statementKinds,
    task_kind: taskKind
  }
}

const buildStreamGroupPayload = (sqls) => {
  const sqlText = sqls.join(';\n') + ';'
  return {
    execution_kind: 'group',
    group_mode: 'dag',
    timeout_s: 0,
    sql_text: sqlText
  }
}

const parseApiError = (error) => {
  const payload = error?.response?.data || {}
  const code = payload.error_code || ''
  const raw = payload.error || error?.message || '未知错误'
    const codeTips = {
      STREAM_GROUP_SQL_TEXT_INVALID: 'SQL 文本格式错误：多 SQL 请使用分号分隔，且 single/group 入参需符合契约',
      STREAM_GROUP_TIMEOUT: '流式组任务执行超时',
      STREAM_GROUP_SHARE_SET_READY_TIMEOUT: '同源分支就绪超时，请检查上游数据与节点启动状态',
      STREAM_GROUP_DAG_INVALID: 'DAG 构建失败，请检查 SQL 之间的依赖关系',
      STREAM_GROUP_DAG_CYCLE_DETECTED: 'DAG 存在环路依赖，请调整 SQL 拓扑',
      STREAM_GROUP_NODE_KIND_INVALID: '节点类型判定失败，请检查 SQL 的 source/sink 类型',
      STREAM_GROUP_NODE_EXECUTION_FAILED: '节点执行失败，请查看节点 phase 与错误详情',
      STREAM_GROUP_NON_STREAM_SINK_MULTI_WRITER: '非 stream sink 当前仅支持单写入者，请调整 DAG 写入拓扑',
      STREAM_GROUP_SINK_CAPABILITY_MISMATCH: '共享 sink 并发写能力不足，请检查 sink 通道并发模式',
      STREAM_CHANNEL_VERSION_CHANGED: '通道配置在执行前被并发修改，请重试',
      STREAM_CHANNEL_MUTATING: '通道正在被修改，请稍后重试',
      STREAM_SOURCE_IN_USE: 'source 通道正在被其他任务占用'
  }
  if (!code) return raw
  const tip = codeTips[code]
  if (!tip) return `${raw} [${code}]`
  if (raw && raw !== tip) return `${tip}（${raw}） [${code}]`
  return `${tip} [${code}]`
}

const isStreamSql = computed(() => sqlTaskKind.value === 'stream')
const sqlStatements = computed(() => {
  const list = sqlAnalyze.value?.statements
  return Array.isArray(list) ? list : []
})
const isMultiSql = computed(() => sqlStatements.value.length > 1)
const allowSyncMode = computed(() => !isStreamSql.value && !isMultiSql.value)

const isTerminal = (status) => ['completed', 'failed', 'stopped', 'cancelled', 'timeout'].includes(status)

const taskStatusTag = (status) => {
  if (status === 'completed' || status === 'stopped') return 'success'
  if (status === 'running' || status === 'pending') return 'warning'
  return 'danger'
}

const formatTaskKind = (kind) => {
  if (kind === 'stream') return 'stream'
  if (kind === 'batch') return 'batch'
  return kind || 'unknown'
}

const taskKindTag = (kind) => {
  if (kind === 'stream') return 'warning'
  if (kind === 'batch') return 'info'
  return ''
}

const canOpenRuntimeGraph = (row) => {
  const taskId = row?.id || row?.task_id
  return Boolean(taskId)
}

const openRuntimeGraph = (row) => {
  const taskId = row?.id || row?.task_id
  if (!taskId) return
  router.push({ name: 'TaskRuntime', params: { taskId: String(taskId) } })
}

const stopPolling = () => {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

const startPolling = () => {
  if (pollTimer) return
  pollTimer = setInterval(loadTasks, POLL_INTERVAL_MS)
}

const applySqlTaskKind = (kind) => {
  if (kind === 'stream') {
    sqlTaskKind.value = 'stream'
    executeMode.value = 'async'
    return
  }
  if (kind === 'batch') {
    sqlTaskKind.value = 'batch'
    return
  }
  sqlTaskKind.value = 'unknown'
}

const analyzeCurrentSql = async ({ silent = false } = {}) => {
  const sql = sqlText.value.trim()
  const seq = ++analyzeSeq
  if (!sql) {
    resetSqlAnalyze()
    applySqlTaskKind('unknown')
    return normalizeAnalyzePayload(null)
  }

  try {
    const res = await api.analyzeSql(sql)
    if (seq !== analyzeSeq) return sqlAnalyze.value
    const normalized = normalizeAnalyzePayload(res?.data)
    sqlAnalyze.value = normalized
    if (normalized.statement_count > 1) {
      executeMode.value = 'async'
    }
    if (normalized.task_kind === 'batch' || normalized.task_kind === 'stream') {
      applySqlTaskKind(normalized.task_kind)
    } else {
      applySqlTaskKind('unknown')
      if (normalized.statement_count > 1) {
        executeMode.value = 'async'
      }
    }
    return normalized
  } catch (error) {
    if (seq === analyzeSeq) {
      resetSqlAnalyze()
      applySqlTaskKind('unknown')
    }
    if (!silent) {
      const detail = parseApiError(error)
      ElMessage.error('SQL 分析失败: ' + detail)
    }
    throw error
  }
}

const scheduleSqlAnalyze = () => {
  if (analyzeTimer) {
    clearTimeout(analyzeTimer)
    analyzeTimer = null
  }
  analyzeTimer = setTimeout(() => {
    analyzeCurrentSql({ silent: true }).catch(() => {})
  }, 250)
}

const syncActiveStreamTaskStatuses = async (rows) => {
  const streamRows = rows.filter((row) => {
    const kind = row.task_kind || ''
    const taskId = row.id || row.task_id
    return kind === 'stream' && taskId && !isTerminal(row.status)
  })
  if (streamRows.length === 0) return rows

  const updates = await Promise.all(streamRows.map(async (row) => {
    const taskId = row.id || row.task_id
    try {
      const res = await api.getStreamTaskStatus(taskId)
      const status = res?.data?.status
      if (!status) return null
      return {
        taskId,
        status,
        runtime_status: res?.data?.runtime_status,
        runtime_kind: res?.data?.runtime_kind || row.runtime_kind || '',
        group_mode: res?.data?.group_mode || row.group_mode || '',
        error_code: String(res?.data?.error_code ?? row.error_code ?? ''),
        error_message: res?.data?.error_message || row.error_message || ''
      }
    } catch {
      return null
    }
  }))

  const updateMap = new Map()
  updates.forEach((it) => {
    if (it && it.taskId) updateMap.set(it.taskId, it)
  })
  if (updateMap.size === 0) return rows

  return rows.map((row) => {
    const taskId = row.id || row.task_id
    const next = updateMap.get(taskId)
    if (!next) return row
    return {
      ...row,
      status: next.status,
      runtime_status: next.runtime_status,
      runtime_kind: next.runtime_kind,
      group_mode: next.group_mode,
      error_code: next.error_code,
      error_message: next.error_message
    }
  })
}

const executeSQL = async () => {
  const sql = sqlText.value.trim()
  if (!sql) {
    ElMessage.warning('请输入 SQL 语句')
    return
  }

  executing.value = true
  currentResult.value = null

  try {
    if (analyzeTimer) {
      clearTimeout(analyzeTimer)
      analyzeTimer = null
    }
    const analysis = await analyzeCurrentSql()
    const sqls = Array.isArray(analysis?.statements) ? analysis.statements : []
    if (sqls.length === 0) {
      throw new Error('SQL 文本为空或无法解析')
    }
    if (sqls.length > 1) {
      executeMode.value = 'async'
      if (analysis.task_kind === 'stream' || analysis.task_kind === 'mixed') {
        const payload = buildStreamGroupPayload(sqls)
        const streamRes = await api.executeStreamTask(payload)
        const submit = streamRes.data || {}
        const taskId = submit.task_id
        currentTaskId.value = taskId
        const nodeCount = submit.node_count || sqls.length
        const groupKindLabel = analysis.task_kind === 'mixed' ? '混合组任务' : '流式组任务'
        ElMessage.success(`${groupKindLabel}已提交 (ID: ${taskId})`)
        currentResult.value = {
          columns: [],
          rows: [],
          message: `${groupKindLabel} ${taskId} 已提交，节点数 ${nodeCount}`
        }
        await loadTasks()
        startPolling()
        return
      }
      if (analysis.task_kind === 'batch') {
        const res = await api.executeBatchTask(sql, executeMode.value)
        const submit = res.data || {}
        const taskId = submit.task_id
        currentTaskId.value = taskId
        ElMessage.success(`批任务已提交 (ID: ${taskId})`)
        currentResult.value = {
          columns: [],
          rows: [],
          message: `批任务 ${taskId} 已提交，正在异步执行`
        }
        await loadTasks()
        startPolling()
        return
      }
      throw new Error('多 SQL 暂不支持 batch 与 stream 混合执行')
    }
    if (analysis.task_kind === 'stream') {
      const streamRes = await api.executeStreamTask({
        execution_kind: 'single',
        sql_text: sqls[0],
        timeout_s: 0
      })
      const submit = streamRes.data || {}
      const taskId = submit.task_id
      currentTaskId.value = taskId
      ElMessage.success(`流式任务已提交 (ID: ${taskId})`)
      currentResult.value = {
        columns: [],
        rows: [],
        message: `流式任务 ${taskId} 已提交，正在异步执行`
      }
      await loadTasks()
      startPolling()
      return
    }
    if (analysis.task_kind !== 'batch') {
      throw new Error('无法判定 SQL 类型，请检查语句')
    }

    const res = await api.executeBatchTask(sqls[0], executeMode.value)
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
    startPolling()
  } catch (error) {
    const detail = parseApiError(error)
    ElMessage.error('执行失败: ' + detail)
    currentResult.value = { error: detail }
  } finally {
    executing.value = false
  }
}

const fillStreamDemoSql = () => {
  sqlText.value = STREAM_DEMO_SQL
}

const loadTasks = async () => {
  loading.value = true
  try {
    const res = await api.getTasks()
    const payload = res.data || {}
    const rawTasks = Array.isArray(payload) ? payload : (payload.items || [])
    tasks.value = await syncActiveStreamTaskStatuses(rawTasks)
    const hasActiveTask = tasks.value.some(t => !isTerminal(t.status))
    if (hasActiveTask) {
      startPolling()
    } else {
      stopPolling()
    }
    if (currentTaskId.value) {
      const current = tasks.value.find(t => (t.id || t.task_id) === currentTaskId.value)
      if (current && isTerminal(current.status)) {
        if ((current.task_kind || '') === 'stream') {
          const statusRes = await api.getStreamTaskStatus(currentTaskId.value)
          const statusPayload = statusRes?.data || {}
          const runtimeKind = statusPayload.runtime_kind || 'single'
          const groupMode = statusPayload.group_mode || ''
          const runtimeStatus = statusPayload.runtime_status || statusPayload.status || 'unknown'
          const nodeCount = Array.isArray(statusPayload.nodes) ? statusPayload.nodes.length : 0
          const shareSetCount = Array.isArray(statusPayload.share_sets) ? statusPayload.share_sets.length : 0
          const suffix = runtimeKind === 'group'
            ? `，group_mode=${groupMode || 'dag'}，nodes=${nodeCount}，share_sets=${shareSetCount}`
            : ''
          currentResult.value = {
            columns: [],
            rows: [],
            message: `流任务已结束（status=${runtimeStatus}${suffix}）`
          }
        } else {
          const resultRes = await api.getTaskResult(currentTaskId.value)
          currentResult.value = formatTaskResult(resultRes.data)
        }
        currentTaskId.value = ''
      }
    }
  } catch (error) {
    ElMessage.error('加载任务列表失败: ' + (error.message || '未知错误'))
  } finally {
    loading.value = false
  }
}

const viewResult = async (rowOrTaskId) => {
  const row = (rowOrTaskId && typeof rowOrTaskId === 'object') ? rowOrTaskId : null
  const taskId = row
    ? (row.id || row.task_id)
    : rowOrTaskId
  const isStreamTask = row
    ? ((row.task_kind || '') === 'stream')
    : false

  resultLoadingId.value = String(taskId || '')
  try {
    if (isStreamTask) {
      const res = await api.getStreamTaskStatus(taskId)
      const payload = res?.data || {}
        const runtimeKind = payload.runtime_kind || 'single'
        if (runtimeKind === 'group') {
          const nodes = Array.isArray(payload.nodes) ? payload.nodes : []
          const rows = nodes.map((n) => ({
            node_id: n.id || n.node_id || '',
            node_kind: n.node_kind || '',
            sql_index: Number.isInteger(n.sql_index) ? n.sql_index : '',
            status: n.status || '',
            phase: n.phase || '',
            start_condition: n.start_condition || '',
            depends_on: Array.isArray(n.depends_on) ? n.depends_on.join(',') : '',
            processed_rows: n.processed_rows ?? 0,
            output_rows: n.output_rows ?? 0,
            error_code: n.error_code || '',
            error_message: (() => {
              if (typeof n.error_message === 'string' && n.error_message) return n.error_message
              if (typeof n.last_error === 'string' && n.last_error) return n.last_error
              if (n.last_error && typeof n.last_error === 'object') {
                if (typeof n.last_error.error_message === 'string' && n.last_error.error_message) {
                  return n.last_error.error_message
                }
                if (typeof n.last_error.message === 'string' && n.last_error.message) {
                  return n.last_error.message
                }
              }
              return ''
            })()
          }))
          const shareSetCount = Array.isArray(payload.share_sets) ? payload.share_sets.length : 0
          const resolvedCount = Array.isArray(payload.resolved_sources) ? payload.resolved_sources.length : 0
          dialogResult.value = rows.length > 0
            ? {
              columns: ['node_id', 'node_kind', 'sql_index', 'status', 'phase', 'start_condition', 'depends_on', 'processed_rows', 'output_rows', 'error_code', 'error_message'],
              rows
            }
          : {
              columns: [],
              rows: [],
              message: `Group 状态：${payload.status || 'unknown'}，group_mode=${payload.group_mode || 'dag'}，share_sets=${shareSetCount}，resolved_sources=${resolvedCount}`
            }
      } else {
        const sharedHubId = typeof payload.shared_hub_id === 'string' ? payload.shared_hub_id : ''
        const sharedSourceKeys = Array.isArray(payload.shared_source_keys)
          ? payload.shared_source_keys.filter(it => typeof it === 'string')
          : []
        const subscriberStats = Array.isArray(payload.subscriber_stats) ? payload.subscriber_stats : []
        if (subscriberStats.length > 0) {
          const rows = subscriberStats.map((s) => ({
            shared_hub_id: sharedHubId || '-',
            shared_source_keys: sharedSourceKeys.join(',') || '-',
            subscriber_id: s?.subscriber_id || '',
            runtime_task_id: s?.runtime_task_id || '',
            active: Boolean(s?.active),
            ready: Boolean(s?.ready),
            delivered_batches: Number(s?.delivered_batches ?? 0),
            dropped_batches: Number(s?.dropped_batches ?? 0),
            lag: Number(s?.lag ?? 0)
          }))
          dialogResult.value = {
            columns: ['shared_hub_id', 'shared_source_keys', 'subscriber_id', 'runtime_task_id', 'active', 'ready', 'delivered_batches', 'dropped_batches', 'lag'],
            rows
          }
        } else {
          const message = `流任务状态：${payload.status || 'unknown'}，processed_rows=${payload.processed_rows ?? 0}，output_rows=${payload.output_rows ?? 0}，hub=${sharedHubId || '-'}`
          dialogResult.value = {
            columns: [],
            rows: [],
            message
          }
        }
      }
    } else {
      const res = await api.getTaskResult(taskId)
      const result = res.data
      dialogResult.value = formatTaskResult(result)
    }

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

const cancelTaskAction = async (row) => {
  const taskId = row.id || row.task_id
  if (!taskId) return
  try {
    if ((row.task_kind || '') === 'stream') {
      await api.stopStreamTask(taskId)
      ElMessage.success('已停止流式任务')
    } else {
      await api.cancelTask(taskId)
      ElMessage.success('已发出取消请求')
    }
    await loadTasks()
  } catch (error) {
    const detail = error.response?.data?.error || error.message || '未知错误'
    ElMessage.error('取消任务失败: ' + detail)
  }
}

onMounted(() => {
  loadTasks()
})

watch(sqlText, () => {
  scheduleSqlAnalyze()
})

onUnmounted(() => {
  stopPolling()
  if (analyzeTimer) {
    clearTimeout(analyzeTimer)
    analyzeTimer = null
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
