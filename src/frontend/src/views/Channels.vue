<template>
  <div class="channels">
    <h1 class="page-title">通道管理</h1>

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
            <el-button
              v-if="activeChannelType === 'stream'"
              type="success"
              @click="openPcapUploadDialog"
            >上传 PCAP</el-button>
            <el-button v-if="activeChannelType === 'stream'" type="primary" @click="openAddStreamDialog">新增 Stream 通道</el-button>
          </div>
        </div>
      </template>
      <div class="channel-layout">
        <div class="channel-sidebar">
          <div
            class="channel-type-item"
            :class="{ active: activeChannelType === 'dataframe' }"
            @click="switchChannelType('dataframe')"
          >
            <span>DataFrame 通道</span>
            <el-tag size="small" effect="plain">{{ dfChannels.length }}</el-tag>
          </div>
          <div
            class="channel-type-item"
            :class="{ active: activeChannelType === 'database' }"
            @click="switchChannelType('database')"
          >
            <span>数据库通道</span>
            <el-tag size="small" effect="plain">{{ dbChannels.length }}</el-tag>
          </div>
          <div
            class="channel-type-item"
            :class="{ active: activeChannelType === 'stream' }"
            @click="switchChannelType('stream')"
          >
            <span>Stream 通道</span>
            <el-tag size="small" effect="plain">{{ streamChannels.length }}</el-tag>
          </div>
        </div>

        <div class="channel-main">
          <div class="section-title">{{ sectionTitle }}</div>

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

          <el-table v-else-if="activeChannelType === 'database'" :data="filteredDbChannels" style="width: 100%" v-loading="loadingDb">
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

          <el-table v-else :data="filteredStreamChannels" style="width: 100%" v-loading="loadingStream">
            <el-table-column prop="type" label="类型" width="140" />
            <el-table-column prop="name" label="名称" min-width="220" />
            <el-table-column prop="role" label="角色" width="110">
              <template #default="scope">
                <el-tag size="small" effect="plain">{{ scope.row.role || 'both' }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="status" label="状态" width="160">
              <template #default="scope">
                <el-tag :type="streamStatusTagType(scope.row.status)">
                  {{ scope.row.status || 'unknown' }}
                </el-tag>
                <el-tag
                  v-if="scope.row.type === 'pcapfile' && scope.row.is_finite"
                  class="finite-stream-tag"
                  type="info"
                  size="small"
                >
                  有限流
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="容量/占用" min-width="180">
              <template #default="scope">
                <span>{{ scope.row.size || 0 }}/{{ scope.row.capacity || 0 }}</span>
                <el-tag v-if="scope.row.in_use" style="margin-left:8px" type="warning" size="small">in_use</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="300">
              <template #default="scope">
                <el-button
                  v-if="scope.row.type !== 'pcapfile'"
                  type="primary"
                  size="small"
                  text
                  @click="openEditStreamDialog(scope.row)"
                >编辑</el-button>
                <el-button
                  v-if="scope.row.type !== 'pcapfile'"
                  type="warning"
                  size="small"
                  text
                  :disabled="scope.row.in_use"
                  @click="resetStream(scope.row)"
                >重置</el-button>
                <el-button type="danger" size="small" text @click="removeStream(scope.row)">删除</el-button>
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
          >
            <template #default="scope">
              <span :class="{ 'packet-raw-cell': isPacketRawPreviewValue(scope.row[col]) }">
                {{ formatPreviewCell(scope.row[col]) }}
              </span>
            </template>
          </el-table-column>
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

    <el-dialog v-model="showStreamDialog" :title="streamDialogTitle" width="620px" @close="resetStreamForm">
      <el-form :model="streamForm" label-width="120px">
        <el-form-item label="通道类型" required>
          <el-select v-model="streamForm.type" placeholder="请选择类型" style="width:100%" @change="onStreamTypeChange">
            <el-option
              v-for="def in streamDefinitions"
              :key="def.channel_type"
              :label="def.display_name || def.channel_type"
              :value="def.channel_type"
            />
          </el-select>
        </el-form-item>
        <el-form-item label="通道名称" required>
          <el-input v-model="streamForm.name" placeholder="例如 npm_hub" :disabled="streamDialogMode === 'edit'" />
        </el-form-item>
        <el-form-item label="角色" required>
          <el-select v-model="streamForm.role" style="width:100%">
            <el-option
              v-for="role in currentStreamRoles"
              :key="role"
              :label="role"
              :value="role"
            />
          </el-select>
        </el-form-item>

        <el-form-item
          v-for="field in currentStreamSchema"
          :key="field.key"
          :label="field.key"
          :required="!!field.required"
        >
          <el-select
            v-if="field.type === 'enum'"
            v-model="streamForm.options[field.key]"
            style="width:100%"
          >
            <el-option
              v-for="v in (field.enum_values || [])"
              :key="v"
              :label="v"
              :value="v"
            />
          </el-select>
          <el-input-number
            v-else-if="field.type === 'int'"
            v-model="streamForm.options[field.key]"
            :min="field.has_range ? field.min_value : undefined"
            :max="field.has_range && field.max_value > 0 ? field.max_value : undefined"
            style="width:100%"
          />
          <el-switch
            v-else-if="field.type === 'bool'"
            v-model="streamForm.options[field.key]"
          />
          <el-input
            v-else-if="field.type === 'array'"
            v-model="streamArrayInputs[field.key]"
            placeholder="使用逗号分隔多个值"
          />
          <el-input
            v-else
            v-model="streamForm.options[field.key]"
          />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="showStreamDialog = false">取消</el-button>
        <el-button type="primary" :loading="streamSubmitting" @click="submitStreamForm">保存</el-button>
      </template>
    </el-dialog>

    <el-dialog
      v-model="showPcapUploadDialog"
      title="上传 PCAP/PCAPNG"
      width="560px"
      :close-on-click-modal="!pcapSubmitting"
      :close-on-press-escape="!pcapSubmitting"
      :show-close="!pcapSubmitting"
      @closed="resetPcapUploadForm"
    >
      <el-form :model="pcapForm" label-width="130px">
        <el-form-item label="Capture 文件" required>
          <input
            ref="pcapInput"
            class="pcap-file-input"
            type="file"
            accept=".pcap,.pcapng"
            :disabled="pcapSubmitting"
            @change="handlePcapFileSelected"
          />
          <div class="form-hint">仅支持单个 .pcap 或 .pcapng 文件</div>
        </el-form-item>
        <el-form-item label="通道名称" required>
          <el-input
            v-model="pcapForm.name"
            :disabled="pcapSubmitting"
            placeholder="例如 capture_01"
          />
        </el-form-item>
        <el-form-item label="文件格式">
          <el-select v-model="pcapForm.format" :disabled="pcapSubmitting" style="width:100%">
            <el-option label="自动识别" value="auto" />
            <el-option label="PCAP" value="pcap" />
            <el-option label="PCAPNG" value="pcapng" />
          </el-select>
        </el-form-item>
        <el-form-item label="每批数据包数">
          <el-input-number
            v-model="pcapForm.batch_packets"
            :disabled="pcapSubmitting"
            :min="1"
            :max="4294967295"
            :precision="0"
            controls-position="right"
            style="width:100%"
          />
        </el-form-item>
        <el-form-item label="回放模式">
          <el-select v-model="pcapForm.replay_mode" :disabled="pcapSubmitting" style="width:100%">
            <el-option label="最快速度" value="fast" />
            <el-option label="按时间戳" value="timestamp" />
          </el-select>
        </el-form-item>
        <el-form-item label="回放速度">
          <el-input-number
            v-model="pcapForm.replay_speed_milli"
            :disabled="pcapSubmitting"
            :min="1"
            :max="4294967295"
            :precision="0"
            controls-position="right"
            style="width:100%"
          />
          <div class="form-hint">1000 表示 1 倍速，仅时间戳回放模式生效</div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button :disabled="pcapSubmitting" @click="showPcapUploadDialog = false">取消</el-button>
        <el-button
          type="primary"
          :loading="pcapSubmitting"
          :disabled="pcapSubmitting"
          @click="submitPcapUpload"
        >上传并创建通道</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { Search, Loading } from '@element-plus/icons-vue'
import api, {
  PCAP_UPLOAD_DEFAULTS,
  formatPreviewCell,
  isPacketRawPreviewValue,
  isSupportedPcapFile,
  pcapUploadErrorMessage
} from '../api'
import { ElMessage, ElMessageBox } from 'element-plus'

const searchText = ref('')
const activeChannelType = ref('dataframe')
const showAddDialog = ref(false)
const submitting = ref(false)
const formRef = ref(null)
const csvInput = ref(null)

const dbChannels = ref([])
const dfChannels = ref([])
const streamChannels = ref([])
const streamDefinitions = ref([])
const loadingDb = ref(false)
const loadingDf = ref(false)
const loadingStream = ref(false)
const showStreamDialog = ref(false)
const streamDialogMode = ref('add')
const streamSubmitting = ref(false)
const streamArrayInputs = ref({})
const showPcapUploadDialog = ref(false)
const pcapSubmitting = ref(false)
const pcapInput = ref(null)
const pcapFile = ref(null)
const createPcapUploadForm = () => ({ name: '', ...PCAP_UPLOAD_DEFAULTS })
const pcapForm = ref(createPcapUploadForm())
const streamForm = ref({
  type: '',
  name: '',
  role: 'both',
  options: {}
})

const streamDialogTitle = computed(() =>
  streamDialogMode.value === 'edit' ? '编辑 Stream 通道' : '新增 Stream 通道'
)

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

const filteredStreamChannels = computed(() => {
  const s = searchText.value.trim().toLowerCase()
  if (!s) return streamChannels.value
  return streamChannels.value.filter(ch =>
    (ch.name || '').toLowerCase().includes(s) ||
    (ch.type || '').toLowerCase().includes(s) ||
    (ch.role || '').toLowerCase().includes(s) ||
    (ch.status || '').toLowerCase().includes(s)
  )
})

const sectionTitle = computed(() => {
  if (activeChannelType.value === 'dataframe') return 'DataFrame 通道'
  if (activeChannelType.value === 'database') return '数据库通道'
  return 'Stream 通道'
})

const currentStreamDefinition = computed(() =>
  streamDefinitions.value.find(d => d.channel_type === streamForm.value.type) || null
)

const currentStreamSchema = computed(() =>
  (currentStreamDefinition.value?.option_schema || [])
)

const currentStreamRoles = computed(() => {
  const roles = currentStreamDefinition.value?.allowed_roles
  if (!roles || roles.length === 0) return ['both', 'source', 'sink']
  return roles
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

const loadStreamChannels = async () => {
  loadingStream.value = true
  try {
    const res = await api.listStreamChannels()
    const list = Array.isArray(res.data) ? res.data : (res.data.channels || [])
    streamChannels.value = list.map(ch => ({
      type: ch.type || '',
      name: ch.name || '',
      role: ch.type === 'pcapfile' ? 'source' : (ch.role || 'both'),
      status: ch.status || 'unknown',
      in_use: !!ch.in_use,
      size: Number(ch.size || 0),
      capacity: Number(ch.capacity || 0),
      is_finite: !!ch.is_finite,
      is_finished: !!ch.is_finished,
      option_json: ch.option_json || {},
      option: ch.option || '',
      derived_channels: Array.isArray(ch.derived_channels) ? ch.derived_channels : []
    }))
  } catch (error) {
    ElMessage.error('加载 Stream 通道失败: ' + (error.response?.data?.error || error.message))
  } finally {
    loadingStream.value = false
  }
}

const loadStreamDefinitions = async () => {
  try {
    const res = await api.getStreamDefinitions()
    streamDefinitions.value = Array.isArray(res.data?.definitions) ? res.data.definitions : []
  } catch (error) {
    streamDefinitions.value = []
    ElMessage.error('加载 Stream 类型定义失败: ' + (error.response?.data?.error || error.message))
  }
}

const reloadAll = async () => {
  await Promise.all([loadDbChannels(), loadDfChannels(), loadStreamChannels(), loadStreamDefinitions()])
}

const switchChannelType = async (nextType) => {
  if (activeChannelType.value === nextType) return
  activeChannelType.value = nextType
  if (nextType === 'stream') {
    await Promise.all([loadStreamChannels(), loadStreamDefinitions()])
  } else if (nextType === 'database') {
    await loadDbChannels()
  } else if (nextType === 'dataframe') {
    await loadDfChannels()
  }
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

const defaultValueByField = (field) => {
  const raw = field?.default_value
  if (field?.type === 'bool') return String(raw).toLowerCase() === 'true'
  if (field?.type === 'int') {
    const n = Number(raw)
    return Number.isFinite(n) ? n : 0
  }
  if (field?.type === 'array') return Array.isArray(raw) ? raw : []
  return raw ?? ''
}

const applyStreamSchemaDefaults = () => {
  const nextOptions = {}
  const nextArrayInputs = {}
  currentStreamSchema.value.forEach((field) => {
    nextOptions[field.key] = defaultValueByField(field)
    if (field.type === 'array') {
      const arr = Array.isArray(nextOptions[field.key]) ? nextOptions[field.key] : []
      nextArrayInputs[field.key] = arr.join(',')
    }
  })
  streamForm.value.options = nextOptions
  streamArrayInputs.value = nextArrayInputs
  const allowedRoles = currentStreamRoles.value
  if (!allowedRoles.includes(streamForm.value.role)) {
    streamForm.value.role = allowedRoles[0] || 'both'
  }
}

const onStreamTypeChange = () => {
  applyStreamSchemaDefaults()
}

const openAddStreamDialog = async () => {
  streamDialogMode.value = 'add'
  if (streamDefinitions.value.length === 0) {
    await loadStreamDefinitions()
  }
  streamForm.value = {
    type: streamDefinitions.value[0]?.channel_type || '',
    name: '',
    role: 'both',
    options: {}
  }
  applyStreamSchemaDefaults()
  showStreamDialog.value = true
}

const resetPcapUploadForm = () => {
  pcapForm.value = createPcapUploadForm()
  pcapFile.value = null
  if (pcapInput.value) pcapInput.value.value = ''
}

const openPcapUploadDialog = () => {
  resetPcapUploadForm()
  showPcapUploadDialog.value = true
}

const handlePcapFileSelected = (event) => {
  const file = event.target.files?.[0] || null
  if (!file) {
    pcapFile.value = null
    return
  }
  if (!isSupportedPcapFile(file)) {
    event.target.value = ''
    pcapFile.value = null
    ElMessage.warning('请选择 .pcap 或 .pcapng 文件')
    return
  }
  pcapFile.value = file
}

const isValidPcapUploadInteger = (value) =>
  Number.isInteger(value) && value >= 1 && value <= 4294967295

const submitPcapUpload = async () => {
  if (pcapSubmitting.value) return
  const name = pcapForm.value.name.trim()
  if (!name) {
    ElMessage.warning('通道名称不能为空')
    return
  }
  if (!isSupportedPcapFile(pcapFile.value)) {
    ElMessage.warning('请选择 .pcap 或 .pcapng 文件')
    return
  }
  if (!isValidPcapUploadInteger(pcapForm.value.batch_packets) ||
      !isValidPcapUploadInteger(pcapForm.value.replay_speed_milli)) {
    ElMessage.warning('批大小和回放速度必须是有效正整数')
    return
  }

  pcapSubmitting.value = true
  try {
    await api.uploadPcap(pcapFile.value, { ...pcapForm.value, name })
    ElMessage.success('PCAP 上传成功，通道已创建')
    showPcapUploadDialog.value = false
    await loadStreamChannels()
  } catch (error) {
    ElMessage.error(pcapUploadErrorMessage(error))
  } finally {
    pcapSubmitting.value = false
  }
}

const openEditStreamDialog = async (row) => {
  streamDialogMode.value = 'edit'
  if (streamDefinitions.value.length === 0) {
    await loadStreamDefinitions()
  }
  const options = { ...(row.option_json || {}) }
  streamForm.value = {
    type: row.type,
    name: row.name,
    role: row.role || 'both',
    options
  }
  applyStreamSchemaDefaults()
  // 用现有值覆盖 schema 默认值
  streamForm.value.options = {
    ...streamForm.value.options,
    ...(row.option_json || {})
  }
  const arrInputs = {}
  currentStreamSchema.value.forEach((field) => {
    if (field.type === 'array') {
      const val = streamForm.value.options[field.key]
      arrInputs[field.key] = Array.isArray(val) ? val.join(',') : ''
    }
  })
  streamArrayInputs.value = arrInputs
  showStreamDialog.value = true
}

const normalizeStreamOptions = () => {
  const out = {}
  currentStreamSchema.value.forEach((field) => {
    let val = streamForm.value.options[field.key]
    if (field.type === 'array') {
      const text = streamArrayInputs.value[field.key] || ''
      val = text.split(',').map(v => v.trim()).filter(Boolean)
    } else if (field.type === 'int') {
      val = Number(val)
      if (!Number.isFinite(val)) val = Number(field.default_value || 0)
    } else if (field.type === 'bool') {
      val = !!val
    }
    if (field.required && (val === '' || val === null || val === undefined || (Array.isArray(val) && val.length === 0))) {
      throw new Error(`参数 ${field.key} 为必填`)
    }
    out[field.key] = val
  })
  return out
}

const submitStreamForm = async () => {
  if (!streamForm.value.type || !streamForm.value.name) {
    ElMessage.warning('类型和名称为必填项')
    return
  }
  const role = streamForm.value.role || 'both'
  if (!['source', 'sink', 'both'].includes(role)) {
    ElMessage.warning('角色必须是 source/sink/both')
    return
  }
  streamSubmitting.value = true
  try {
    const payload = {
      type: streamForm.value.type,
      name: streamForm.value.name,
      role,
      options: normalizeStreamOptions()
    }
    if (streamDialogMode.value === 'edit') {
      await api.modifyStreamChannel(payload)
      ElMessage.success('Stream 通道修改成功')
    } else {
      await api.addStreamChannel(payload)
      ElMessage.success('Stream 通道新增成功')
    }
    showStreamDialog.value = false
    await loadStreamChannels()
  } catch (error) {
    ElMessage.error('保存 Stream 通道失败: ' + (error.response?.data?.error || error.message))
  } finally {
    streamSubmitting.value = false
  }
}

const removeStream = async (row) => {
  try {
    await ElMessageBox.confirm(`确认删除 Stream 通道 ${row.type}.${row.name}？`, '删除确认', { type: 'warning' })
    await api.removeStreamChannel(row.type, row.name)
    ElMessage.success('删除成功')
    await loadStreamChannels()
  } catch (e) {
    if (e !== 'cancel') ElMessage.error('删除失败: ' + (e.response?.data?.error || e.message))
  }
}

const resetStream = async (row) => {
  try {
    await ElMessageBox.confirm(
      `确认重置 Stream 通道 ${row.type}.${row.name}？重置会清空运行态并重新初始化该通道。`,
      '重置确认',
      { type: 'warning' }
    )
    await api.resetStreamChannel(row.type, row.name)
    ElMessage.success('重置成功')
    await loadStreamChannels()
  } catch (e) {
    if (e !== 'cancel' && e !== 'close') {
      ElMessage.error('重置失败: ' + (e.response?.data?.error || e.message))
    }
  }
}

const resetForm = () => {
  form.value = { type: 'mysql', name: '', host: '127.0.0.1', port: '', user: '', password: '', database: '', path: '' }
}

const resetStreamForm = () => {
  streamForm.value = { type: '', name: '', role: 'both', options: {} }
  streamArrayInputs.value = {}
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

const streamStatusTagType = (status) => {
  if (status === 'running') return 'success'
  if (status === 'draining') return 'warning'
  if (status === 'stopped') return 'info'
  return ''
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
.card-header { display: flex; justify-content: space-between; align-items: center; gap: 12px; }
.header-left { display: flex; align-items: center; }
.header-actions { display: flex; gap: 10px; align-items: center; }
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
.finite-stream-tag { margin-left: 8px; }
.pcap-file-input { width: 100%; color: var(--text-primary); }
.form-hint { margin-top: 4px; color: var(--text-secondary); font-size: 12px; line-height: 1.4; }
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

.packet-raw-cell {
  font-family: monospace;
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
