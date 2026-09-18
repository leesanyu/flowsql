<template>
  <div class="config-channels">
    <div class="config-toolbar">
      <el-alert
        v-if="lastPublishedReference"
        :title="`已发布 ${lastPublishedReference}`"
        type="success"
        :closable="true"
        show-icon
        @close="lastPublishedReference = ''"
      />
      <el-button v-if="lastPublishedReference" text @click="copyReference(lastPublishedReference)">复制刚发布的引用</el-button>
      <el-button type="primary" @click="openCreate">新建配置通道</el-button>
    </div>
    <el-table :data="filteredChannels" style="width: 100%" v-loading="loading">
      <el-table-column prop="name" label="名称" min-width="150" />
      <el-table-column prop="current_revision" label="Current revision" width="140">
        <template #default="scope">r{{ scope.row.current_revision }}</template>
      </el-table-column>
      <el-table-column prop="format" label="格式" width="90">
        <template #default="scope">
          <el-tag size="small" effect="plain">{{ scope.row.format }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="schema_id" label="Schema ID" min-width="150" />
      <el-table-column label="大小" width="100">
        <template #default="scope">{{ formatBytes(scope.row.content_bytes) }}</template>
      </el-table-column>
      <el-table-column label="SHA-256" width="140">
        <template #default="scope">
          <span class="digest" :title="scope.row.sha256_hex">{{ shortDigest(scope.row.sha256_hex) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="更新时间" min-width="170">
        <template #default="scope">{{ formatTime(scope.row.updated_at_unix_ms) }}</template>
      </el-table-column>
      <el-table-column label="操作" width="430" fixed="right">
        <template #default="scope">
          <el-button type="primary" size="small" text @click="openPreview(currentReference(scope.row))">预览</el-button>
          <el-button type="info" size="small" text @click="copyReference(currentReference(scope.row))">复制引用</el-button>
          <el-button type="primary" size="small" text @click="openEdit(scope.row)">编辑并发布</el-button>
          <el-button type="success" size="small" text @click="openUpload(scope.row)">上传新版本</el-button>
          <el-button type="info" size="small" text @click="openHistory(scope.row)">版本历史</el-button>
        </template>
      </el-table-column>
    </el-table>
    <div v-if="nextCursor" class="load-more">
      <el-button :loading="loadingMore" @click="loadMore">加载更多</el-button>
    </div>

    <el-drawer v-model="showHistory" :title="`版本历史：config.${historyChannelName}`" size="78%">
      <el-table :data="historyItems" v-loading="historyLoading" style="width: 100%">
        <el-table-column prop="revision" label="Revision" width="90">
          <template #default="scope">r{{ scope.row.revision }}</template>
        </el-table-column>
        <el-table-column prop="format" label="格式" width="85" />
        <el-table-column prop="schema_id" label="Schema ID" min-width="140" />
        <el-table-column label="摘要" width="135">
          <template #default="scope">
            <span class="digest" :title="scope.row.sha256_hex">{{ shortDigest(scope.row.sha256_hex) }}</span>
          </template>
        </el-table-column>
        <el-table-column label="大小" width="105">
          <template #default="scope">{{ formatBytes(scope.row.content_bytes) }}</template>
        </el-table-column>
        <el-table-column label="发布时间" width="180">
          <template #default="scope">{{ formatTime(scope.row.created_at_unix_ms) }}</template>
        </el-table-column>
        <el-table-column label="来源 revision" width="125">
          <template #default="scope">{{ scope.row.base_revision ? `r${scope.row.base_revision}` : '-' }}</template>
        </el-table-column>
        <el-table-column prop="change_note" label="变更说明" min-width="140" show-overflow-tooltip />
        <el-table-column label="操作" width="340" fixed="right">
          <template #default="scope">
            <el-button size="small" text @click="openPreview(historyReference(scope.row))">预览</el-button>
            <el-button size="small" text @click="downloadSnapshot(historyReference(scope.row))">下载</el-button>
            <el-button size="small" text @click="copyReference(historyReference(scope.row))">复制引用</el-button>
            <el-button type="primary" size="small" text @click="restoreHistory(scope.row)">基于此版本创建新版本</el-button>
          </template>
        </el-table-column>
      </el-table>
      <div v-if="historyCursor" class="load-more">
        <el-button :loading="historyLoadingMore" @click="loadMoreHistory">加载更多历史</el-button>
      </div>
    </el-drawer>

    <el-dialog v-model="showPreview" :title="`纯文本预览：${previewReference}`" width="720px">
      <div v-loading="previewLoading" class="preview-body">
        <pre v-text="previewText" class="preview-plain-text" />
      </div>
      <template #footer>
        <el-button @click="showPreview = false">关闭</el-button>
        <el-button type="primary" :disabled="previewLoading" @click="downloadSnapshot(previewReference)">下载</el-button>
      </template>
    </el-dialog>

    <el-dialog
      v-model="showPublishDialog"
      :title="publishDialogTitle"
      width="720px"
      :close-on-click-modal="!submitting"
      @closed="resetPublishDialog"
    >
      <el-form label-width="120px">
        <el-form-item label="通道名称" required>
          <el-input
            v-model.trim="draft.name"
            :disabled="publishKind !== 'create' || submitting"
            placeholder="例如 corp-apps"
          />
        </el-form-item>
        <el-form-item v-if="publishKind !== 'create'" label="Current revision">
          <el-input :model-value="`r${expectedRevision}（只读）`" disabled />
        </el-form-item>
        <el-form-item label="格式" required>
          <el-select v-model="draft.format" :disabled="submitting" style="width:100%">
            <el-option label="JSON" value="json" />
            <el-option label="YAML" value="yaml" />
            <el-option label="XML" value="xml" />
          </el-select>
        </el-form-item>
        <el-form-item label="Schema ID" required>
          <el-input v-model.trim="draft.schema_id" :disabled="submitting" placeholder="例如 scope-v1" />
        </el-form-item>
        <el-form-item label="变更说明">
          <el-input
            v-model="draft.change_note"
            :disabled="submitting"
            maxlength="1024"
            show-word-limit
          />
        </el-form-item>
        <el-form-item label="内容来源" required>
          <el-radio-group v-model="inputMode" :disabled="submitting || resolving || readingFile" @change="changeInputMode">
            <el-radio-button value="editor">文本编辑器</el-radio-button>
            <el-radio-button value="file">选择文件</el-radio-button>
          </el-radio-group>
        </el-form-item>
        <el-form-item v-if="inputMode === 'file'" label="配置文件" required>
          <input
            ref="fileInput"
            class="config-file-input"
            type="file"
            accept=".json,.yaml,.yml,.xml"
            :disabled="submitting || readingFile"
            @change="handleFileSelected"
          />
          <div class="form-hint">仅接受 JSON、YAML、YML 或 XML，原文最大 512 KiB</div>
        </el-form-item>
        <el-form-item v-else label="配置内容" required>
          <el-input
            v-model="draft.content"
            type="textarea"
            :rows="14"
            :disabled="submitting || resolving"
            resize="vertical"
            placeholder="输入 UTF-8 配置原文"
          />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button :disabled="submitting" @click="showPublishDialog = false">取消</el-button>
        <el-button type="primary" :loading="submitting || resolving || readingFile" @click="submitPublish">发布</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { ElMessage } from 'element-plus'

import api from '../api'
import {
  CONFIG_CONTENT_MAX_BYTES,
  buildConfigPublishPayload,
  configChannelErrorMessage,
  configDownloadName,
  configReference,
  decodeConfigBytes,
  decodeUtf8Bytes,
  decodeUtf8Base64,
  formatFromFilename,
  validateConfigDraft
} from '../utils/configChannel.js'

const props = defineProps({
  searchText: { type: String, default: '' }
})
const emit = defineEmits(['count-change'])

const channels = ref([])
const nextCursor = ref('')
const loading = ref(false)
const loadingMore = ref(false)
const showPublishDialog = ref(false)
const submitting = ref(false)
const resolving = ref(false)
const readingFile = ref(false)
const publishKind = ref('create')
const inputMode = ref('editor')
const expectedRevision = ref(0)
const fileInput = ref(null)
const lastPublishedReference = ref('')
const baseRevision = ref(0)
const showHistory = ref(false)
const historyChannelName = ref('')
const historyCurrentRevision = ref(0)
const historyItems = ref([])
const historyCursor = ref('')
const historyLoading = ref(false)
const historyLoadingMore = ref(false)
const showPreview = ref(false)
const previewReference = ref('')
const previewText = ref('')
const previewLoading = ref(false)
const emptyDraft = () => ({
  name: '', format: 'json', schema_id: '', content: '', original_filename: '', change_note: ''
})
const draft = ref(emptyDraft())

const publishDialogTitle = computed(() => {
  if (publishKind.value === 'create') return '新建配置通道'
  return inputMode.value === 'file' ? '上传配置新版本' : '编辑并发布新版本'
})

const filteredChannels = computed(() => {
  const search = props.searchText.trim().toLowerCase()
  if (!search) return channels.value
  return channels.value.filter((channel) =>
    [channel.name, channel.format, channel.schema_id]
      .some((value) => String(value || '').toLowerCase().includes(search))
  )
})

const errorDetail = (error) => error.response?.data?.error || error.message || '未知错误'
const shortDigest = (digest) => digest ? `${digest.slice(0, 12)}…` : '-'
const formatBytes = (bytes) => {
  const value = Number(bytes || 0)
  if (value < 1024) return `${value} B`
  return `${(value / 1024).toFixed(value < 10 * 1024 ? 1 : 0)} KiB`
}
const formatTime = (milliseconds) => {
  const value = Number(milliseconds)
  return Number.isFinite(value) && value > 0 ? new Date(value).toLocaleString() : '-'
}

const fetchPage = async (cursor, append) => {
  const res = await api.listConfigChannels(cursor, 100)
  const items = Array.isArray(res.data?.items) ? res.data.items : []
  channels.value = append ? [...channels.value, ...items] : items
  nextCursor.value = typeof res.data?.next_cursor === 'string' ? res.data.next_cursor : ''
  emit('count-change', channels.value.length)
}

const loadChannels = async () => {
  loading.value = true
  try {
    await fetchPage('', false)
  } catch (error) {
    ElMessage.error(`加载配置通道失败: ${errorDetail(error)}`)
  } finally {
    loading.value = false
  }
}

const loadMore = async () => {
  if (!nextCursor.value || loadingMore.value) return
  loadingMore.value = true
  try {
    await fetchPage(nextCursor.value, true)
  } catch (error) {
    ElMessage.error(`加载更多配置通道失败: ${errorDetail(error)}`)
  } finally {
    loadingMore.value = false
  }
}

const openCreate = () => {
  publishKind.value = 'create'
  inputMode.value = 'editor'
  expectedRevision.value = 0
  baseRevision.value = 0
  draft.value = emptyDraft()
  showPublishDialog.value = true
}

const currentReference = (row) => configReference(row.name, row.current_revision)
const historyReference = (row) => configReference(historyChannelName.value, row.revision)

const openUpload = (row) => {
  publishKind.value = 'update'
  inputMode.value = 'file'
  expectedRevision.value = row.current_revision
  baseRevision.value = 0
  draft.value = {
    ...emptyDraft(), name: row.name, format: row.format, schema_id: row.schema_id
  }
  showPublishDialog.value = true
}

const openEdit = async (row) => {
  publishKind.value = 'update'
  inputMode.value = 'editor'
  expectedRevision.value = row.current_revision
  baseRevision.value = 0
  draft.value = {
    ...emptyDraft(), name: row.name, format: row.format, schema_id: row.schema_id
  }
  showPublishDialog.value = true
  resolving.value = true
  try {
    const res = await api.resolveConfigChannel(currentReference(row))
    draft.value.content = decodeUtf8Base64(res.data?.content_base64 || '')
  } catch (error) {
    ElMessage.error(`载入 current 配置失败: ${configChannelErrorMessage(error)}`)
    showPublishDialog.value = false
  } finally {
    resolving.value = false
  }
}

const changeInputMode = () => {
  draft.value.content = ''
  draft.value.original_filename = ''
  if (fileInput.value) fileInput.value.value = ''
}

const handleFileSelected = async (event) => {
  draft.value.content = ''
  draft.value.original_filename = ''
  const file = event.target.files?.[0]
  if (!file) return
  const format = formatFromFilename(file.name)
  if (!format) {
    ElMessage.warning('仅支持 .json、.yaml、.yml 或 .xml 文件')
    event.target.value = ''
    return
  }
  if (file.size > CONFIG_CONTENT_MAX_BYTES) {
    ElMessage.warning('配置内容不能超过 512 KiB')
    event.target.value = ''
    return
  }
  readingFile.value = true
  try {
    const content = decodeUtf8Bytes(await file.arrayBuffer())
    draft.value.format = format
    draft.value.content = content
    draft.value.original_filename = file.name
  } catch {
    ElMessage.warning('配置文件必须是有效 UTF-8 文本')
    event.target.value = ''
  } finally {
    readingFile.value = false
  }
}

const submitPublish = async () => {
  if (submitting.value || resolving.value || readingFile.value) return
  if (inputMode.value === 'file' && !draft.value.original_filename) {
    ElMessage.warning('请选择配置文件')
    return
  }
  const validation = validateConfigDraft(draft.value)
  if (validation) {
    ElMessage.warning(validation)
    return
  }
  submitting.value = true
  try {
    const payload = buildConfigPublishPayload(draft.value, expectedRevision.value, baseRevision.value)
    const res = await api.publishConfigChannel(payload)
    lastPublishedReference.value = res.data?.exact_reference ||
      configReference(draft.value.name, res.data?.revision)
    ElMessage.success(res.data?.created_revision === false ? '内容未变化，已保留当前版本' : '配置发布成功')
    showPublishDialog.value = false
    await loadChannels()
  } catch (error) {
    ElMessage.error(configChannelErrorMessage(error))
  } finally {
    submitting.value = false
  }
}

const resetPublishDialog = () => {
  draft.value = emptyDraft()
  expectedRevision.value = 0
  baseRevision.value = 0
  inputMode.value = 'editor'
  resolving.value = false
  readingFile.value = false
  if (fileInput.value) fileInput.value.value = ''
}

const fetchHistoryPage = async (cursor, append) => {
  const res = await api.listConfigHistory(historyChannelName.value, cursor, 100)
  const items = Array.isArray(res.data?.items) ? res.data.items : []
  historyItems.value = append ? [...historyItems.value, ...items] : items
  historyCursor.value = typeof res.data?.next_cursor === 'string' ? res.data.next_cursor : ''
}

const openHistory = async (row) => {
  historyChannelName.value = row.name
  historyCurrentRevision.value = row.current_revision
  historyItems.value = []
  historyCursor.value = ''
  showHistory.value = true
  historyLoading.value = true
  try {
    await fetchHistoryPage('', false)
  } catch (error) {
    ElMessage.error(`加载版本历史失败: ${configChannelErrorMessage(error)}`)
  } finally {
    historyLoading.value = false
  }
}

const loadMoreHistory = async () => {
  if (!historyCursor.value || historyLoadingMore.value) return
  historyLoadingMore.value = true
  try {
    await fetchHistoryPage(historyCursor.value, true)
  } catch (error) {
    ElMessage.error(`加载更多历史失败: ${configChannelErrorMessage(error)}`)
  } finally {
    historyLoadingMore.value = false
  }
}

const copyReference = async (reference) => {
  try {
    await navigator.clipboard.writeText(reference)
    ElMessage.success(`已复制 ${reference}`)
  } catch {
    ElMessage.error('复制引用失败，请检查浏览器剪贴板权限')
  }
}

const openPreview = async (reference) => {
  previewReference.value = reference
  previewText.value = ''
  showPreview.value = true
  previewLoading.value = true
  try {
    const res = await api.resolveConfigChannel(reference)
    previewText.value = decodeUtf8Base64(res.data?.content_base64 || '')
  } catch (error) {
    ElMessage.error(`预览失败: ${configChannelErrorMessage(error)}`)
    showPreview.value = false
  } finally {
    previewLoading.value = false
  }
}

const downloadSnapshot = async (reference) => {
  try {
    const res = await api.resolveConfigChannel(reference)
    const snapshot = res.data
    const bytes = decodeConfigBytes(snapshot.content_base64)
    const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }))
    const link = document.createElement('a')
    link.href = url
    link.download = configDownloadName(snapshot.name, snapshot.revision, snapshot.format)
    document.body.append(link)
    link.click()
    link.remove()
    setTimeout(() => URL.revokeObjectURL(url), 0)
  } catch (error) {
    ElMessage.error(`下载失败: ${configChannelErrorMessage(error)}`)
  }
}

const restoreHistory = async (row) => {
  if (resolving.value) return
  resolving.value = true
  try {
    const res = await api.resolveConfigChannel(historyReference(row))
    const snapshot = res.data
    draft.value = {
      ...emptyDraft(),
      name: historyChannelName.value,
      format: snapshot.format,
      schema_id: snapshot.schema_id,
      content: decodeUtf8Base64(snapshot.content_base64),
      change_note: `基于 revision ${row.revision} 创建新版本`
    }
    baseRevision.value = row.revision
    expectedRevision.value = historyCurrentRevision.value
    publishKind.value = 'update'
    inputMode.value = 'editor'
    showHistory.value = false
    showPublishDialog.value = true
  } catch (error) {
    ElMessage.error(`载入历史版本失败: ${configChannelErrorMessage(error)}`)
  } finally {
    resolving.value = false
  }
}

onMounted(loadChannels)
</script>

<style scoped>
.config-toolbar { display: flex; justify-content: flex-end; gap: 12px; margin-bottom: 12px; }
.config-toolbar :deep(.el-alert) { flex: 1; }
.digest { font-family: monospace; }
.load-more { display: flex; justify-content: center; padding-top: 16px; }
.config-file-input { width: 100%; color: var(--text-primary); }
.form-hint { width: 100%; margin-top: 4px; color: var(--text-secondary); font-size: 12px; }
.preview-body { min-height: 80px; max-height: 60vh; overflow: auto; }
.preview-plain-text { font-family: monospace; white-space: pre-wrap; overflow-wrap: anywhere; }
</style>
