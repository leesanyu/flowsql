<template>
  <div class="schema-view">
    <p v-if="schema.description">{{ schema.description }}</p>
    <p class="constraints">{{ constraints }}</p>
    <p v-if="schema.$ref">精确结构引用：<code>{{ schema.$ref }}</code>（在下方“结构资源”中查看对应定义）</p>
    <div v-if="schema.fields" class="schema-scroll"><table><thead><tr><th>输出字段</th><th>完整类型</th><th>可空性</th></tr></thead><tbody><tr v-for="field in schema.fields" :key="field.name"><td>{{ field.name }}</td><td>{{ format(field.type) }}</td><td>{{ field.nullable ? '可空' : '非空' }}</td></tr></tbody></table></div>
    <div v-if="schema.properties" class="schema-scroll"><table><thead><tr><th>参数 / 属性</th><th>要求与默认值</th><th>类型和约束</th></tr></thead><tbody>
      <tr v-for="(item, name) in schema.properties" :key="name"><td><strong>{{ name }}</strong></td><td>{{ schema.required?.includes(name) ? '必填' : '可选' }}<div v-if="Object.hasOwn(item, 'default')">默认：{{ format(item.default) }}</div></td><td><ContractSchemaView :schema="item" /></td></tr>
    </tbody></table></div>
    <details v-if="schema.items"><summary>数组元素</summary><ContractSchemaView :schema="schema.items" /></details>
    <details v-for="kind in branches" :key="kind"><summary>{{ branchLabels[kind] }}</summary><section v-for="(item, i) in schema[kind]" :key="i"><h5>分支 {{ i + 1 }}</h5><ContractSchemaView :schema="item" /></section></details>
    <details v-if="schema.if || schema.then || schema.else"><summary>条件规则</summary><div v-for="key in ['if','then','else']" :key="key"><strong v-if="schema[key]">{{ key }}</strong><ContractSchemaView v-if="schema[key]" :schema="schema[key]" /></div></details>
    <p v-if="schema.not">禁止：{{ format(schema.not) }}</p>
    <p v-if="schema.additionalProperties === false">不接受未声明属性</p>
    <p v-if="schema.required?.length && !schema.properties">必填项：{{ schema.required.join('、') }}</p>
  </div>
</template>
<script setup>
import { computed } from 'vue'
const props = defineProps({ schema: { type: [Object, Boolean], required: true } })
const format = value => JSON.stringify(value)
const branchLabels = { oneOf: '只满足一个条件分支', anyOf: '至少满足一个条件分支', allOf: '同时满足的约束' }
const branches = computed(() => Object.keys(branchLabels).filter(k => props.schema?.[k]?.length))
const constraints = computed(() => {
  if (typeof props.schema === 'boolean') return props.schema ? '允许任意值' : '不接受值'
  return ['type','const','enum','minimum','maximum','exclusiveMinimum','exclusiveMaximum','minLength','maxLength','pattern','minItems','maxItems','uniqueItems','format']
    .filter(k => Object.hasOwn(props.schema, k)).map(k => `${k}: ${format(props.schema[k])}`).join(' · ')
})
</script>
<style scoped>
.schema-view { min-width: 0; overflow-wrap: anywhere; } .schema-scroll { max-width: 100%; overflow: auto; }
table { border-collapse: collapse; min-width: 480px; width: 100%; } th, td { border: 1px solid var(--border-color); padding: 8px; text-align: left; vertical-align: top; } th { background: var(--bg-secondary); }
p { margin: 4px 0; } details { padding: 6px 0; } summary { cursor: pointer; } section { border-left: 2px solid var(--border-color); margin: 8px 0; padding-left: 10px; } code, .constraints { font-family: monospace; }
</style>
