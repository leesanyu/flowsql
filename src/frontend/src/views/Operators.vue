<template>
  <div class="operators">
    <h1 class="page-title">算子管理</h1>

    <el-card>
      <template #header>
        <div class="card-header">
          <div class="header-left">
            <el-input
              v-model="searchText"
              placeholder="搜索算子名称"
              style="width: 300px"
              clearable
            >
              <template #prefix>
                <el-icon><Search /></el-icon>
              </template>
            </el-input>
          </div>
          <div class="header-buttons">
            <el-button v-if="operatorType === 'python'" type="primary" @click="showCreateDialog">
              <el-icon><Edit /></el-icon>
              新建算子
            </el-button>
            <el-button v-if="operatorType !== 'builtin'" type="success" @click="triggerFileInput">
                <el-icon><Upload /></el-icon>
                上传文件
              </el-button>
              <input v-if="operatorType !== 'builtin'"
                ref="fileInputRef"
                type="file"
                :accept="uploadAccept"
                style="display:none"
                @change="handleFileChange"
              />
          </div>
        </div>
      </template>

      <div class="operator-layout">
        <div class="operator-sidebar">
          <div
            class="operator-type-item"
            :class="{ active: operatorType === 'builtin' }"
            @click="switchOperatorType('builtin')"
          >
            <span>内置算子</span>
            <el-tag size="small" effect="plain">{{ operatorCountText('builtin') }}</el-tag>
          </div>
          <div
            class="operator-type-item"
            :class="{ active: operatorType === 'python' }"
            @click="switchOperatorType('python')"
          >
            <span>Python 算子</span>
            <el-tag size="small" effect="plain">{{ operatorCountText('python') }}</el-tag>
          </div>
          <div
            class="operator-type-item"
            :class="{ active: operatorType === 'cpp' }"
            @click="switchOperatorType('cpp')"
          >
            <span>C++ 插件</span>
            <el-tag size="small" effect="plain">{{ operatorCountText('cpp') }}</el-tag>
          </div>
        </div>

        <div class="operator-main">
          <div class="section-title">{{ operatorTypeLabel }}</div>

          <el-table v-if="operatorType !== 'cpp'" :data="filteredOperators" style="width: 100%" v-loading="loading">
            <el-table-column prop="name" label="名称" width="250" />
            <el-table-column label="类别" width="150">
              <template #default="scope">
                {{ scope.row.category }}
              </template>
            </el-table-column>
            <el-table-column prop="position" label="位置" width="150">
              <template #default="scope">
                <el-tag :type="scope.row.position === 'STORAGE' ? 'warning' : 'info'">
                  {{ formatPosition(scope.row.position) }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column prop="active" label="状态" width="100">
              <template #default="scope">
                <el-tag :type="scope.row.active ? 'success' : 'info'">
                  {{ scope.row.active ? '激活' : '未激活' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="320">
              <template #default="scope">
                <template v-if="scope.row.active">
                  <div class="operator-action-group">
                    <el-button
                      class="operator-action-link"
                      type="warning"
                      size="small"
                      text
                      @click="deactivateOperator(scope.row)"
                    >
                      去激活
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      type="primary"
                      size="small"
                      text
                      @click="viewOperator(scope.row)"
                    >
                      查看
                    </el-button>
                  </div>
                </template>
                <template v-else>
                  <div class="operator-action-group">
                    <el-button
                      class="operator-action-link"
                      type="success"
                      size="small"
                      text
                      @click="activateOperator(scope.row)"
                    >
                      激活
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      type="primary"
                      size="small"
                      text
                      @click="editOperator(scope.row)"
                    >
                      编辑
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      v-if="operatorType === 'python'"
                      type="danger"
                      size="small"
                      text
                      @click="deletePythonOperator(scope.row)"
                    >
                      删除
                    </el-button>
                  </div>
                </template>
              </template>
            </el-table-column>
          </el-table>

          <el-table v-else :data="filteredOperators" style="width: 100%" v-loading="loading">
            <el-table-column prop="plugin.so_file" label="插件名" min-width="180" />
            <el-table-column prop="plugin.size_bytes" label="大小(bytes)" width="130" />
            <el-table-column label="状态" width="120">
              <template #default="scope">
                <el-tag :type="scope.row.plugin?.status === 'activated' ? 'success' : (scope.row.plugin?.status === 'broken' ? 'danger' : 'info')">
                  {{ scope.row.plugin?.status || '-' }}
                </el-tag>
              </template>
            </el-table-column>
            <el-table-column label="算子数量" width="100">
              <template #default="scope">
                {{ scope.row.plugin?.operator_count ?? '-' }}
              </template>
            </el-table-column>
            <el-table-column label="算子列表" min-width="180">
              <template #default="scope">
                <div v-if="Array.isArray(scope.row.plugin?.operators) && scope.row.plugin.operators.length > 0" class="cpp-operator-lines">
                  <div
                    v-for="op in scope.row.plugin.operators"
                    :key="op"
                    class="cpp-operator-line"
                  >
                    {{ op }}
                  </div>
                </div>
                <span v-else>-</span>
              </template>
            </el-table-column>
            <el-table-column label="操作" width="300">
              <template #default="scope">
                <template v-if="scope.row.plugin?.status === 'activated'">
                  <div class="operator-action-group">
                    <el-button
                      class="operator-action-link"
                      type="warning"
                      size="small"
                      text
                      @click="deactivateOperator(scope.row)"
                    >
                      去激活
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      type="primary"
                      size="small"
                      text
                      @click="viewOperator(scope.row)"
                    >
                      查看
                    </el-button>
                  </div>
                </template>
                <template v-else>
                  <div class="operator-action-group">
                    <el-button
                      class="operator-action-link"
                      type="success"
                      size="small"
                      text
                      @click="activateOperator(scope.row)"
                    >
                      激活
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      type="primary"
                      size="small"
                      text
                      @click="viewOperator(scope.row)"
                    >
                      查看
                    </el-button>
                    <el-button
                      class="operator-action-link"
                      type="danger"
                      size="small"
                      text
                      @click="deleteCppPlugin(scope.row)"
                    >
                      删除
                    </el-button>
                  </div>
                </template>
              </template>
            </el-table-column>
          </el-table>
        </div>
      </div>
    </el-card>

    <!-- 新建算子对话框 -->
    <el-dialog
      v-model="createDialogVisible"
      title="新建 Python 算子"
      :width="createDialogFullscreen ? '100%' : '900px'"
      :fullscreen="createDialogFullscreen"
      :close-on-click-modal="false"
      @closed="onCreateDialogClosed"
    >
      <el-form :model="operatorForm" :label-width="showCreateMeta ? '100px' : '0px'">
        <el-form-item v-if="showCreateMeta" label="算子名称" required>
          <el-input
            v-model="operatorForm.fullName"
            placeholder="例如: explore.chisquare"
            @input="parseOperatorName"
          >
            <template #append>
              <el-tooltip content="格式: 类别.名称" placement="top">
                <el-icon><QuestionFilled /></el-icon>
              </el-tooltip>
            </template>
          </el-input>
          <div class="form-hint">
            类别: <el-tag size="small">{{ operatorForm.category || '未设置' }}</el-tag>
            名称: <el-tag size="small">{{ operatorForm.name || '未设置' }}</el-tag>
          </div>
        </el-form-item>

        <el-form-item v-if="showCreateMeta" label="算子描述">
          <el-input
            v-model="operatorForm.description"
            placeholder="简要描述算子功能"
          />
        </el-form-item>

        <el-form-item v-if="showCreateMeta" label="依赖库">
          <el-input
            v-model="operatorForm.dependencies"
            placeholder="例如: scipy>=1.10.0, scikit-learn>=1.3.0 (可选)"
          />
          <div class="form-hint">
            已预装: pandas, numpy, scipy, scikit-learn, statsmodels
          </div>
        </el-form-item>

        <el-form-item
          :label="showCreateMeta ? 'Python 代码' : ''"
          required
          :class="['python-code-form-item', { 'python-code-form-item-fullscreen': createDialogFullscreen }]"
        >
          <div class="code-editor-header">
            <el-button size="small" @click="insertTemplate">
              <el-icon><Document /></el-icon>
              插入模板
            </el-button>
            <el-button size="small" @click="insertExample">
              <el-icon><Tickets /></el-icon>
              插入示例
            </el-button>
            <el-button size="small" @click="toggleCreateFullscreen">
              <el-icon><FullScreen /></el-icon>
              {{ createDialogFullscreen ? '退出最大化' : '最大化编写' }}
            </el-button>
          </div>
          <div
            :class="['code-editor-container', 'code-editor-container-editable']"
            :style="createCodeContainerStyle"
          >
            <div ref="createCodeLineNumbersRef" class="code-line-numbers code-line-numbers-editor">
              <div v-for="line in createCodeLines" :key="line">{{ line }}</div>
            </div>
            <div class="code-editor-stage">
              <pre ref="createCodeHighlightRef" class="code-content code-highlight-layer"><code v-html="highlightedCreateCode"></code></pre>
              <textarea
                ref="createCodeTextareaRef"
                v-model="operatorForm.code"
                class="code-editor-input"
                wrap="off"
                spellcheck="false"
                @scroll="syncCreateEditorScroll"
                @input="syncCreateEditorScroll"
                @keyup="syncCreateEditorScroll"
                @click="syncCreateEditorScroll"
              ></textarea>
            </div>
          </div>
        </el-form-item>
      </el-form>

      <template #footer>
        <span class="dialog-footer">
          <el-button @click="createDialogVisible = false">取消</el-button>
          <el-button type="primary" @click="saveOperator(false)" :loading="uploading">
            保存并上传
          </el-button>
          <el-button type="success" @click="saveOperator(true)" :loading="uploading">
            保存并激活
          </el-button>
        </span>
      </template>
    </el-dialog>

    <el-dialog
      v-model="detailDialogVisible"
      :title="isCppDetail ? '查看 C++ 插件详情' : (detailMode === 'view' ? '查看算子' : '编辑算子')"
      :width="detailDialogFullscreen ? '100%' : '900px'"
      :fullscreen="detailDialogFullscreen"
      :close-on-click-modal="false"
      @closed="onDetailDialogClosed"
    >
      <el-form :model="detailForm" :label-width="showDetailMeta ? '100px' : '0px'">
        <template v-if="isCppDetail">
          <el-form-item label="插件文件">
            <el-input :model-value="cppDetail.so_file || '-'" disabled />
          </el-form-item>
          <el-form-item label="Plugin ID">
            <el-input :model-value="cppDetail.plugin_id || '-'" disabled class="mono-input" />
          </el-form-item>
          <el-form-item label="状态">
            <el-tag :type="cppDetail.status === 'activated' ? 'success' : (cppDetail.status === 'broken' ? 'danger' : 'info')">
              {{ cppDetail.status || '-' }}
            </el-tag>
          </el-form-item>
          <el-form-item label="大小(bytes)">
            <el-input :model-value="cppDetail.size_bytes ?? '-'" disabled />
          </el-form-item>
          <el-form-item label="ABI版本">
            <el-input :model-value="cppDetail.abi_version ?? '-'" disabled />
          </el-form-item>
          <el-form-item label="算子数量">
            <el-input :model-value="cppDetail.operator_count ?? '-'" disabled />
          </el-form-item>
          <el-form-item label="SHA256">
            <el-input :model-value="cppDetail.sha256 || '-'" disabled class="mono-input" />
          </el-form-item>
          <el-form-item label="算子列表">
            <div class="cpp-op-list">
              <el-tag v-for="name in cppDetail.operators" :key="name" type="info">{{ name }}</el-tag>
              <span v-if="!Array.isArray(cppDetail.operators) || cppDetail.operators.length === 0">-</span>
            </div>
          </el-form-item>
          <el-form-item label="错误信息">
            <el-input :model-value="cppDetail.last_error || '-'" type="textarea" :rows="3" disabled />
          </el-form-item>
        </template>
        <template v-else>
          <el-form-item v-if="showDetailMeta" label="算子名称">
            <el-input v-model="detailForm.fullName" disabled />
          </el-form-item>
          <el-form-item v-if="showDetailMeta" label="算子描述">
            <el-input v-model="detailForm.description" :disabled="detailMode === 'view'" />
          </el-form-item>
          <el-form-item v-if="showDetailMeta" label="位置">
            <el-input :model-value="formatPosition(detailForm.position)" disabled />
          </el-form-item>
          <el-form-item v-if="showDetailMeta" label="来源">
            <el-input v-model="detailForm.source" disabled />
          </el-form-item>
          <el-form-item v-if="showDetailMeta" label="类型">
            <el-input v-model="detailForm.type" disabled />
          </el-form-item>
          <el-form-item
            v-if="isPythonDetail"
            :label="showDetailMeta ? 'Python 代码' : ''"
            :class="['python-code-form-item', { 'python-code-form-item-fullscreen': detailDialogFullscreen }]"
          >
            <div class="code-viewer-header">
              <el-tag size="small" type="success">Python</el-tag>
              <el-button size="small" @click="toggleDetailFullscreen">
                <el-icon><FullScreen /></el-icon>
                {{ detailDialogFullscreen ? '退出最大化' : '最大化查看' }}
              </el-button>
            </div>
            <template v-if="detailMode === 'view'">
              <div
                :class="['code-editor-container', 'code-editor-container-readonly']"
                :style="detailCodeContainerStyle"
              >
                <div ref="detailCodeReadonlyLineNumbersRef" class="code-line-numbers code-line-numbers-readonly">
                  <div v-for="line in detailCodeLines" :key="line">{{ line }}</div>
                </div>
                <pre
                  ref="detailCodeReadonlyContentRef"
                  class="code-content code-content-readonly"
                  @scroll="syncReadonlyScroll"
                ><code v-html="highlightedDetailCode"></code></pre>
              </div>
            </template>
            <template v-else>
              <div
                :class="['code-editor-container', 'code-editor-container-editable']"
                :style="detailCodeContainerStyle"
              >
                <div ref="detailCodeLineNumbersRef" class="code-line-numbers code-line-numbers-editor">
                  <div v-for="line in detailCodeLines" :key="line">{{ line }}</div>
                </div>
                <div class="code-editor-stage">
                  <pre ref="detailCodeHighlightRef" class="code-content code-highlight-layer"><code v-html="highlightedDetailCode"></code></pre>
                  <textarea
                    ref="detailCodeTextareaRef"
                    v-model="detailForm.code"
                    class="code-editor-input"
                    wrap="off"
                    spellcheck="false"
                    @scroll="syncEditorScroll"
                    @input="syncEditorScroll"
                    @keyup="syncEditorScroll"
                    @click="syncEditorScroll"
                  ></textarea>
                </div>
              </div>
            </template>
          </el-form-item>
        </template>
      </el-form>
      <template #footer>
        <span class="dialog-footer">
          <el-button @click="detailDialogVisible = false">关闭</el-button>
          <el-button
            v-if="detailMode === 'edit'"
            type="primary"
            :loading="uploading"
            @click="saveOperatorEdit"
          >
            保存
          </el-button>
        </span>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { computed, nextTick, ref, onMounted } from 'vue'
import { Upload, Edit, QuestionFilled, Document, Tickets, FullScreen, Search } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const operators = ref([])
const searchText = ref('')
const operatorType = ref('python')
const operatorCounts = ref({ builtin: null, python: null, cpp: null })
const loading = ref(false)
const fileInputRef = ref(null)
const createDialogVisible = ref(false)
const createDialogFullscreen = ref(false)
const detailDialogVisible = ref(false)
const detailMode = ref('view')
const uploading = ref(false)
const detailDialogFullscreen = ref(false)
const createCodeTextareaRef = ref(null)
const createCodeHighlightRef = ref(null)
const createCodeLineNumbersRef = ref(null)
const detailCodeTextareaRef = ref(null)
const detailCodeHighlightRef = ref(null)
const detailCodeLineNumbersRef = ref(null)
const detailCodeReadonlyContentRef = ref(null)
const detailCodeReadonlyLineNumbersRef = ref(null)

const operatorForm = ref({
  fullName: '',
  category: '',
  name: '',
  description: '',
  dependencies: '',
  code: ''
})

const detailForm = ref({
  fullName: '',
  category: '',
  name: '',
  description: '',
  position: 'DATA',
  source: '',
  type: '',
  editable: false,
  code: ''
})
const cppDetail = ref({
  plugin_id: '',
  so_file: '',
  status: '',
  size_bytes: null,
  sha256: '',
  abi_version: null,
  operator_count: null,
  operators: [],
  last_error: ''
})

const PYTHON_KEYWORDS = new Set([
  'and', 'as', 'assert', 'async', 'await', 'break', 'class', 'continue', 'def', 'del',
  'elif', 'else', 'except', 'finally', 'for', 'from', 'global', 'if', 'import', 'in',
  'is', 'lambda', 'nonlocal', 'not', 'or', 'pass', 'raise', 'return', 'try', 'while',
  'with', 'yield'
])

const PYTHON_LITERALS = new Set(['True', 'False', 'None'])

const escapeHtml = (text = '') => {
  return text
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

const highlightPythonLine = (line) => {
  let output = ''
  let index = 0
  while (index < line.length) {
    const ch = line[index]

    if (ch === '#') {
      output += `<span class="token-comment">${escapeHtml(line.slice(index))}</span>`
      break
    }

    if (ch === '"' || ch === "'") {
      const quote = ch
      let end = index + 1
      while (end < line.length) {
        if (line[end] === '\\' && end + 1 < line.length) {
          end += 2
          continue
        }
        if (line[end] === quote) {
          end += 1
          break
        }
        end += 1
      }
      output += `<span class="token-string">${escapeHtml(line.slice(index, end))}</span>`
      index = end
      continue
    }

    if (/[A-Za-z_]/.test(ch)) {
      let end = index + 1
      while (end < line.length && /[A-Za-z0-9_]/.test(line[end])) {
        end += 1
      }
      const word = line.slice(index, end)
      if (PYTHON_KEYWORDS.has(word)) {
        output += `<span class="token-keyword">${word}</span>`
      } else if (PYTHON_LITERALS.has(word)) {
        output += `<span class="token-literal">${word}</span>`
      } else {
        output += escapeHtml(word)
      }
      index = end
      continue
    }

    if (/[0-9]/.test(ch)) {
      let end = index + 1
      while (end < line.length && /[0-9._]/.test(line[end])) {
        end += 1
      }
      output += `<span class="token-number">${escapeHtml(line.slice(index, end))}</span>`
      index = end
      continue
    }

    output += escapeHtml(ch)
    index += 1
  }
  return output
}

const highlightPythonCode = (code = '') => {
  return code
    .replace(/\r\n/g, '\n')
    .split('\n')
    .map((line) => highlightPythonLine(line))
    .join('\n')
}

const isPythonDetail = computed(() => String(detailForm.value.type || '').toLowerCase() === 'python')
const isCppDetail = computed(() => String(detailForm.value.type || '').toLowerCase() === 'cpp')
const showDetailMeta = computed(() => !(detailDialogFullscreen.value && isPythonDetail.value))
const detailCodeContainerStyle = computed(() => (
  detailDialogFullscreen.value
    ? { height: 'calc(100vh - 150px)' }
    : { height: '360px' }
))

const detailCodeLines = computed(() => {
  const code = (detailForm.value.code || '').replace(/\r\n/g, '\n')
  const count = code.length === 0 ? 1 : code.split('\n').length
  return Array.from({ length: count }, (_, idx) => idx + 1)
})

const highlightedDetailCode = computed(() => highlightPythonCode(detailForm.value.code || ''))
const highlightedCreateCode = computed(() => highlightPythonCode(operatorForm.value.code || ''))
const createCodeLines = computed(() => {
  const code = (operatorForm.value.code || '').replace(/\r\n/g, '\n')
  const count = code.length === 0 ? 1 : code.split('\n').length
  return Array.from({ length: count }, (_, idx) => idx + 1)
})
const showCreateMeta = computed(() => !createDialogFullscreen.value)
const createCodeContainerStyle = computed(() => (
  createDialogFullscreen.value
    ? { height: 'calc(100vh - 180px)' }
    : { height: '420px' }
))
const operatorTypeLabel = computed(() => {
  if (operatorType.value === 'builtin') return '内置算子'
  if (operatorType.value === 'cpp') return 'C++ 插件'
  return 'Python 算子'
})
const uploadAccept = computed(() => (operatorType.value === 'cpp' ? '.so' : '.py'))

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms))
const OPERATOR_TYPES = ['builtin', 'python', 'cpp']

const filteredOperators = computed(() => {
  const list = Array.isArray(operators.value) ? operators.value : []
  const keyword = searchText.value.trim().toLowerCase()
  if (!keyword) return list

  if (operatorType.value === 'cpp') {
    return list.filter((op) => {
      const pluginId = String(op.plugin_id || '').toLowerCase()
      const soFile = String(op.plugin?.so_file || '').toLowerCase()
      const names = Array.isArray(op.plugin?.operators) ? op.plugin.operators : []
      if (pluginId.includes(keyword) || soFile.includes(keyword)) return true
      return names.some((name) => String(name).toLowerCase().includes(keyword))
    })
  }

  return list.filter((op) => {
    const category = String(op.category || '').toLowerCase()
    const name = String(op.name || '').toLowerCase()
    const fullName = `${category}.${name}`
    const description = String(op.description || '').toLowerCase()
    return category.includes(keyword) ||
      name.includes(keyword) ||
      fullName.includes(keyword) ||
      description.includes(keyword)
  })
})

const extractOperatorList = (res) => {
  return Array.isArray(res.data)
    ? res.data
    : (Array.isArray(res.data?.operators) ? res.data.operators : [])
}

const syncEditorScroll = () => {
  const textarea = detailCodeTextareaRef.value
  if (!textarea) return
  const highlight = detailCodeHighlightRef.value
  const lineNumbers = detailCodeLineNumbersRef.value
  if (highlight) {
    highlight.scrollTop = textarea.scrollTop
    highlight.scrollLeft = textarea.scrollLeft
  }
  if (lineNumbers) {
    lineNumbers.scrollTop = textarea.scrollTop
  }
}

const syncCreateEditorScroll = () => {
  const textarea = createCodeTextareaRef.value
  if (!textarea) return
  const highlight = createCodeHighlightRef.value
  const lineNumbers = createCodeLineNumbersRef.value
  if (highlight) {
    highlight.scrollTop = textarea.scrollTop
    highlight.scrollLeft = textarea.scrollLeft
  }
  if (lineNumbers) {
    lineNumbers.scrollTop = textarea.scrollTop
  }
}

const syncReadonlyScroll = () => {
  const readonlyContent = detailCodeReadonlyContentRef.value
  const readonlyLineNumbers = detailCodeReadonlyLineNumbersRef.value
  if (!readonlyContent || !readonlyLineNumbers) return
  readonlyLineNumbers.scrollTop = readonlyContent.scrollTop
}

const resetCodeScroll = () => {
  const createTextarea = createCodeTextareaRef.value
  if (createTextarea) {
    createTextarea.scrollTop = 0
    createTextarea.scrollLeft = 0
    syncCreateEditorScroll()
  }
  const textarea = detailCodeTextareaRef.value
  if (textarea) {
    textarea.scrollTop = 0
    textarea.scrollLeft = 0
    syncEditorScroll()
  }
  const readonlyContent = detailCodeReadonlyContentRef.value
  if (readonlyContent) {
    readonlyContent.scrollTop = 0
    readonlyContent.scrollLeft = 0
    syncReadonlyScroll()
  }
}

const loadOperators = async (currentType = operatorType.value) => {
  loading.value = true
  try {
    const res = await api.getOperators(currentType)
    const list = extractOperatorList(res)
    const mappedList = list.map((op) => {
      if (currentType === 'cpp') {
        return {
          ...op,
          active: op.active === '1' || op.active === 1,
          plugin_id: op.plugin_id || '',
          plugin: op.plugin || {}
        }
      }
      return {
        ...op,
        active: op.active === '1' || op.active === 1
      }
    })
    operatorCounts.value = {
      ...operatorCounts.value,
      [currentType]: mappedList.length
    }
    operators.value = mappedList
  } catch (error) {
    ElMessage.error('加载算子列表失败: ' + (error.response?.data?.error || error.message))
  } finally {
    loading.value = false
  }
}

const refreshOperatorCounts = async () => {
  const settled = await Promise.allSettled(
    OPERATOR_TYPES.map(async (type) => {
      const res = await api.getOperators(type)
      return { type, count: extractOperatorList(res).length }
    })
  )
  const next = { ...operatorCounts.value }
  for (const item of settled) {
    if (item.status !== 'fulfilled') continue
    next[item.value.type] = item.value.count
  }
  operatorCounts.value = next
}

const switchOperatorType = async (nextType) => {
  if (nextType === operatorType.value) return
  operatorType.value = nextType
  await loadOperators()
}

const operatorCountText = (type) => {
  const v = operatorCounts.value[type]
  return Number.isFinite(v) ? v : '-'
}

const waitOperatorSynced = async (fullName, maxRetries = 10, delayMs = 300) => {
  if (operatorType.value !== 'python') {
    await loadOperators()
    return true
  }
  for (let i = 0; i < maxRetries; i += 1) {
    await loadOperators()
    if (!fullName || operators.value.some(op => `${op.category}.${op.name}` === fullName)) {
      return true
    }
    await delay(delayMs)
  }
  return false
}

const activateOperator = async (row) => {
  const isCpp = operatorType.value === 'cpp'
  const payload = isCpp
    ? { type: 'cpp', plugin_id: row.plugin_id }
    : { type: operatorType.value, name: `${row.category}.${row.name}` }
  const label = isCpp ? row.plugin?.so_file || row.plugin_id : payload.name
  try {
    await api.activateOperator(payload)
    ElMessage.success(`算子 ${label} 已激活`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('激活失败: ' + (error.response?.data?.error || error.message))
  }
}

const deactivateOperator = async (row) => {
  const isCpp = operatorType.value === 'cpp'
  const payload = isCpp
    ? { type: 'cpp', plugin_id: row.plugin_id }
    : { type: operatorType.value, name: `${row.category}.${row.name}` }
  const label = isCpp ? row.plugin?.so_file || row.plugin_id : payload.name
  try {
    await api.deactivateOperator(payload)
    ElMessage.success(`算子 ${label} 已去激活`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('去激活失败: ' + (error.response?.data?.error || error.message))
  }
}

const formatPosition = (position) => {
  if (position === 'STORAGE') return '存储算子'
  return '数据算子'
}

const openDetail = async (fullName, mode) => {
  try {
    const res = await api.getOperatorDetail({ type: operatorType.value, name: fullName })
    const d = res.data || {}
    cppDetail.value = {
      plugin_id: '',
      so_file: '',
      status: '',
      size_bytes: null,
      sha256: '',
      abi_version: null,
      operator_count: null,
      operators: [],
      last_error: ''
    }
    detailForm.value = {
      fullName: `${d.category}.${d.name}`,
      category: d.category || '',
      name: d.name || '',
      description: d.description || '',
      position: d.position || 'DATA',
      source: d.source || '',
      type: d.type || '',
      editable: !!d.editable,
      code: d.content || ''
    }
    if (mode === 'edit' && !detailForm.value.editable) {
      ElMessage.warning('该算子不支持编辑（通常是内置算子）')
      return
    }
    detailMode.value = mode
    detailDialogFullscreen.value = false
    detailDialogVisible.value = true
    nextTick(() => {
      resetCodeScroll()
    })
  } catch (error) {
    ElMessage.error('获取算子详情失败: ' + (error.response?.data?.error || error.message))
  }
}

const openCppDetail = async (pluginId) => {
  try {
    const res = await api.getOperatorDetail({ type: 'cpp', plugin_id: pluginId })
    const d = res.data || {}
    const p = d.plugin || {}
    cppDetail.value = {
      plugin_id: d.plugin_id || pluginId,
      so_file: p.so_file || '',
      status: p.status || '',
      size_bytes: p.size_bytes ?? null,
      sha256: p.sha256 || '',
      abi_version: p.abi_version ?? null,
      operator_count: p.operator_count ?? null,
      operators: Array.isArray(p.operators) ? p.operators : [],
      last_error: p.last_error || ''
    }
    detailForm.value = {
      fullName: p.so_file || pluginId,
      category: 'cpp',
      name: pluginId,
      description: p.last_error || '',
      position: 'DATA',
      source: '',
      type: 'cpp',
      editable: false,
      code: ''
    }
    detailMode.value = 'view'
    detailDialogFullscreen.value = false
    detailDialogVisible.value = true
  } catch (error) {
    ElMessage.error('获取插件详情失败: ' + (error.response?.data?.error || error.message))
  }
}

const onDetailDialogClosed = () => {
  detailDialogFullscreen.value = false
}

const onCreateDialogClosed = () => {
  createDialogFullscreen.value = false
}

const toggleCreateFullscreen = () => {
  createDialogFullscreen.value = !createDialogFullscreen.value
  nextTick(() => {
    syncCreateEditorScroll()
  })
}

const toggleDetailFullscreen = () => {
  detailDialogFullscreen.value = !detailDialogFullscreen.value
  nextTick(() => {
    syncEditorScroll()
    syncReadonlyScroll()
  })
}

const viewOperator = async (row) => {
  if (operatorType.value === 'cpp') {
    await openCppDetail(row.plugin_id)
    return
  }
  await openDetail(`${row.category}.${row.name}`, 'view')
}

const editOperator = async (row) => {
  if (operatorType.value !== 'python') {
    ElMessage.warning('仅 Python 算子支持编辑')
    return
  }
  await openDetail(`${row.category}.${row.name}`, 'edit')
}

const saveOperatorEdit = async () => {
  if (!detailForm.value.fullName) return
  uploading.value = true
  try {
    await api.updateOperator(detailForm.value.fullName, {
      description: detailForm.value.description,
      content: detailForm.value.code
    })
    ElMessage.success('算子更新成功')
    detailDialogVisible.value = false
    await loadOperators()
  } catch (error) {
    ElMessage.error('更新失败: ' + (error.response?.data?.error || error.message))
  } finally {
    uploading.value = false
  }
}

const beforeUpload = (file) => {
  const isPython = file.name.endsWith('.py')
  if (!isPython) {
    ElMessage.error('只能上传 .py 文件')
  }
  return isPython
}

const triggerFileInput = () => {
  fileInputRef.value.click()
}

const handleFileChange = async (event) => {
  const file = event.target.files[0]
  if (!file) return
  const isCpp = operatorType.value === 'cpp'
  if (isCpp && !file.name.endsWith('.so')) {
    ElMessage.error('当前仅支持上传 .so 文件')
    return
  }
  if (!isCpp && !file.name.endsWith('.py')) {
    ElMessage.error('当前仅支持上传 .py 文件')
    return
  }
  try {
    if (isCpp) {
      await api.uploadOperatorFile(file, 'cpp')
    } else {
      const content = await file.text()
      await api.uploadOperator(file.name, content, 'python')
    }
    ElMessage.success('算子上传成功')
    await loadOperators()
  } catch (error) {
    ElMessage.error('上传失败: ' + (error.response?.data?.error || error.message))
  } finally {
    event.target.value = ''
  }
}

const handleUploadSuccess = () => {
  ElMessage.success('算子上传成功')
  loadOperators()
}

const handleUploadError = (error) => {
  ElMessage.error('上传失败: ' + error.message)
}

const showCreateDialog = () => {
  operatorForm.value = {
    fullName: '',
    category: '',
    name: '',
    description: '',
    dependencies: '',
    code: ''
  }
  createDialogFullscreen.value = false
  createDialogVisible.value = true
  nextTick(() => {
    resetCodeScroll()
  })
}

const parseOperatorName = () => {
  const parts = operatorForm.value.fullName.split('.')
  if (parts.length === 2) {
    operatorForm.value.category = parts[0]
    operatorForm.value.name = parts[1]
  } else {
    operatorForm.value.category = ''
    operatorForm.value.name = ''
  }
}

const insertTemplate = () => {
  const template = `from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    category="${operatorForm.value.category || 'your_category'}",
    name="${operatorForm.value.name || 'your_name'}",
    description="${operatorForm.value.description || '算子描述'}"
)
class MyOperator(OperatorBase):
    """算子类 - 类名可以任意"""

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        """
        核心处理方法

        参数:
            df_in: 输入 DataFrame

        返回:
            输出 DataFrame
        """
        # 获取配置参数（来自 SQL 的 WITH 子句）
        # param1 = self.get_config('param1', 'default_value')

        # 在这里编写你的数据处理逻辑
        df_out = df_in.copy()

        return df_out
`
  operatorForm.value.code = template
}

const insertExample = () => {
  const example = `from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    category="${operatorForm.value.category || 'explore'}",
    name="${operatorForm.value.name || 'chisquare'}",
    description="${operatorForm.value.description || '卡方独立性检验'}"
)
class ChiSquareOperator(OperatorBase):
    """卡方检验示例算子"""

    def work(self, df_in: pd.DataFrame) -> pd.DataFrame:
        """对输入 DataFrame 的前两列执行卡方独立性检验"""
        try:
            from scipy.stats import chi2_contingency

            if df_in.shape[1] < 2:
                return pd.DataFrame({"error": ["需要至少 2 列"]})

            # 获取阈值参数
            threshold = float(self.get_config('threshold', '0.05'))

            # 构建列联表
            col1, col2 = df_in.columns[0], df_in.columns[1]
            contingency = pd.crosstab(df_in[col1], df_in[col2])

            # 执行卡方检验
            stat, p, dof, expected = chi2_contingency(contingency)

            # 返回结果
            return pd.DataFrame({
                "statistic": [stat],
                "p_value": [p],
                "dof": [dof],
                "significant": [p < threshold]
            })
        except ImportError:
            return pd.DataFrame({"error": ["scipy 未安装"]})
        except Exception as e:
            return pd.DataFrame({"error": [str(e)]})
`
  operatorForm.value.code = example
}

const saveOperator = async (autoActivate) => {
  // 验证
  if (!operatorForm.value.fullName || !operatorForm.value.category || !operatorForm.value.name) {
    ElMessage.warning('请输入算子名称（格式: 类别.名称）')
    return
  }
  if (!operatorForm.value.code.trim()) {
    ElMessage.warning('请编写算子代码')
    return
  }

  uploading.value = true
  try {
    // 添加依赖注释（如果有）
    let finalCode = operatorForm.value.code
    if (operatorForm.value.dependencies.trim()) {
      finalCode = `# DEPENDENCIES: ${operatorForm.value.dependencies}\n\n${finalCode}`
    }

    // 生成文件名
    const fileName = `${operatorForm.value.category}_${operatorForm.value.name}.py`

    // 上传（JSON base64）
    await api.uploadOperator(fileName, finalCode, 'python')
    ElMessage.success('算子上传成功')

    const fullName = `${operatorForm.value.category}.${operatorForm.value.name}`
    const synced = await waitOperatorSynced(fullName)
    if (!synced) {
      ElMessage.warning('算子文件已上传，但目录同步较慢，请稍后在列表中确认')
    }

    // 如果需要自动激活
    if (autoActivate) {
      await api.activateOperator({ type: 'python', name: fullName })
      ElMessage.success('算子已激活')
    }

    // 关闭对话框并刷新列表
    createDialogVisible.value = false
    await waitOperatorSynced('')
  } catch (error) {
    ElMessage.error('保存失败: ' + error.message)
  } finally {
    uploading.value = false
  }
}

const deleteCppPlugin = async (row) => {
  try {
    await api.deleteOperator({ type: 'cpp', plugin_id: row.plugin_id })
    ElMessage.success('插件已删除')
    await loadOperators()
  } catch (error) {
    ElMessage.error('删除失败: ' + (error.response?.data?.error || error.message))
  }
}

const deletePythonOperator = async (row) => {
  const fullName = `${row.category}.${row.name}`
  try {
    await api.deleteOperator({ type: 'python', name: fullName })
    ElMessage.success(`算子 ${fullName} 已删除`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('删除失败: ' + (error.response?.data?.error || error.message))
  }
}

onMounted(async () => {
  await Promise.allSettled([loadOperators(), refreshOperatorCounts()])
})
</script>

<style scoped>
.operators {
}

.page-title {
  font-size: 24px;
  font-weight: 600;
  margin-bottom: 20px;
  color: var(--text-primary);
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
}

.header-left {
  display: flex;
  align-items: center;
}

.header-buttons {
  display: flex;
  gap: 10px;
  align-items: center;
}

.operator-layout {
  display: flex;
  gap: 16px;
}

.operator-sidebar {
  width: 180px;
  flex-shrink: 0;
  border-right: 1px solid var(--border-color);
  padding-right: 12px;
}

.operator-type-item {
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

.operator-type-item:hover {
  background: var(--bg-secondary);
}

.operator-type-item.active {
  background: var(--sidebar-active-bg, #dbeafe);
  color: var(--accent);
  font-weight: 600;
}

.operator-main {
  flex: 1;
  min-width: 0;
}

.section-title {
  margin: 10px 0;
  font-weight: 600;
  color: var(--text-primary);
}

.mono {
  font-family: Consolas, 'Courier New', monospace;
  font-size: 12px;
}

.mono-input :deep(input) {
  font-family: Consolas, 'Courier New', monospace;
  font-size: 12px;
}

.cpp-op-list {
  width: 100%;
  min-height: 32px;
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  align-items: center;
}

.cpp-operator-lines {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.cpp-operator-line {
  line-height: 1.3;
  word-break: break-all;
}

.operator-action-group {
  display: flex;
  align-items: center;
  gap: 6px;
}

.operator-action-link {
  width: 64px;
  justify-content: center;
  margin-left: 0 !important;
  padding-left: 0;
  padding-right: 0;
}

.form-hint {
  font-size: 12px;
  color: #909399;
  margin-top: 5px;
}

.code-editor-header {
  display: flex;
  gap: 10px;
  margin-bottom: 10px;
}

.code-viewer-header {
  width: 100%;
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}

.python-code-form-item {
  width: 100%;
}

.python-code-form-item :deep(.el-form-item__content) {
  min-width: 0;
  width: 100%;
}

.python-code-form-item-fullscreen :deep(.el-form-item__content) {
  margin-left: 0 !important;
}

.code-editor-container {
  width: 100%;
  min-width: 0;
  display: flex;
  border: 1px solid #dcdfe6;
  border-radius: 8px;
  background: #202535;
  min-height: 420px;
}

.code-editor-container-readonly {
  overflow: hidden;
}

.code-editor-container-editable {
  overflow: hidden;
}

.code-line-numbers {
  flex: 0 0 56px;
  align-self: stretch;
  min-width: 56px;
  padding: 12px 8px;
  margin: 0;
  text-align: right;
  color: #8b95a5;
  background: #2b3143;
  border-right: 1px solid #40485d;
  font-family: 'Consolas', 'Courier New', monospace;
  font-size: 13px;
  line-height: 1.6;
  user-select: none;
  overflow: hidden;
}

.code-line-numbers-editor {
  border-right: 1px solid #40485d;
}

.code-editor-stage {
  position: relative;
  flex: 1;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}

.code-content {
  margin: 0;
  padding: 12px 16px;
  min-width: max-content;
  white-space: pre;
  color: #e8edf3;
  font-family: 'Consolas', 'Courier New', monospace;
  font-size: 13px;
  line-height: 1.6;
  tab-size: 4;
}

.code-content-readonly {
  flex: 1;
  min-width: 0;
  min-height: 0;
  overflow-x: auto;
  overflow-y: auto;
}

.code-content code {
  font-family: inherit;
}

:deep(.code-highlight-layer) {
  position: absolute;
  inset: 0;
  pointer-events: none;
  overflow: auto;
  z-index: 1;
  scrollbar-width: none;
  -ms-overflow-style: none;
}

:deep(.code-highlight-layer::-webkit-scrollbar) {
  display: none;
}

:deep(.code-highlight-layer code) {
  display: block;
  min-height: 100%;
}

.code-editor-input {
  position: absolute;
  inset: 0;
  display: block;
  width: 100%;
  height: 100%;
  z-index: 2;
  border: none;
  resize: none;
  margin: 0;
  padding: 12px 16px;
  background: transparent;
  color: transparent;
  -webkit-text-fill-color: transparent;
  caret-color: #e8edf3;
  font-family: 'Consolas', 'Courier New', monospace;
  font-size: 13px;
  line-height: 1.6;
  tab-size: 4;
  white-space: pre;
  word-break: normal;
  overflow-wrap: normal;
  overflow-x: scroll;
  overflow-y: scroll;
  outline: none;
}

.code-editor-input::selection {
  background: rgba(115, 208, 255, 0.35);
}

:deep(.token-keyword) {
  color: #ffb454;
  font-weight: 600;
}

:deep(.token-string) {
  color: #bae67e;
}

:deep(.token-number) {
  color: #73d0ff;
}

:deep(.token-literal) {
  color: #d4bfff;
}

:deep(.token-comment) {
  color: #6e7a8f;
}

:deep(.el-dialog.is-fullscreen .el-dialog__body) {
  overflow: hidden;
}

.dialog-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
}

@media (max-width: 900px) {
  .operator-layout {
    flex-direction: column;
    gap: 12px;
  }

  .operator-sidebar {
    width: 100%;
    border-right: 0;
    border-bottom: 1px solid var(--border-color);
    padding-right: 0;
    padding-bottom: 8px;
  }
}
</style>
