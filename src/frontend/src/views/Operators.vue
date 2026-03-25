<template>
  <div class="operators">
    <h1 class="page-title">算子管理</h1>

    <el-card>
      <template #header>
        <div class="card-header">
          <span>算子列表</span>
          <div class="header-buttons">
            <el-button type="primary" @click="showCreateDialog">
              <el-icon><Edit /></el-icon>
              新建算子
            </el-button>
            <el-button type="success" @click="triggerFileInput">
                <el-icon><Upload /></el-icon>
                上传文件
              </el-button>
              <input
                ref="fileInputRef"
                type="file"
                accept=".py"
                style="display:none"
                @change="handleFileChange"
              />
          </div>
        </div>
      </template>

      <el-table :data="operators" style="width: 100%" v-loading="loading">
        <el-table-column prop="name" label="名称" width="250" />
        <el-table-column prop="catelog" label="类别" width="150" />
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
        <el-table-column label="操作" width="260">
          <template #default="scope">
            <template v-if="scope.row.active">
              <el-button
                type="warning"
                size="small"
                @click="deactivateOperator(scope.row.catelog + '.' + scope.row.name)"
              >
                去激活
              </el-button>
              <el-button
                type="primary"
                size="small"
                @click="viewOperator(scope.row)"
              >
                查看
              </el-button>
            </template>
            <template v-else>
              <el-button
                type="success"
                size="small"
                @click="activateOperator(scope.row.catelog + '.' + scope.row.name)"
              >
                激活
              </el-button>
              <el-button
                type="primary"
                size="small"
                @click="editOperator(scope.row)"
              >
                编辑
              </el-button>
            </template>
          </template>
        </el-table-column>
      </el-table>
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
            类别: <el-tag size="small">{{ operatorForm.catelog || '未设置' }}</el-tag>
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
      :title="detailMode === 'view' ? '查看算子' : '编辑算子'"
      :width="detailDialogFullscreen ? '100%' : '900px'"
      :fullscreen="detailDialogFullscreen"
      :close-on-click-modal="false"
      @closed="onDetailDialogClosed"
    >
      <el-form :model="detailForm" :label-width="showDetailMeta ? '100px' : '0px'">
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
import { Upload, Edit, QuestionFilled, Document, Tickets, FullScreen } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const operators = ref([])
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
  catelog: '',
  name: '',
  description: '',
  dependencies: '',
  code: ''
})

const detailForm = ref({
  fullName: '',
  catelog: '',
  name: '',
  description: '',
  position: 'DATA',
  source: '',
  type: '',
  editable: false,
  code: ''
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

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

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

const loadOperators = async () => {
  loading.value = true
  try {
    const res = await api.getOperators()
    const list = Array.isArray(res.data)
      ? res.data
      : (Array.isArray(res.data?.operators) ? res.data.operators : [])
    operators.value = list.map(op => ({
      ...op,
      active: op.active === '1' || op.active === 1
    }))
  } catch (error) {
    ElMessage.error('加载算子列表失败: ' + (error.response?.data?.error || error.message))
  } finally {
    loading.value = false
  }
}

const waitOperatorSynced = async (fullName, maxRetries = 10, delayMs = 300) => {
  for (let i = 0; i < maxRetries; i += 1) {
    await loadOperators()
    if (!fullName || operators.value.some(op => `${op.catelog}.${op.name}` === fullName)) {
      return true
    }
    await delay(delayMs)
  }
  return false
}

const activateOperator = async (name) => {
  try {
    await api.activateOperator(name)
    ElMessage.success(`算子 ${name} 已激活`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('激活失败: ' + (error.response?.data?.error || error.message))
  }
}

const deactivateOperator = async (name) => {
  try {
    await api.deactivateOperator(name)
    ElMessage.success(`算子 ${name} 已去激活`)
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
    const res = await api.getOperatorDetail(fullName)
    const d = res.data || {}
    detailForm.value = {
      fullName: `${d.catelog}.${d.name}`,
      catelog: d.catelog || '',
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
  await openDetail(`${row.catelog}.${row.name}`, 'view')
}

const editOperator = async (row) => {
  await openDetail(`${row.catelog}.${row.name}`, 'edit')
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
  if (!file.name.endsWith('.py')) {
    ElMessage.error('只能上传 .py 文件')
    return
  }
  try {
    const content = await file.text()
    await api.uploadOperator(file.name, content)
    ElMessage.success('算子上传成功')
    await waitOperatorSynced('')
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
    catelog: '',
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
    operatorForm.value.catelog = parts[0]
    operatorForm.value.name = parts[1]
  } else {
    operatorForm.value.catelog = ''
    operatorForm.value.name = ''
  }
}

const insertTemplate = () => {
  const template = `from flowsql import OperatorBase, register_operator
import pandas as pd

@register_operator(
    catelog="${operatorForm.value.catelog || 'your_catelog'}",
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
    catelog="${operatorForm.value.catelog || 'explore'}",
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
  if (!operatorForm.value.fullName || !operatorForm.value.catelog || !operatorForm.value.name) {
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
    const fileName = `${operatorForm.value.catelog}_${operatorForm.value.name}.py`

    // 上传（JSON base64）
    const res = await api.uploadOperator(fileName, finalCode)
    ElMessage.success('算子上传成功')

    const fullName = `${operatorForm.value.catelog}.${operatorForm.value.name}`
    const synced = await waitOperatorSynced(fullName)
    if (!synced) {
      ElMessage.warning('算子文件已上传，但目录同步较慢，请稍后在列表中确认')
    }

    // 如果需要自动激活
    if (autoActivate) {
      await api.activateOperator(fullName)
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

onMounted(() => {
  loadOperators()
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
}

.header-buttons {
  display: flex;
  gap: 10px;
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
</style>
