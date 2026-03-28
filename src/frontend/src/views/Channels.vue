<template>
  <div class="channels">
    <h1 class="page-title">通道列表</h1>

    <el-card>
      <template #header>
        <div class="card-header">
          <div class="header-left">
            <el-input
              v-model="searchText"
              placeholder="搜索通道名称"
              style="width: 300px"
              clearable
            >
              <template #prefix>
                <el-icon><Search /></el-icon>
              </template>
            </el-input>
          </div>
          <div class="header-actions">
            <input ref="csvInput" type="file" accept=".csv" style="display:none" @change="handleCsvSelected" />
            <el-button v-if="activeChannelType === 'dataframe'" type="success" @click="triggerCsvUpload">导入 CSV</el-button>
            <el-button v-if="activeChannelType === 'database'" type="primary" @click="showAddDialog = true">新增数据库通道</el-button>
          </div>
        </div>
      </template>
      <div class="channel-layout">
        <div class="channel-sidebar">
          <div
            class="channel-type-item"
            :class="{ active: activeChannelType === 'dataframe' }"
            @click="activeChannelType = 'dataframe'"
          >
            <span>DataFrame 通道</span>
            <el-tag size="small" effect="plain">{{ dfChannels.length }}</el-tag>
          </div>
          <div
            class="channel-type-item"
            :class="{ active: activeChannelType === 'database' }"
            @click="activeChannelType = 'database'"
          >
            <span>数据库通道</span>
            <el-tag size="small" effect="plain">{{ dbChannels.length }}</el-tag>
          </div>
        </div>

        <div class="channel-main">
          <div class="section-title">{{ activeChannelType === 'dataframe' ? 'DataFrame 通道' : '数据库通道' }}</div>

          <el-table v-if="activeChannelType === 'dataframe'" :data="filteredDfChannels" style="width: 100%" v-loading="loadingDf">
            <el-table-column prop="name" label="名称" min-width="220" />
            <el-table-column prop="rows" label="行数" width="120" />
            <el-table-column label="Schema" min-width="260">
              <template #default="scope">
                <el-popover placement="top" :width="420" trigger="hover">
                  <template #reference>
                    <el-tag>{{ scope.row.schema.length }} 字段</el-tag>
                  </template>
                  <div class="schema-popup">
                    <div v-for="field in scope.row.schema" :key="field.name" class="schema-field">
                      <span class="field-name">{{ field.name }}</span>
                      <span class="field-type">{{ getTypeName(field.type) }}</span>
                    </div>
                  </div>
                </el-popover>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="260">
              <template #default="scope">
                <el-button type="primary" size="small" text @click="previewDf(scope.row)">预览</el-button>
                <el-button type="warning" size="small" text @click="renameDf(scope.row)">重命名</el-button>
                <el-button type="danger" size="small" text @click="removeDf(scope.row)">删除</el-button>
              </template>
            </el-table-column>
          </el-table>

          <el-table v-else :data="filteredDbChannels" style="width: 100%" v-loading="loadingDb">
            <el-table-column prop="name" label="名称" width="180" />
            <el-table-column prop="type" label="类型" width="140" />
            <el-table-column prop="schema" label="Schema" min-width="200" />
            <el-table-column label="操作" width="160">
              <template #default="scope">
                <el-button type="primary" size="small" text @click="openDbBrowser(scope.row)">浏览</el-button>
                <el-button type="danger" size="small" text @click="handleRemoveDb(scope.row)">删除</el-button>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </div>
    </el-card>

    <el-drawer
      v-model="drawerVisible"
      :title="drawerTitle"
      size="60%"
      direction="rtl"
    >
      <div v-if="drawerMode === 'df'" class="browser-content" style="padding: 0">
        <el-table
          v-if="previewData.rows && previewData.rows.length > 0"
          :data="previewData.rows"
          size="small"
          border
          style="width: 100%"
          max-height="600"
          v-loading="previewLoading"
        >
          <el-table-column
            v-for="col in previewData.columns"
            :key="col"
            :prop="col"
            :label="col"
            min-width="100"
            show-overflow-tooltip
          />
        </el-table>
        <div v-else-if="previewLoading" v-loading="true" style="height: 100px" />
        <div v-else class="browser-empty">暂无数据</div>
        <div v-if="dfPreviewTotal > 0" class="df-pagination">
          <el-pagination
            background
            layout="total, sizes, prev, pager, next"
            :current-page="dfPreviewPage"
            :page-size="dfPreviewPageSize"
            :page-sizes="[10, 20, 50, 100]"
            :total="dfPreviewTotal"
            @current-change="onDfPageChange"
            @size-change="onDfPageSizeChange"
          />
        </div>
      </div>

      <div v-else class="browser-layout">
        <div class="browser-tables">
          <div v-if="tablesLoading" class="browser-loading">
            <el-icon class="is-loading"><Loading /></el-icon>
          </div>
          <div v-else-if="tableList.length === 0" class="browser-empty">暂无表</div>
          <div
            v-for="t in tableList"
            :key="t"
            class="browser-table-item"
            :class="{ active: t === selectedTable }"
            @click="selectTable(t)"
          >{{ t }}</div>
        </div>

        <div class="browser-content">
          <el-tabs v-model="activeTab" v-if="selectedTable">
            <el-tab-pane label="表结构" name="describe">
              <el-table
                v-if="describeData.rows && describeData.rows.length > 0"
                :data="describeData.rows"
                size="small"
                border
                style="width: 100%"
                v-loading="describeLoading"
              >
                <el-table-column
                  v-for="col in describeData.columns"
                  :key="col"
                  :prop="col"
                  :label="col"
                  min-width="100"
                  show-overflow-tooltip
                />
              </el-table>
              <div v-else-if="describeLoading" v-loading="true" style="height: 100px" />
            </el-tab-pane>
            <el-tab-pane label="数据预览" name="preview">
              <el-table
                v-if="previewData.rows && previewData.rows.length > 0"
                :data="previewData.rows"
                size="small"
                border
                style="width: 100%"
                max-height="500"
                v-loading="previewLoading"
              >
                <el-table-column
                  v-for="col in previewData.columns"
                  :key="col"
                  :prop="col"
                  :label="col"
                  min-width="100"
                  show-overflow-tooltip
                />
              </el-table>
              <div v-else-if="previewLoading" v-loading="true" style="height: 100px" />
            </el-tab-pane>
          </el-tabs>
          <div v-else class="browser-empty">请选择左侧表</div>
        </div>
      </div>
    </el-drawer>

    <el-dialog v-model="showAddDialog" title="新增数据库通道" width="500px" @close="resetForm">
      <el-form :model="form" label-width="100px" ref="formRef">
        <el-form-item label="类型" prop="type" required>
          <el-select v-model="form.type" placeholder="选择数据库类型" style="width:100%">
            <el-option label="SQLite" value="sqlite" />
            <el-option label="MySQL" value="mysql" />
            <el-option label="PostgreSQL" value="postgres" />
            <el-option label="ClickHouse" value="clickhouse" />
          </el-select>
        </el-form-item>
        <el-form-item label="名称" prop="name" required>
          <el-input v-model="form.name" placeholder="通道名称，如 mydb" />
        </el-form-item>

        <template v-if="form.type === 'sqlite'">
          <el-form-item label="文件路径">
            <el-input v-model="form.path" placeholder=":memory: 或 /data/test.db" />
          </el-form-item>
        </template>

        <template v-if="form.type === 'mysql' || form.type === 'postgres' || form.type === 'clickhouse'">
          <el-form-item label="主机">
            <el-input v-model="form.host" placeholder="127.0.0.1" />
          </el-form-item>
          <el-form-item label="端口">
            <el-input v-model="form.port" :placeholder="dbPortPlaceholder(form.type)" />
          </el-form-item>
          <el-form-item label="用户名">
            <el-input v-model="form.user" :placeholder="dbUserPlaceholder(form.type)" />
          </el-form-item>
          <el-form-item label="密码">
            <el-input v-model="form.password" type="password" show-password placeholder="留空表示无密码" />
          </el-form-item>
          <el-form-item label="数据库" prop="database" required>
            <el-input v-model="form.database" :placeholder="dbNamePlaceholder(form.type)" />
          </el-form-item>
        </template>
      </el-form>
      <template #footer>
        <el-button @click="showAddDialog = false">取消</el-button>
        <el-button type="primary" :loading="submitting" @click="handleAdd">确认添加</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { Search, Loading } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage, ElMessageBox } from 'element-plus'

const searchText = ref('')
const activeChannelType = ref('dataframe')
const showAddDialog = ref(false)
const submitting = ref(false)
const formRef = ref(null)
const csvInput = ref(null)

const dbChannels = ref([])
const dfChannels = ref([])
const loadingDb = ref(false)
const loadingDf = ref(false)

const form = ref({
  type: 'mysql', name: '', host: '127.0.0.1', port: '',
  user: '', password: '', database: '', path: ''
})

const filteredDbChannels = computed(() => {
  const s = searchText.value.trim().toLowerCase()
  if (!s) return dbChannels.value
  return dbChannels.value.filter(ch =>
    (ch.name || '').toLowerCase().includes(s) ||
    (ch.type || '').toLowerCase().includes(s)
  )
})

const filteredDfChannels = computed(() => {
  const s = searchText.value.trim().toLowerCase()
  if (!s) return dfChannels.value
  return dfChannels.value.filter(ch => (ch.name || '').toLowerCase().includes(s))
})

const getTypeName = (typeId) => {
  const typeMap = {
    0: 'INT32', 1: 'INT64', 2: 'UINT32', 3: 'UINT64', 4: 'FLOAT',
    5: 'DOUBLE', 6: 'STRING', 7: 'BYTES', 8: 'TIMESTAMP', 9: 'BOOLEAN'
  }
  return typeMap[typeId] || 'UNKNOWN'
}

const dbPortPlaceholder = (type) => {
  if (type === 'mysql') return '3306'
  if (type === 'postgres') return '5432'
  if (type === 'clickhouse') return '8123'
  return ''
}

const dbUserPlaceholder = (type) => {
  if (type === 'mysql') return 'root'
  if (type === 'postgres') return 'postgres'
  if (type === 'clickhouse') return 'default'
  return ''
}

const dbNamePlaceholder = (type) => {
  if (type === 'mysql') return 'mydb'
  if (type === 'postgres') return 'postgres'
  if (type === 'clickhouse') return 'default'
  return ''
}

const loadDbChannels = async () => {
  loadingDb.value = true
  try {
    const res = await api.listDbChannels()
    dbChannels.value = (res.data || []).map(ch => ({
      ...ch,
      schema: ch.database || ch.path || ''
    }))
  } catch (error) {
    ElMessage.error('加载数据库通道失败: ' + (error.response?.data?.error || error.message))
  } finally {
    loadingDb.value = false
  }
}

const loadDfChannels = async () => {
  loadingDf.value = true
  try {
    const res = await api.listDfChannels()
    dfChannels.value = (res.data.channels || []).map(ch => ({
      name: ch.name,
      rows: ch.rows || 0,
      schema: ch.schema || []
    }))
  } catch (error) {
    ElMessage.error('加载 DataFrame 通道失败: ' + (error.response?.data?.error || error.message))
  } finally {
    loadingDf.value = false
  }
}

const reloadAll = async () => {
  await Promise.all([loadDbChannels(), loadDfChannels()])
}

const triggerCsvUpload = () => {
  if (csvInput.value) csvInput.value.click()
}

const handleCsvSelected = async (e) => {
  const file = e.target.files?.[0]
  e.target.value = ''
  if (!file) return
  try {
    await api.importCsv(file)
    ElMessage.success('CSV 导入成功')
    await loadDfChannels()
  } catch (error) {
    ElMessage.error('CSV 导入失败: ' + (error.response?.data?.error || error.message))
  }
}

const renameDf = async (row) => {
  try {
    const { value } = await ElMessageBox.prompt('输入新通道名', `重命名 ${row.name}`, {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      inputValue: row.name,
      inputValidator: (v) => !!v || '名称不能为空'
    })
    if (!value || value === row.name) return
    await api.renameDfChannel(row.name, value)
    ElMessage.success('重命名成功')
    await loadDfChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('重命名失败: ' + (e.response?.data?.error || e.message))
  }
}

const removeDf = async (row) => {
  try {
    await ElMessageBox.confirm(`确认删除 DataFrame 通道 ${row.name}？`, '删除确认', { type: 'warning' })
    await api.deleteDfChannel(row.name)
    ElMessage.success('删除成功')
    await loadDfChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('删除失败: ' + (e.response?.data?.error || e.message))
  }
}

const buildConfigStr = () => {
  const f = form.value
  let s = `type=${f.type};name=${f.name}`
  if (f.type === 'sqlite') {
    if (f.path) s += `;path=${f.path}`
  } else {
    if (f.host) s += `;host=${f.host}`
    if (f.port) s += `;port=${f.port}`
    if (f.user) s += `;user=${f.user}`
    if (f.password) s += `;password=${f.password}`
    if (f.database) s += `;database=${f.database}`
  }
  return s
}

const handleAdd = async () => {
  if (!form.value.type || !form.value.name) {
    ElMessage.warning('类型和名称为必填项')
    return
  }
  if ((form.value.type === 'mysql' || form.value.type === 'postgres' || form.value.type === 'clickhouse') &&
      !form.value.database) {
    ElMessage.warning('MySQL/PostgreSQL/ClickHouse 通道必须填写数据库名称')
    return
  }
  submitting.value = true
  try {
    await api.addDbChannel(buildConfigStr())
    ElMessage.success('数据库通道添加成功')
    showAddDialog.value = false
    await loadDbChannels()
  } catch (error) {
    ElMessage.error('添加失败: ' + (error.response?.data?.error || error.message))
  } finally {
    submitting.value = false
  }
}

const handleRemoveDb = async (row) => {
  try {
    await ElMessageBox.confirm(`确认删除通道 ${row.type}.${row.name}？`, '删除确认', { type: 'warning' })
    await api.removeDbChannel(row.type, row.name)
    ElMessage.success('删除成功')
    await loadDbChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('删除失败: ' + (e.response?.data?.error || e.message))
  }
}

const resetForm = () => {
  form.value = { type: 'mysql', name: '', host: '127.0.0.1', port: '', user: '', password: '', database: '', path: '' }
}

const drawerVisible = ref(false)
const drawerMode = ref('db')
const drawerTitle = ref('')
const browserChannel = ref({ type: '', name: '' })
const tableList = ref([])
const tablesLoading = ref(false)
const selectedTable = ref('')
const activeTab = ref('describe')
const describeData = ref({ columns: [], rows: [] })
const describeLoading = ref(false)
const previewData = ref({ columns: [], rows: [] })
const previewLoading = ref(false)
const dfPreviewName = ref('')
const dfPreviewPage = ref(1)
const dfPreviewPageSize = ref(20)
const dfPreviewTotal = ref(0)

const toRows = (res) => {
  if (!res || !res.columns || !res.data) return []
  return res.data.map(row => {
    const obj = {}
    res.columns.forEach((col, i) => { obj[col] = row[i] })
    return obj
  })
}

const loadDfPreview = async () => {
  if (!dfPreviewName.value) return
  previewLoading.value = true
  try {
    const res = await api.previewDfChannel(dfPreviewName.value, dfPreviewPage.value, dfPreviewPageSize.value)
    previewData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
    dfPreviewTotal.value = Number(res.data.rows || 0)
  } catch (e) {
    ElMessage.error('获取预览失败: ' + (e.response?.data?.error || e.message))
  } finally {
    previewLoading.value = false
  }
}

const previewDf = async (row) => {
  drawerMode.value = 'df'
  drawerTitle.value = `预览：dataframe.${row.name}`
  previewData.value = { columns: [], rows: [] }
  dfPreviewName.value = row.name
  dfPreviewPage.value = 1
  dfPreviewPageSize.value = 20
  dfPreviewTotal.value = 0
  drawerVisible.value = true
  await loadDfPreview()
}

const onDfPageChange = async (p) => {
  dfPreviewPage.value = p
  await loadDfPreview()
}

const onDfPageSizeChange = async (s) => {
  dfPreviewPageSize.value = s
  dfPreviewPage.value = 1
  await loadDfPreview()
}

const openDbBrowser = async (row) => {
  drawerMode.value = 'db'
  browserChannel.value = { type: row.type, name: row.name }
  drawerTitle.value = `浏览：${row.type}.${row.name}`
  tableList.value = []
  selectedTable.value = ''
  describeData.value = { columns: [], rows: [] }
  previewData.value = { columns: [], rows: [] }
  drawerVisible.value = true

  tablesLoading.value = true
  try {
    const res = await api.listDbTables(row.type, row.name)
    tableList.value = res.data.tables || []
    if (tableList.value.length > 0) await selectTable(tableList.value[0])
  } catch (e) {
    ElMessage.error('获取表列表失败: ' + (e.response?.data?.error || e.message))
  } finally {
    tablesLoading.value = false
  }
}

const selectTable = async (table) => {
  selectedTable.value = table
  activeTab.value = 'describe'
  describeData.value = { columns: [], rows: [] }
  previewData.value = { columns: [], rows: [] }

  const { type, name } = browserChannel.value

  describeLoading.value = true
  previewLoading.value = true

  try {
    const res = await api.describeDbTable(type, name, table)
    describeData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
  } catch (e) {
    ElMessage.error('获取表结构失败: ' + (e.response?.data?.error || e.message))
  } finally {
    describeLoading.value = false
  }

  try {
    const res = await api.previewDbTable(type, name, table)
    previewData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
  } catch (e) {
    ElMessage.error('获取数据预览失败: ' + (e.response?.data?.error || e.message))
  } finally {
    previewLoading.value = false
  }
}

onMounted(() => { reloadAll() })
</script>

<style scoped>
.channels { }
.page-title { font-size: 24px; font-weight: 600; margin-bottom: 20px; color: var(--text-primary); }
.card-header { display: flex; justify-content: space-between; align-items: center; }
.header-actions { display: flex; gap: 10px; }
.channel-layout {
  display: flex;
  gap: 16px;
}
.channel-sidebar {
  width: 180px;
  flex-shrink: 0;
  border-right: 1px solid var(--border-color);
  padding-right: 12px;
}
.channel-type-item {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 10px 12px;
  border-radius: 8px;
  cursor: pointer;
  color: var(--text-primary);
  margin-bottom: 8px;
}
.channel-type-item:hover {
  background: var(--bg-secondary);
}
.channel-type-item.active {
  background: var(--sidebar-active-bg, #dbeafe);
  color: var(--accent);
  font-weight: 600;
}
.channel-main {
  flex: 1;
  min-width: 0;
}
.section-title { margin: 10px 0; font-weight: 600; color: var(--text-primary); }
.schema-popup { max-height: 300px; overflow-y: auto; }
.schema-field { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid #eee; }
.schema-field:last-child { border-bottom: none; }
.field-name { font-weight: 600; color: var(--text-primary); }
.field-type { color: var(--text-secondary); font-family: monospace; }

.browser-layout {
  display: flex;
  height: calc(100vh - 120px);
}

.browser-tables {
  width: 180px;
  flex-shrink: 0;
  border-right: 1px solid var(--border-color);
  overflow-y: auto;
  padding: 8px 0;
}

.browser-table-item {
  padding: 8px 16px;
  font-size: 13px;
  cursor: pointer;
  color: var(--text-primary);
  border-radius: 4px;
  margin: 2px 6px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.browser-table-item:hover { background-color: var(--bg-secondary); }

.browser-table-item.active {
  background-color: var(--sidebar-active-bg, #dbeafe);
  color: var(--accent);
  font-weight: 600;
}

.browser-content {
  flex: 1;
  overflow: auto;
  padding: 0 16px;
}

.browser-loading,
.browser-empty {
  padding: 20px 16px;
  font-size: 13px;
  color: var(--text-secondary);
  text-align: center;
}

.df-pagination {
  display: flex;
  justify-content: flex-end;
  padding: 10px 6px 0 6px;
}

@media (max-width: 900px) {
  .channel-layout {
    flex-direction: column;
    gap: 12px;
  }

  .channel-sidebar {
    width: 100%;
    border-right: 0;
    border-bottom: 1px solid var(--border-color);
    padding-right: 0;
    padding-bottom: 8px;
  }
}
</style>
