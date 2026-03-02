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
            <el-upload
              :action="uploadUrl"
              :on-success="handleUploadSuccess"
              :on-error="handleUploadError"
              :before-upload="beforeUpload"
              :show-file-list="false"
              accept=".py"
            >
              <el-button type="success">
                <el-icon><Upload /></el-icon>
                上传文件
              </el-button>
            </el-upload>
          </div>
        </div>
      </template>

      <el-table :data="operators" style="width: 100%" v-loading="loading">
        <el-table-column prop="name" label="名称" width="250" />
        <el-table-column prop="catelog" label="类别" width="150" />
        <el-table-column prop="position" label="位置" width="150">
          <template #default="scope">
            <el-tag :type="scope.row.position === 'builtin' ? 'info' : 'warning'">
              {{ scope.row.position }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="active" label="状态" width="100">
          <template #default="scope">
            <el-tag :type="scope.row.active ? 'success' : 'info'">
              {{ scope.row.active ? '活跃' : '未激活' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="200">
          <template #default="scope">
            <el-button
              v-if="!scope.row.active"
              type="success"
              size="small"
              @click="activateOperator(scope.row.catelog + '.' + scope.row.name)"
            >
              激活
            </el-button>
            <el-button
              v-else
              type="warning"
              size="small"
              @click="deactivateOperator(scope.row.catelog + '.' + scope.row.name)"
            >
              停用
            </el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 新建算子对话框 -->
    <el-dialog
      v-model="createDialogVisible"
      title="新建 Python 算子"
      width="900px"
      :close-on-click-modal="false"
    >
      <el-form :model="operatorForm" label-width="100px">
        <el-form-item label="算子名称" required>
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

        <el-form-item label="算子描述">
          <el-input
            v-model="operatorForm.description"
            placeholder="简要描述算子功能"
          />
        </el-form-item>

        <el-form-item label="依赖库">
          <el-input
            v-model="operatorForm.dependencies"
            placeholder="例如: scipy>=1.10.0, scikit-learn>=1.3.0 (可选)"
          />
          <div class="form-hint">
            已预装: pandas, numpy, scipy, scikit-learn, statsmodels
          </div>
        </el-form-item>

        <el-form-item label="Python 代码" required>
          <div class="code-editor-header">
            <el-button size="small" @click="insertTemplate">
              <el-icon><Document /></el-icon>
              插入模板
            </el-button>
            <el-button size="small" @click="insertExample">
              <el-icon><Tickets /></el-icon>
              插入示例
            </el-button>
          </div>
          <el-input
            v-model="operatorForm.code"
            type="textarea"
            :rows="20"
            placeholder="在此编写 Python 代码..."
            class="code-textarea"
          />
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
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { Upload, Edit, QuestionFilled, Document, Tickets } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const operators = ref([])
const loading = ref(false)
const uploadUrl = 'http://localhost:8081/api/operators/upload'
const createDialogVisible = ref(false)
const uploading = ref(false)

const operatorForm = ref({
  fullName: '',
  catelog: '',
  name: '',
  description: '',
  dependencies: '',
  code: ''
})

const loadOperators = async () => {
  loading.value = true
  try {
    const res = await api.getOperators()
    operators.value = res.data.map(op => ({
      ...op,
      active: op.active === '1' || op.active === 1
    }))
  } catch (error) {
    ElMessage.error('加载算子列表失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

const activateOperator = async (name) => {
  try {
    await api.activateOperator(name)
    ElMessage.success(`算子 ${name} 已激活`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('激活失败: ' + error.message)
  }
}

const deactivateOperator = async (name) => {
  try {
    await api.deactivateOperator(name)
    ElMessage.success(`算子 ${name} 已停用`)
    await loadOperators()
  } catch (error) {
    ElMessage.error('停用失败: ' + error.message)
  }
}

const beforeUpload = (file) => {
  const isPython = file.name.endsWith('.py')
  if (!isPython) {
    ElMessage.error('只能上传 .py 文件')
  }
  return isPython
}

const handleUploadSuccess = (response) => {
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
  createDialogVisible.value = true
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

    // 将代码转为 Blob
    const blob = new Blob([finalCode], { type: 'text/plain' })
    const file = new File([blob], fileName, { type: 'text/plain' })

    // 创建 FormData
    const formData = new FormData()
    formData.append('file', file)

    // 上传
    const res = await api.uploadOperator(file)
    ElMessage.success('算子上传成功')

    // 如果需要自动激活
    if (autoActivate) {
      const fullName = `${operatorForm.value.catelog}.${operatorForm.value.name}`
      await api.activateOperator(fullName)
      ElMessage.success('算子已激活')
    }

    // 关闭对话框并刷新列表
    createDialogVisible.value = false
    await loadOperators()
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
  max-width: 1400px;
}

.page-title {
  font-size: 24px;
  font-weight: 600;
  margin-bottom: 20px;
  color: #303133;
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

.code-textarea {
  font-family: 'Courier New', 'Consolas', monospace;
  font-size: 13px;
}

.code-textarea :deep(textarea) {
  font-family: 'Courier New', 'Consolas', monospace;
  font-size: 13px;
  line-height: 1.5;
}

.dialog-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
}
</style>
