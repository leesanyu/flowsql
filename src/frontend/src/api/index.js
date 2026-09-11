import axios from 'axios'

const api = axios.create({
  baseURL: 'http://localhost:8081',  // WebPlugin 对外端口，统一入口
  timeout: 30000
})

// 响应拦截器：统一错误处理
api.interceptors.response.use(
  response => response,
  error => {
    console.error('API Error:', error)
    return Promise.reject(error)
  }
)

const unwrapList = (payload, keys = []) => {
  if (Array.isArray(payload)) return payload
  for (const key of keys) {
    if (Array.isArray(payload?.[key])) return payload[key]
  }
  return []
}

export const PCAP_UPLOAD_DEFAULTS = Object.freeze({
  format: 'auto',
  batch_packets: 256,
  replay_mode: 'fast',
  replay_speed: 1
})

export const PCAP_REPLAY_SPEEDS = Object.freeze([0.001, 0.01, 0.1, 1, 10, 100, 1000])

export const replaySpeedToMilli = (speed) => Math.round(Number(speed) * 1000)

export const PCAP_UPLOAD_PATH = '/api/channels/pcapfile/upload'
export const PCAP_UPLOAD_REQUEST_CONFIG = Object.freeze({ timeout: 0 })

export const isSupportedPcapFile = (file) =>
  typeof file?.name === 'string' && /^.+\.(pcap|pcapng)$/i.test(file.name)

export const buildPcapUploadFormData = (file, fields = {}, createFormData = () => new FormData()) => {
  const values = { ...PCAP_UPLOAD_DEFAULTS, ...fields }
  const form = createFormData()
  form.append('name', String(values.name ?? '').trim())
  form.append('format', String(values.format))
  form.append('batch_packets', String(values.batch_packets))
  form.append('replay_mode', String(values.replay_mode))
  form.append('replay_speed_milli', String(replaySpeedToMilli(values.replay_speed)))
  form.append('file', file)
  return form
}

const pcapUploadErrorMessages = Object.freeze({
  invalid_request: '上传参数或文件无效',
  channel_conflict: '通道名称已存在',
  file_too_large: '文件超过上传大小限制',
  provider_unavailable: 'PCAP 服务暂不可用',
  storage_failure: '服务器保存文件失败',
  internal_error: '服务器内部错误'
})

export const pcapUploadErrorMessage = (error) => {
  const code = error?.response?.data?.error
  if (typeof code === 'string' && pcapUploadErrorMessages[code]) {
    return pcapUploadErrorMessages[code]
  }
  if (typeof code === 'string' && code) return `PCAP 上传失败（${code}）`
  if (error?.response?.status) return `PCAP 上传失败（HTTP ${error.response.status}）`
  return '无法连接 PCAP 上传服务'
}

export const isPacketRawPreviewValue = (value) =>
  value !== null &&
  typeof value === 'object' &&
  !Array.isArray(value) &&
  typeof value.hex === 'string' &&
  Number.isSafeInteger(value.byte_length) &&
  value.byte_length >= 0 &&
  typeof value.truncated === 'boolean'

export const formatPreviewCell = (value) => {
  if (value === null || value === undefined) return 'NULL'
  if (isPacketRawPreviewValue(value)) {
    if (value.byte_length === 0) return '（0 B）'
    const truncated = value.truncated ? '…' : ''
    const detail = value.truncated ? `${value.byte_length} B，已截断` : `${value.byte_length} B`
    return `${value.hex}${truncated}（${detail}）`
  }
  if (typeof value === 'object') return JSON.stringify(value)
  return String(value)
}

export default {
  // WebPlugin 直接处理的路由
  health: () => api.get('/api/health'),
  getChannels: () => api.get('/api/channels/list').then((res) => ({
    ...res,
    data: unwrapList(res?.data, ['channels'])
  })),
  getOperators: (type = 'python') => api.get('/api/operators/list', { params: { type } }).then((res) => {
    return { ...res, data: unwrapList(res?.data, ['operators']) }
  }),
  uploadOperator: (filename, content, type = 'python') => api.post('/api/operators/upload', { type, filename, content }),
  uploadOperatorFile: (file, type = 'python') => {
    const form = new FormData()
    form.append('file', file)
    form.append('type', type)
    return api.post('/api/operators/upload', form, {
      headers: { 'Content-Type': 'multipart/form-data' }
    })
  },
  activateOperator: (nameOrPayload, type = 'python') => {
    const body = (nameOrPayload && typeof nameOrPayload === 'object')
      ? nameOrPayload
      : { type, name: nameOrPayload }
    return api.post('/api/operators/activate', body)
  },
  deactivateOperator: (nameOrPayload, type = 'python') => {
    const body = (nameOrPayload && typeof nameOrPayload === 'object')
      ? nameOrPayload
      : { type, name: nameOrPayload }
    return api.post('/api/operators/deactivate', body)
  },
  deleteOperator: (payload) => api.post('/api/operators/delete', payload),
  getOperatorDetail: (nameOrPayload, type = 'python') => {
    const body = (nameOrPayload && typeof nameOrPayload === 'object')
      ? nameOrPayload
      : { type, name: nameOrPayload }
    return api.post('/api/operators/detail', body)
  },
  updateOperator: (name, payload) => api.post('/api/operators/update', { name, ...payload }),
  getTasks: (params = {}) => api.post('/api/tasks/list', params).then((res) => {
    const payload = res?.data
    const items = unwrapList(payload, ['items', 'tasks', 'data'])
    const total = Number.isFinite(payload?.total) ? payload.total : items.length
    return {
      ...res,
      data: items,
      total
    }
  }),
  executeBatchTask: (sqlText, mode = 'async') => api.post('/api/tasks/batch/execute', { sql_text: sqlText, mode }),
  classifySql: (sql) => api.post('/api/tasks/sql/classify', { sql }),
  analyzeSql: (sqlText) => api.post('/api/tasks/sql/analyze', { sql_text: sqlText }),
  getTaskResult: (id) => api.post('/api/tasks/result', { task_id: id }),
  deleteTask: (id) => api.post('/api/tasks/delete', { task_id: id }),
  cancelTask: (id) => api.post('/api/tasks/cancel', { task_id: id }),
  getTaskDiagnostics: (id) => api.post('/api/tasks/diagnostics', { task_id: id }),
  executeStreamTask: (sqlTextOrPayload, timeout_s = 0) => {
    let payload
    if (typeof sqlTextOrPayload === 'string') {
      payload = { execution_kind: 'single', sql_text: sqlTextOrPayload, timeout_s }
    } else {
      payload = (sqlTextOrPayload && typeof sqlTextOrPayload === 'object')
        ? { ...sqlTextOrPayload }
        : { execution_kind: 'single', sql_text: '', timeout_s }
      if (!payload.execution_kind) payload.execution_kind = 'single'
      if (typeof timeout_s === 'number' && payload.timeout_s === undefined) {
        payload.timeout_s = timeout_s
      }
    }
    return api.post('/api/tasks/stream/execute', payload)
  },
  stopStreamTask: (taskId) => api.post('/api/tasks/stream/stop', { task_id: taskId }),
  getStreamTaskStatus: (taskId) => api.post('/api/tasks/stream/status', { task_id: taskId }),
  listStreamTasks: (params = {}) => api.post('/api/tasks/stream/list', params),
  getTaskRuntimeGraph: (taskId, cursor = 0, includeEvents = true) =>
    api.post('/api/tasks/runtime/graph/query', {
      task_id: taskId,
      cursor,
      include_events: includeEvents
    }),

  // 数据库通道管理（WebPlugin 内部转发给 DatabasePlugin）
  listDbChannels: () => api.post('/api/channels/database/query', {}),
  addDbChannel: (config) => api.post('/api/channels/database/add', { config }),
  removeDbChannel: (type, name) => api.post('/api/channels/database/remove', { type, name }),
  updateDbChannel: (config) => api.post('/api/channels/database/modify', { config }),

  // 数据库通道浏览器
  listDbTables:    (type, name)        => api.post('/api/channels/database/tables',   { type, name }),
  describeDbTable: (type, name, table) => api.post('/api/channels/database/describe', { type, name, table }),
  previewDbTable:  (type, name, table) => api.post('/api/channels/database/preview',  { type, name, table }),

  // dataframe 通道管理
  listDfChannels: () => api.get('/api/channels/dataframe'),
  listStreamChannels: () => api.post('/api/channels/stream/query', {}),
  getStreamDefinitions: () => api.post('/api/channels/stream/definitions/query', {}),
  addStreamChannel: (payload) => api.post('/api/channels/stream/add', payload),
  modifyStreamChannel: (payload) => api.post('/api/channels/stream/modify', payload),
  resetStreamChannel: (type, name) => api.post('/api/channels/stream/reset', { type, name }),
  removeStreamChannel: (type, name) => api.post('/api/channels/stream/remove', { type, name }),
  uploadPcap: (file, fields) =>
    api.post(PCAP_UPLOAD_PATH, buildPcapUploadFormData(file, fields), PCAP_UPLOAD_REQUEST_CONFIG),
  importCsv: (file) => {
    const form = new FormData()
    form.append('file', file)
    return api.post('/api/channels/dataframe/import', form, {
      headers: { 'Content-Type': 'multipart/form-data' }
    })
  },
  previewDfChannel: (name, page = 1, pageSize = 20) =>
    api.post('/api/channels/dataframe/preview', { category: 'dataframe', name, page, page_size: pageSize }),
  renameDfChannel: (name, newName) => api.post('/api/channels/dataframe/rename', { name, new_name: newName }),
  deleteDfChannel: (name) => api.post('/api/channels/dataframe/delete', { name }),
}
