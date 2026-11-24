<template>
  <button type="button" class="result-semantics-trigger" data-testid="result-semantics" @click="open()">本次结果说明 · JSON</button>
</template>
<script setup>
import { watch, onBeforeUnmount } from 'vue'
import { semanticsCall } from '../utils/dataSemantics.js'
import { resultReading } from '../utils/semanticReading.js'
import { showJsonReading } from '../utils/semanticJsonDialog.js'
const props = defineProps({ context: { type: Object, required: true } })
let reader = null
const close = () => reader?.close()
watch(() => props.context, close)
onBeforeUnmount(close)
function open(index = null) {
  const context = props.context
  const sources = context.sources.filter(source => source.basis.dictionary_ref || source.basis.contract_ref)
  reader = showJsonReading({
    title: index === null ? '本次结果说明' : context.fields.find(field => field.output_index === index).output_name + ' · 第' + (index + 1) + '列说明',
    value: resultReading(context, index === null ? null : [index]),
    links: context.sources.flatMap((source, i) => [{ label: '当前对象管理页' + (context.sources.length > 1 ? ' ' + (i + 1) : ''),
      href: source.target.kind === 'operator' ? '/operators?name=' + encodeURIComponent(source.target.name) : '/channels?dataset=' + encodeURIComponent(JSON.stringify(source.target.locator)) },
      ...(source.basis.dictionary_ref ? [{ label: '本次精确版本' + (context.sources.length > 1 ? ' ' + (i + 1) : ''), href: '/semantics?ref=' + encodeURIComponent(source.basis.dictionary_ref) }] : [])]),
    resolve: context.error && sources.length ? { label: '按本次精确引用重试读取', load: () => Promise.all(sources.map(source => semanticsCall('resolve', { exact_reference: source.basis.dictionary_ref || source.basis.contract_ref }))) } : null
  })
}
defineExpose({ open, close })
</script>
<style scoped>
.result-semantics-trigger { margin-top: 12px; padding: 5px 12px; color: var(--el-color-primary); background: var(--bg-secondary); border: 1px solid var(--border-color); border-radius: 6px; cursor: pointer; font: inherit; font-size: 13px; }
</style>
