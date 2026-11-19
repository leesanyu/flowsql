<template>
  <details @toggle="read" :data-resource-pointer="pointer">
    <summary>{{ label }}</summary>
    <p v-if="error" role="alert">{{ error }}</p>
    <template v-if="loaded">
      <template v-if="groups.length">
        <section v-for="group in groups" :key="group.name" class="entity-group"><h5>{{ group.name }}</h5>
          <article v-for="entity in group.entities" :key="entity.entity"><strong>{{ entity.entity }}</strong><p>{{ entity.description || '' }}</p>
            <OperatorContractDetail v-for="path in entityPaths(entity)" :key="path" :reference="reference" :pointer="path"
              :label="'实体规则：' + path.split('/').at(-1)" :ancestors="[...ancestors, pointer]" />
          </article>
        </section>
      </template>
      <ContractSchemaView v-else-if="isSchema" :schema="schema" />
      <pre v-else>{{ pretty(resource) }}</pre>
      <OperatorContractDetail v-if="!groups.length" v-for="path in references" :key="path" :reference="reference" :pointer="path"
        :label="'关联结构：' + path.split('/').at(-1)" :ancestors="[...ancestors, pointer]" />
      <details v-if="isSchema || groups.length"><summary>局部完整规则 JSON</summary><pre>{{ pretty(resource) }}</pre></details>
    </template>
  </details>
</template>
<script setup>
import { computed, onBeforeUnmount, ref } from 'vue'
import ContractSchemaView from './ContractSchemaView.vue'
import { json, parseJson, prettyJson } from '../utils/dataSemantics.js'
const props = defineProps({ reference: String, pointer: String, label: String, ancestors: { type: Array, default: () => [] } })
const resource = ref(null), loaded = ref(false), error = ref('')
let pending = false, disposed = false
onBeforeUnmount(() => { disposed = true })
const pretty = prettyJson
const schema = computed(() => ['oneOf', 'allOf', 'anyOf'].includes(props.pointer.split('/').at(-1))
  ? { [props.pointer.split('/').at(-1)]: resource.value } : resource.value)
const isSchema = computed(() => typeof schema.value === 'boolean' || (schema.value && !Array.isArray(schema.value) &&
  ['type','properties','fields','oneOf','anyOf','allOf','$ref'].some(key => Object.hasOwn(schema.value, key))))
const groups = computed(() => {
  const grouped = new Map()
  for (const entity of resource.value?.entities || []) {
    const name = entity.module || '实体'
    if (!grouped.has(name)) grouped.set(name, [])
    grouped.get(name).push(entity)
  }
  return [...grouped].map(([name, entities]) => ({ name, entities }))
})
const entityPaths = entity => [...new Set(['structure_ref','dictionary_pointer'].map(key => entity[key])
  .filter(value => typeof value === 'string' && value.startsWith('#/')).map(value => value.slice(1)))]
const references = computed(() => {
  const paths = new Set()
  function visit(value) {
    if (!value || typeof value !== 'object') return
    for (const [key, item] of Object.entries(value)) {
      if (['$ref','structure_ref','dictionary_pointer','schema_ref'].includes(key) && typeof item === 'string' && item.startsWith('#/') && ![...props.ancestors, props.pointer].includes(item.slice(1))) paths.add(item.slice(1))
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
      body: json({ exact_reference: props.reference, view: 'resource', resource_pointer: props.pointer }) })
    const body = parseJson(await response.text())
    if (!response.ok) throw Error(body.error?.message || `HTTP ${response.status}`)
    if (!disposed) { resource.value = body.data.resource; loaded.value = true }
  } catch (e) { if (!disposed) error.value = e.message }
  finally { pending = false }
}
</script>
<style scoped>
details { margin: 8px 0; padding: 8px; border-left: 2px solid var(--border-color); min-width: 0; }
.entity-group { border-top: 1px solid var(--border-color); margin: 12px 0; } article { padding: 8px; overflow-wrap: anywhere; }
summary { cursor: pointer; overflow-wrap: anywhere; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 340px; overflow: auto; }
[role=alert] { color: var(--el-color-danger); }
</style>
