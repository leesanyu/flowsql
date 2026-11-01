<template>
  <details @toggle="read" :data-resource-pointer="pointer">
    <summary>{{ label }}</summary>
    <p v-if="error" role="alert">{{ error }}</p>
    <template v-if="loaded">
      <ContractSchemaView v-if="isSchema" :schema="schema" />
      <pre v-else>{{ pretty(resource) }}</pre>
      <OperatorContractDetail v-for="path in references" :key="path" :reference="reference" :pointer="path"
        :label="'关联结构：' + path.split('/').at(-1)" :ancestors="[...ancestors, pointer]" />
      <details v-if="isSchema"><summary>原始规则 JSON</summary><pre>{{ pretty(resource) }}</pre></details>
    </template>
  </details>
</template>
<script setup>
import { computed, onBeforeUnmount, ref } from 'vue'
import ContractSchemaView from './ContractSchemaView.vue'
const props = defineProps({ reference: String, pointer: String, label: String, ancestors: { type: Array, default: () => [] } })
const resource = ref(null), loaded = ref(false), error = ref('')
let pending = false, disposed = false
onBeforeUnmount(() => { disposed = true })
const pretty = value => JSON.stringify(value, null, 2)
const schema = computed(() => ['oneOf', 'allOf', 'anyOf'].includes(props.pointer.split('/').at(-1))
  ? { [props.pointer.split('/').at(-1)]: resource.value } : resource.value)
const isSchema = computed(() => typeof schema.value === 'boolean' || (schema.value && !Array.isArray(schema.value) &&
  ['type','properties','fields','oneOf','anyOf','allOf','$ref'].some(key => Object.hasOwn(schema.value, key))))
const references = computed(() => {
  const paths = new Set()
  function visit(value) {
    if (!value || typeof value !== 'object') return
    for (const [key, item] of Object.entries(value)) {
      if (key === '$ref' && typeof item === 'string' && item.startsWith('#/') && ![...props.ancestors, props.pointer].includes(item.slice(1))) paths.add(item.slice(1))
      else visit(item)
    }
  }
  visit(resource.value)
  return [...paths]
})
async function read(event) {
  if (!event.target.open || loaded.value || pending) return
  pending = true; error.value = ''
  try {
    const response = await fetch('/api/semantics/v1/resolve', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ exact_reference: props.reference, view: 'resource', resource_pointer: props.pointer }) })
    const body = await response.json()
    if (!response.ok) throw Error(body.error?.message || `HTTP ${response.status}`)
    if (!disposed) { resource.value = body.data.resource; loaded.value = true }
  } catch (e) { if (!disposed) error.value = e.message }
  finally { pending = false }
}
</script>
<style scoped>
details { margin: 8px 0; padding: 8px; border-left: 2px solid var(--border-color); min-width: 0; }
summary { cursor: pointer; overflow-wrap: anywhere; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 340px; overflow: auto; }
[role=alert] { color: var(--el-color-danger); }
</style>
