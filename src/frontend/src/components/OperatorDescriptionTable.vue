<template>
  <div class="description-rows" :data-testid="'description-' + section">
    <div class="table-scroll">
      <table :class="section">
        <colgroup><col v-for="field in fields" :key="field.key" :class="field.key" /><col class="actions" /></colgroup>
        <thead><tr><th v-for="field in fields" :key="field.key" scope="col">{{ field.label }}</th><th class="actions" scope="col">操作</th></tr></thead>
        <tbody>
          <tr v-for="(row,index) in modelValue" :key="index">
            <td v-for="field in fields" :key="field.key">
              <select v-if="field.kind === 'boolean'" :aria-label="label(index,field)" :value="row[field.key] === null ? '' : String(row[field.key])" @change="update(index,field.key,$event.target.value === '' ? null : $event.target.value === 'true')">
                <option value=""></option><option value="true">是</option><option value="false">否</option>
              </select>
              <select v-else-if="field.key === 'requirement'" :aria-label="label(index,field)" :value="row.requirement" @change="update(index,field.key,$event.target.value)">
                <option value=""></option><option value="required_non_null">必填非空</option><option value="required_nullable">必填可空</option><option value="optional">可选</option>
              </select>
              <select v-else-if="section === 'parameters' && field.key === 'type'" :aria-label="label(index,field)" :value="row.type" @change="update(index,field.key,$event.target.value)">
                <option value=""></option><option value="string">字符串</option><option value="integer">整数</option><option value="number">数值</option><option value="boolean">布尔</option><option value="json">JSON</option>
              </select>
              <textarea v-else-if="field.key === 'description'" :aria-label="label(index,field)" :value="row.description" rows="1" maxlength="30000" @input="update(index,field.key,$event.target.value)" />
              <input v-else :aria-label="label(index,field)" :value="row[field.key]" :maxlength="field.key === 'default' ? 30000 : 256" @input="update(index,field.key,$event.target.value)" />
            </td>
            <td class="actions"><button type="button" :aria-label="'删除' + rowTitle(index)" @click="remove(index)">删除</button></td>
          </tr>
        </tbody>
      </table>
    </div>
    <button type="button" class="add" :disabled="modelValue.length >= 100" @click="add">添加{{ title }}{{ section === 'parameters' ? '' : '字段' }}</button>
  </div>
</template>
<script setup>
import { computed } from 'vue'
const props = defineProps({ section: { type: String, required: true }, modelValue: { type: Array, required: true } })
const emit = defineEmits(['update:modelValue'])
const title = computed(() => ({ inputs: '输入', parameters: '参数', outputs: '输出' })[props.section])
const field = (key,label,kind='input') => ({ key,label,kind })
const fields = computed(() => props.section === 'parameters' ? [
  field('name','参数名'),field('type','类型'),field('required','必填','boolean'),field('default','默认值'),field('description','说明')
] : props.section === 'inputs' ? [
  field('name','字段名'),field('type','类型'),field('requirement','填写要求'),field('description','说明')
] : [field('name','字段名'),field('type','类型'),field('nullable','允许空值','boolean'),field('description','说明')])
const rowTitle = index => title.value + ' ' + (index + 1)
const label = (index,field) => rowTitle(index) + ' ' + field.label
function update(index,key,value) { emit('update:modelValue',props.modelValue.map((row,i) => i === index ? { ...row,[key]:value } : row)) }
function remove(index) { emit('update:modelValue',props.modelValue.filter((_,i) => i !== index)) }
function add() { emit('update:modelValue',[...props.modelValue,Object.fromEntries(fields.value.map(field => [field.key,field.kind === 'boolean' ? null : '']))]) }
</script>
<style scoped>
.description-rows { min-width: 0; color-scheme: light; } .dark .description-rows { color-scheme: dark; }
.table-scroll { overflow-x: auto; max-width: 100%; }
table { width: 100%; min-width: 590px; table-layout: fixed; border-collapse: collapse; font-size: 13px; } table.parameters { min-width: 700px; }
col.name { width: 21%; } col.type { width: 17%; } col.requirement,col.nullable { width: 21%; } col.required { width: 12%; } col.default { width: 19%; } .parameters col.name { width: 18%; } .parameters col.type { width: 15%; }
th,td { padding: 9px 7px; border-bottom: 1px solid var(--border-color); vertical-align: top; text-align: left; } th { color: var(--text-secondary); font-weight: 500; white-space: nowrap; }
input,select,textarea { box-sizing: border-box; width: 100%; min-width: 0; padding: 8px; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 4px; font: inherit; font-size: 13px; line-height: 20px; }
option { color: var(--text-primary); background: var(--bg-card); }
textarea { resize: vertical; min-height: 38px; } input:focus,select:focus,textarea:focus { outline: 2px solid var(--el-color-primary-light-7); border-color: var(--el-color-primary); }
.actions { width: 62px; } button { cursor: pointer; white-space: nowrap; padding: 7px 8px; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 4px; font: inherit; font-size: 12px; line-height: 20px; } .add { margin-top: 10px; } button:disabled { opacity: .5; cursor: default; }
</style>
