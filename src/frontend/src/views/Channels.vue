<template>
  <div class="channels">
    <h1 class="page-title">通道列表</h1>

    <el-card>
      <template #header>
        <div class="card-header">
          <el-input
            v-model="searchText"
            placeholder="搜索通道名称或类型"
            style="width: 300px"
            clearable
          >
            <template #prefix>
              <el-icon><Search /></el-icon>
            </template>
          </el-input>
          <el-button type="primary" @click="showAddDialog = true">
            <el-icon><Plus /></el-icon> 新增数据库通道
          </el-button>
        </div>
      </template>

      <el-table :data="filteredChannels" style="width: 100%" v-loading="loading">
        <el-table-column prop="name" label="名称" width="200" />
        <el-table-column prop="catelog" label="类别" width="150" />
        <el-table-column prop="type" label="类型" width="150" />
        <el-table-column label="Schema" min-width="200">
          <template #default="scope">
            <!-- 数据库通道：显示数据库名 -->
            <span v-if="isDbChannel(scope.row)" class="db-label">
              {{ scope.row.schema || '—' }}
            </span>
            <!-- dataframe 通道：显示字段数，悬停展开 -->
            <el-popover v-else placement="top" :width="400" trigger="hover">
              <template #reference>
                <el-tag>{{ scope.row.schema.length }} 字段</el-tag>
              </template>
              <div class="schema-popup">
                <div v-for="field in scope.row.schema" :key="field.name" class="schema-field">
                  <span class="field-name">{{ field.name }}</span>
                  <span class="field-type">{{ field.type }}</span>
                </div>
              </div>
            </el-popover>
          </template>
        </el-table-column>
        <el-table-column prop="status" label="状态" width="100">
          <template #default="scope">
            <el-tag type="success">{{ scope.row.status }}</el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="160">
          <template #default="scope">
            <el-button
              type="primary" size="small" text
              @click="openBrowser(scope.row)"
            >浏览</el-button>
            <el-button
              v-if="isDbChannel(scope.row)"
              type="danger" size="small" text
              @click="handleRemove(scope.row)"
            >删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 数据库浏览器 Drawer -->
    <el-drawer
      v-model="drawerVisible"
      :title="`浏览：${browserChannel.catelog}.${browserChannel.name}`"
      size="60%"
      direction="rtl"
    >
      <!-- dataframe 通道：直接展示数据 -->
      <div v-if="!isDbChannel(browserChannel)" class="browser-content" style="padding: 0">
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
      </div>

      <!-- 数据库通道：左侧表列表 + 右侧 Tab -->
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

    <!-- 新增数据库通道对话框 -->
    <el-dialog v-model="showAddDialog" title="新增数据库通道" width="500px" @close="resetForm">
      <el-form :model="form" label-width="100px" ref="formRef">
        <el-form-item label="类型" prop="type" required>
          <el-select v-model="form.type" placeholder="选择数据库类型" style="width:100%">
            <el-option label="SQLite" value="sqlite" />
            <el-option label="MySQL" value="mysql" />
            <el-option label="ClickHouse" value="clickhouse" />
          </el-select>
        </el-form-item>
        <el-form-item label="名称" prop="name" required>
          <el-input v-model="form.name" placeholder="通道名称，如 mydb" />
        </el-form-item>

        <!-- SQLite 专属字段 -->
        <template v-if="form.type === 'sqlite'">
          <el-form-item label="文件路径">
            <el-input v-model="form.path" placeholder=":memory: 或 /data/test.db" />
          </el-form-item>
        </template>

        <!-- MySQL / ClickHouse 公共字段 -->
        <template v-if="form.type === 'mysql' || form.type === 'clickhouse'">
          <el-form-item label="主机">
            <el-input v-model="form.host" placeholder="127.0.0.1" />
          </el-form-item>
          <el-form-item label="端口">
            <el-input v-model="form.port" :placeholder="form.type === 'mysql' ? '3306' : '8123'" />
          </el-form-item>
          <el-form-item label="用户名">
            <el-input v-model="form.user" :placeholder="form.type === 'mysql' ? 'root' : 'default'" />
          </el-form-item>
          <el-form-item label="密码">
            <el-input v-model="form.password" type="password" show-password placeholder="留空表示无密码" />
          </el-form-item>
          <el-form-item label="数据库" prop="database" required>
            <el-input v-model="form.database" :placeholder="form.type === 'mysql' ? 'mydb' : 'default'" />
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
import { Search, Plus, Loading } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage, ElMessageBox } from 'element-plus'

const channels = ref([])
const searchText = ref('')
const loading = ref(false)
const showAddDialog = ref(false)
const submitting = ref(false)
const formRef = ref(null)

const form = ref({
  type: 'mysql', name: '', host: '127.0.0.1', port: '',
  user: '', password: '', database: '', path: ''
})

const hasDbChannels = computed(() =>
  channels.value.some(ch => ['sqlite','mysql','clickhouse'].includes(ch.catelog))
)

const isDbChannel = (row) =>
  row && ['sqlite','mysql','clickhouse'].includes(row.catelog)

const filteredChannels = computed(() => {
  if (!searchText.value) return channels.value
  const search = searchText.value.toLowerCase()
  return channels.value.filter(ch =>
    ch.name.toLowerCase().includes(search) ||
    ch.type.toLowerCase().includes(search) ||
    ch.catelog.toLowerCase().includes(search)
  )
})

const loadChannels = async () => {
  loading.value = true
  try {
    const res = await api.getChannels()
    channels.value = res.data.map(ch => {
      const isDb = ['sqlite','mysql','clickhouse'].includes(ch.catelog)
      return {
        ...ch,
        // 数据库通道 schema 是字符串（数据库名），dataframe 通道是字段数组
        schema: isDb
          ? (ch.schema || '')
          : JSON.parse(ch.schema_json || ch.schema || '[]').map(field => ({
              name: field.name,
              type: getTypeName(field.type)
            }))
      }
    })
  } catch (error) {
    ElMessage.error('加载通道列表失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

const getTypeName = (typeId) => {
  const typeMap = { 0:'BOOL',1:'INT8',2:'UINT32',3:'UINT64',4:'INT32',5:'INT64',6:'STRING',7:'DOUBLE' }
  return typeMap[typeId] || 'UNKNOWN'
}

// 将表单转为 config_str 格式
const buildConfigStr = () => {
  const f = form.value
  let s = `type=${f.type};name=${f.name}`
  if (f.type === 'sqlite') {
    if (f.path) s += `;path=${f.path}`
  } else {
    if (f.host)     s += `;host=${f.host}`
    if (f.port)     s += `;port=${f.port}`
    if (f.user)     s += `;user=${f.user}`
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
  if ((form.value.type === 'mysql' || form.value.type === 'clickhouse') && !form.value.database) {
    ElMessage.warning('MySQL/ClickHouse 通道必须填写数据库名称')
    return
  }
  submitting.value = true
  try {
    await api.addDbChannel(buildConfigStr())
    ElMessage.success('数据库通道添加成功')
    showAddDialog.value = false
    loadChannels()
  } catch (error) {
    const msg = error.response?.data?.error || error.message
    ElMessage.error('添加失败: ' + msg)
  } finally {
    submitting.value = false
  }
}

const handleRemove = async (row) => {
  try {
    await ElMessageBox.confirm(`确认删除通道 ${row.catelog}.${row.name}？`, '删除确认', { type: 'warning' })
    await api.removeDbChannel(row.catelog, row.name)
    ElMessage.success('删除成功')
    loadChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('删除失败: ' + (e.response?.data?.error || e.message))
  }
}

const resetForm = () => {
  form.value = { type: 'mysql', name: '', host: '127.0.0.1', port: '', user: '', password: '', database: '', path: '' }
}

// ==================== 数据库浏览器 ====================
const drawerVisible = ref(false)
const browserChannel = ref({ catelog: '', name: '' })
const tableList = ref([])
const tablesLoading = ref(false)
const selectedTable = ref('')
const activeTab = ref('describe')

const describeData = ref({ columns: [], rows: [] })
const describeLoading = ref(false)
const previewData = ref({ columns: [], rows: [] })
const previewLoading = ref(false)

// 将 {columns, data} 转为 el-table 需要的对象数组
const toRows = (res) => {
  if (!res || !res.columns || !res.data) return []
  return res.data.map(row => {
    const obj = {}
    res.columns.forEach((col, i) => { obj[col] = row[i] })
    return obj
  })
}

const openBrowser = async (row) => {
  browserChannel.value = { catelog: row.catelog, name: row.name }
  tableList.value = []
  selectedTable.value = ''
  describeData.value = { columns: [], rows: [] }
  previewData.value = { columns: [], rows: [] }
  drawerVisible.value = true

  if (!isDbChannel(row)) {
    // dataframe 通道：直接预览
    previewLoading.value = true
    try {
      const res = await api.previewDataframe(row.catelog, row.name)
      previewData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
    } catch (e) {
      ElMessage.error('获取数据失败: ' + (e.response?.data?.error || e.message))
    } finally {
      previewLoading.value = false
    }
    return
  }

  // 数据库通道：加载表列表
  tablesLoading.value = true
  try {
    const res = await api.listDbTables(row.catelog, row.name)
    tableList.value = res.data.tables || []
    if (tableList.value.length > 0) {
      selectTable(tableList.value[0])
    }
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

  const { catelog, name } = browserChannel.value

  describeLoading.value = true
  previewLoading.value = true

  try {
    const res = await api.describeDbTable(catelog, name, table)
    describeData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
  } catch (e) {
    ElMessage.error('获取表结构失败: ' + (e.response?.data?.error || e.message))
  } finally {
    describeLoading.value = false
  }

  try {
    const res = await api.previewDbTable(catelog, name, table)
    previewData.value = { columns: res.data.columns || [], rows: toRows(res.data) }
  } catch (e) {
    ElMessage.error('获取数据预览失败: ' + (e.response?.data?.error || e.message))
  } finally {
    previewLoading.value = false
  }
}

onMounted(() => { loadChannels() })
</script>

<style scoped>
.channels { }
.page-title { font-size: 24px; font-weight: 600; margin-bottom: 20px; color: var(--text-primary); }
.card-header { display: flex; justify-content: space-between; align-items: center; }
.schema-popup { max-height: 300px; overflow-y: auto; }
.schema-field { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid #eee; }
.schema-field:last-child { border-bottom: none; }
.field-name { font-weight: 600; color: var(--text-primary); }
.field-type { color: var(--text-secondary); font-family: monospace; }
.db-label { font-size: 13px; color: var(--text-secondary); font-family: monospace; }

/* 浏览器 Drawer */
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
</style>
