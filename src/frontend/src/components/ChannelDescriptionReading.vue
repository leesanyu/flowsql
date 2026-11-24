<template>
  <section data-testid="channel-reading" class="reading">
    <p v-if="observation.structure.state === 'pending'" role="status">结构待加载；可转到当前对象管理页加载后重新 DESC。</p>
    <p v-if="observation.structure.state === 'unsupported'">{{ observation.structure.reason }}</p>
    <p>说明状态：{{ observation.description.state }} · {{ observation.description.origin }}</p>
    <p v-if="observation.description.state === 'needs_review'" role="status">结构或绑定已变化，说明待核对。</p>
    <p v-if="observation.basis.dictionary_ref">精确依据：<a :href="'/semantics?ref=' + encodeURIComponent(observation.basis.dictionary_ref)">{{ observation.basis.dictionary_ref }}</a></p>
    <p v-if="observation.description.origin === 'producer_default'">生产者默认说明，未独立保存 · {{ observation.basis.contract_ref }}</p>
    <SemanticValue :value="document?.description" />
    <dl><template v-for="key in semanticKeys.entity" :key="key"><dt>{{ labels[key] }}</dt><dd><SemanticValue :value="document?.entity?.[key]" /></dd></template></dl>
    <details v-for="(field,index) in observation.structure.fields || []" :key="index" :data-reading-field="field.name">
      <summary>{{ field.name }} · {{ field.type?.name }} · {{ field.nullable === null ? '可空性未知' : field.nullable ? '可空' : '非空' }}</summary>
      <dl><template v-for="key in semanticKeys.field" :key="key"><dt>{{ labels[key] }}</dt><dd><SemanticValue :value="definition(field.name)?.[key]" /></dd></template></dl>
    </details>
    <details><summary>本次读取依据与结构</summary><pre>{{ prettyJson(observation) }}</pre></details>
  </section>
</template>
<script setup>
import { computed } from 'vue'
import SemanticValue from './SemanticValue.vue'
import { labels, semanticKeys, prettyJson } from '../utils/dataSemantics.js'
const props = defineProps({ observation: { type: Object, required: true } })
const document = computed(() => props.observation.description.document)
const definition = name => document.value?.fields?.find(f => (props.observation.context.binding?.field_map?.[f.field_id] || f.name) === name)
</script>
<style scoped>
.reading { overflow-wrap: anywhere; line-height: 1.6; } dl { display: grid; grid-template-columns: 120px minmax(0,1fr); gap: 8px; } dd { margin: 0; } summary { cursor: pointer; padding: 8px 0; } pre { white-space: pre-wrap; max-height: 360px; overflow: auto; } a { color: var(--el-color-primary); }
</style>
