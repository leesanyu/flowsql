<template>
  <div class="semantic-value" :data-semantic-state="value?.state || 'unknown'">
    <span v-if="value?.state === 'declared'" class="meaning">{{ readable(value.value) }}</span>
    <span v-else>{{ states[value?.state || 'unknown'] }}</span>
    <small>{{ value?.state === 'declared' ? '已声明 · ' : '' }}{{ origins[value?.origin] || '来源未声明' }}</small>
    <details v-if="value?.state === 'declared' && value.value !== null && typeof value.value === 'object'"><summary>完整定义</summary><pre>{{ prettyJson(value) }}</pre></details>
  </div>
</template>
<script setup>
import { prettyJson, labels } from '../utils/dataSemantics.js'
const props = defineProps({ value: Object, fieldNames: { type: Object, default: () => ({}) } })
const states = { unknown: '未知', not_applicable: '不适用' }
const origins = { manual: '人工填写', imported: '导入', producer: '生产者声明', derived: '派生' }
function readable(value) {
  if (Array.isArray(value)) return value.length ? value.map(readable).join('；') : '空集合'
  if (value && typeof value === 'object') {
    if ('state' in value && 'value' in value) return value.state === 'declared' ? readable(value.value) : states[value.state]
    return Object.entries(value).map(([key, item]) => `${labels[key] || key}：${readable(item)}`).join('；')
  }
  return Object.hasOwn(props.fieldNames, value) ? props.fieldNames[value] : String(value ?? '')
}
</script>
<style scoped>
.semantic-value { min-width: 0; overflow-wrap: anywhere; white-space: pre-wrap; line-height: 1.6; }
small { display: block; color: var(--text-secondary); font-size: 12px; }
details { margin-top: 4px; } summary { cursor: pointer; }
pre { max-height: 300px; overflow: auto; white-space: pre-wrap; overflow-wrap: anywhere; background: var(--bg-secondary); padding: 8px; }
</style>
