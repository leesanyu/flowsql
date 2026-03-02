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
        </div>
      </template>

      <el-table :data="filteredChannels" style="width: 100%" v-loading="loading">
        <el-table-column prop="name" label="名称" width="200" />
        <el-table-column prop="catelog" label="类别" width="150" />
        <el-table-column prop="type" label="类型" width="150" />
        <el-table-column label="Schema" min-width="300">
          <template #default="scope">
            <el-popover placement="top" :width="400" trigger="hover">
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
      </el-table>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { Search } from '@element-plus/icons-vue'
import api from '../api'
import { ElMessage } from 'element-plus'

const channels = ref([])
const searchText = ref('')
const loading = ref(false)

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
    // 解析 schema_json 字段
    channels.value = res.data.map(ch => ({
      ...ch,
      schema: JSON.parse(ch.schema_json || '[]').map(field => ({
        name: field.name,
        type: getTypeName(field.type)
      }))
    }))
  } catch (error) {
    ElMessage.error('加载通道列表失败: ' + error.message)
  } finally {
    loading.value = false
  }
}

// 类型映射（根据 DataType 枚举）
const getTypeName = (typeId) => {
  const typeMap = {
    0: 'BOOL', 1: 'INT8', 2: 'UINT32', 3: 'UINT64',
    4: 'INT32', 5: 'INT64', 6: 'STRING', 7: 'DOUBLE'
  }
  return typeMap[typeId] || 'UNKNOWN'
}

onMounted(() => {
  loadChannels()
})
</script>

<style scoped>
.channels {
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

.schema-popup {
  max-height: 300px;
  overflow-y: auto;
}

.schema-field {
  display: flex;
  justify-content: space-between;
  padding: 5px 0;
  border-bottom: 1px solid #eee;
}

.schema-field:last-child {
  border-bottom: none;
}

.field-name {
  font-weight: 600;
  color: #303133;
}

.field-type {
  color: #909399;
  font-family: monospace;
}
</style>
