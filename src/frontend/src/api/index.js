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

export default {
  // WebPlugin 直接处理的路由
  health: () => api.get('/api/health'),
  getChannels: () => api.get('/api/channels'),
  getOperators: () => api.get('/api/operators'),
  uploadOperator: (filename, content) => api.post('/api/operators/upload', { filename, content }),
  activateOperator: (name) => api.post('/api/operators/activate', { name }),
  deactivateOperator: (name) => api.post('/api/operators/deactivate', { name }),
  getTasks: () => api.get('/api/tasks'),
  createTask: (sql) => api.post('/api/tasks', { sql }),
  getTaskResult: (id) => api.post('/api/tasks/result', { task_id: id }),

  // 数据库通道管理（WebPlugin 内部转发给 DatabasePlugin）
  listDbChannels: () => api.post('/api/channels/database/query', {}),
  addDbChannel: (config) => api.post('/api/channels/database/add', { config }),
  removeDbChannel: (type, name) => api.post('/api/channels/database/remove', { type, name }),
  updateDbChannel: (config) => api.post('/api/channels/database/modify', { config }),

  // 数据库通道浏览器
  listDbTables:    (type, name)        => api.post('/api/channels/database/tables',   { type, name }),
  describeDbTable: (type, name, table) => api.post('/api/channels/database/describe', { type, name, table }),
  previewDbTable:  (type, name, table) => api.post('/api/channels/database/preview',  { type, name, table }),

  // dataframe 通道预览
  previewDataframe: (catelog, name) => api.post('/api/channels/dataframe/preview', { catelog, name }),
}
