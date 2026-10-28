<template>
  <div class="semantic-value">
    <label>{{ label }}
      <select :aria-label="label + '状态'" :value="modelValue.state" :disabled="disabled" @change="state($event.target.value)">
        <option value="unknown">未知</option><option value="declared">已声明</option><option value="not_applicable">不适用</option>
      </select>
    </label>
    <template v-if="modelValue.state === 'declared'">
      <select v-if="options" :aria-label="label" :value="modelValue.value" :disabled="disabled" @change="value($event.target.value)">
        <option value="">请选择</option><option v-for="o in options" :key="o" :value="o">{{ o }}</option>
      </select>
      <select v-else-if="fieldKey === 'standard_refs'" :aria-label="label" multiple :disabled="disabled" @change="value([...$event.target.selectedOptions].map(o => o.value))">
        <option v-for="ref in allReferences" :key="ref" :value="ref" :selected="modelValue.value?.includes(ref)">{{ ref }}</option>
      </select>
      <textarea v-else-if="complex" :aria-label="label" :value="text" :disabled="disabled" @input="edit($event.target.value)" rows="3" />
      <textarea v-else :aria-label="label" :value="modelValue.value" :disabled="disabled" @input="value($event.target.value)" rows="2" />
    </template>
    <label>来源 <select :aria-label="label + '来源'" :value="modelValue.origin" :disabled="disabled" @change="emit('update:modelValue', { ...modelValue, origin: $event.target.value })">
      <option v-for="o in ['manual', 'imported', 'producer', 'derived']" :key="o">{{ o }}</option>
    </select></label>
    <small v-if="complex && modelValue.state === 'declared'">填写 JSON；字段引用使用 field_id，关系使用精确版本。</small>
    <span v-if="error" role="alert">{{ error }}</span>
  </div>
</template>
<script setup>
import { computed, ref, watch } from 'vue'
import { json, parseJson, choices, complexDefaults, clone } from '../utils/dataSemantics.js'
const props = defineProps({ modelValue: Object, label: String, fieldKey: String, disabled: Boolean, references: { type: Array, default: () => [] } })
const emit = defineEmits(['update:modelValue', 'validity'])
const options = computed(() => choices[props.fieldKey])
const complex = computed(() => Object.hasOwn(complexDefaults, props.fieldKey))
const allReferences = computed(() => [...new Set([...props.references, ...(props.modelValue.value || [])])])
const text = ref(''), error = ref('')
watch(() => props.modelValue, v => { text.value = json(v.value); error.value = ''; emit('validity', true) }, { immediate: true })
function value(v) { emit('update:modelValue', { ...props.modelValue, value: v }) }
function state(s) {
  emit('update:modelValue', { ...props.modelValue, state: s, value: s === 'declared' ? clone(complexDefaults[props.fieldKey] ?? '') : null })
}
function edit(raw) {
  text.value = raw
  try { value(parseJson(raw)); error.value = ''; emit('validity', true) } catch { error.value = 'JSON 格式无效，输入尚未保存'; emit('validity', false) }
}
</script>
<style scoped>
.semantic-value { display: grid; gap: 6px; padding: 10px 0; border-bottom: 1px solid var(--border-color, #ddd); }
label { display: flex; gap: 12px; align-items: center; } small { color: var(--text-secondary); } [role=alert] { color: #c33; }
</style>
