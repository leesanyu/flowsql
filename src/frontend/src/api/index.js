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
  createTask: (sql, mode = 'async') => api.post('/api/tasks/submit', { sql, mode }),
  getTaskResult: (id) => api.post('/api/tasks/result', { task_id: id }),
  deleteTask: (id) => api.post('/api/tasks/delete', { task_id: id }),
  cancelTask: (id) => api.post('/api/tasks/cancel', { task_id: id }),
  getTaskDiagnostics: (id) => api.post('/api/tasks/diagnostics', { task_id: id }),

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
