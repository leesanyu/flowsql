<template>
  <div class="relations">
    <p v-if="error" role="alert">{{ error }}</p>
    <div v-for="item in related" :key="item.index" class="relation">
      <strong>{{ targetTitle(item.relation.dictionary_ref) }}</strong>
      <p class="pairs">{{ mappingTitle(item.relation) }}</p>
      <p v-if="targets[item.relation.dictionary_ref]?.error" role="status">目标说明暂不可读；原关系和精确版本已保留。</p>
      <button :disabled="loading || (!!draft && editing >= 0)" @click="edit(item.index)">编辑关联 {{ item.index + 1 }}</button><button :disabled="loading || (!!draft && editing >= 0)" @click="remove(item.index)">删除关联 {{ item.index + 1 }}</button>
    </div>
    <button :disabled="loading || !!draft" @click="add">新增关联</button>
    <div v-if="draft" class="relation-form">
      <p v-if="editing >= 0" class="hint">保留当前精确版本；只有重新选择目标才会替换目标版本。</p>
      <label>目标类型<select aria-label="关联目标类型" v-model="kind" @change="catalog"><option value="dataframe">DataFrame</option><option value="database_table">数据库表</option><option value="kafka_source">Kafka 结构化来源</option></select></label>
      <label>{{ kind === 'database_table' ? '目标连接' : '目标对象' }}<select aria-label="关联目标对象" v-model="selected" :disabled="loading" @change="selectObject"><option value="">请选择</option><option v-for="(item, i) in objects" :key="i" :value="String(i)">{{ item.label }}</option></select></label>
      <label v-if="kind === 'database_table' && selected !== ''">目标表<select aria-label="关联目标表" v-model="table" :disabled="loading" @change="describeTarget"><option value="">请选择</option><option v-for="name in tables" :key="name" :value="name">{{ name }}</option></select></label>
      <p v-if="loading" role="status">正在读取目标目录或说明…</p>
      <p v-if="targetDocument" class="hint">{{ targetDocument.title }} · 使用当前选定的精确版本</p>
      <div v-for="(pair, i) in draft.pairs" :key="i" class="pair">
        <select :aria-label="'关联源字段 ' + (i + 1)" v-model="pair.source"><option value="">源字段</option><option v-for="f in sourceFields" :key="f.name" :value="f.name">{{ f.name }}</option></select>
        <span>→</span>
        <select :aria-label="'关联目标字段 ' + (i + 1)" v-model="pair.target" :disabled="!targetDocument"><option value="">目标字段</option><option v-for="f in targetDocument?.fields || []" :key="f.field_id" :value="f.field_id">{{ f.title || f.name }} · {{ f.name }}</option></select>
        <button :aria-label="'移除字段配对 ' + (i + 1)" @click="draft.pairs.splice(i, 1)">移除</button>
      </div>
      <button @click="draft.pairs.push({ source: '', target: '' })">增加字段配对</button>
      <button :disabled="loading || !targetDocument" @click="apply">保留关联草稿</button><button @click="cancel">取消关联编辑</button>
    </div>
  </div>
</template>
<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { semanticsCall as call, clone, json, parseJson } from '../utils/dataSemantics.js'
const props = defineProps({ modelValue: { type: Array, required: true }, sourceFields: { type: Array, required: true }, fieldName: String, targets: { type: Object, required: true }, draftKey: String, startNew: Boolean })
const emit = defineEmits(['update:modelValue', 'target'])
const draft = ref(null), editing = ref(-1), kind = ref('dataframe'), selected = ref(''), table = ref(''), objects = ref([]), tables = ref([]), targetDocument = ref(null), loading = ref(false), error = ref('')
const related = computed(() => props.modelValue.map((relation, index) => ({ relation, index })).filter(x => Object.hasOwn(x.relation.field_map, props.fieldName)))
const targetTitle = ref => props.targets[ref]?.document?.title || '历史关联'
const mappingTitle = relation => Object.entries(relation.field_map).map(([source, id]) => `${source} → ${props.targets[relation.dictionary_ref]?.document?.fields.find(f => f.field_id === id)?.name || '目标字段'}`).join('；')
const storageKey = () => props.draftKey + '.relation.' + props.fieldName
function persist() { if (draft.value) sessionStorage.setItem(storageKey(), json({ draft: draft.value, editing: editing.value, kind: kind.value })); else sessionStorage.removeItem(storageKey()) }
watch([draft, editing, kind], persist, { deep: true })
async function request(path, payload) {
  const response = await fetch('/api/' + path, payload === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json(payload) })
  const data = parseJson(await response.text())
  if (!response.ok) throw Error(data.error?.message || data.error || '目标目录读取失败')
  return data
}
async function catalog() {
  loading.value = true; error.value = ''; objects.value = []; selected.value = ''; table.value = ''; tables.value = []; targetDocument.value = null
  try {
    if (kind.value === 'dataframe') {
      const data = await request('channels/dataframe')
      objects.value = (data.channels || []).map(x => ({ label: x.name, locator: { kind: 'dataframe', name: x.name } }))
    } else if (kind.value === 'database_table') {
      const data = await request('channels/database/query', {})
      objects.value = data.map(x => ({ label: `${x.type}.${x.name}`, locator: { kind: 'database_table', type: x.type, name: x.name } }))
    } else {
      let after = '', items = []
      do { const data = await request('channels/kafka/channels/query', { after, limit: 100 }); items.push(...data.items); after = data.next_cursor || '' } while (after)
      objects.value = items.filter(x => x.payload_kind === 'structured' && x.role !== 'sink').map(x => ({ label: x.name, locator: { kind: 'kafka_source', name: x.name } }))
    }
  } catch (e) { error.value = e.message } finally { loading.value = false }
}
async function add() { editing.value = -1; draft.value = { baseRelations: clone(props.modelValue), dictionary_ref: '', pairs: [{ source: props.fieldName, target: '' }] }; targetDocument.value = null; await catalog() }
async function edit(index) {
  const relation = props.modelValue[index]
  editing.value = index; draft.value = { baseRelations: clone(props.modelValue), dictionary_ref: relation.dictionary_ref, pairs: Object.entries(relation.field_map).map(([source, target]) => ({ source, target })) }
  await catalog(); await resolveTarget(relation.dictionary_ref)
}
async function resolveTarget(exact) {
  loading.value = true; error.value = ''; targetDocument.value = null
  try { const resolved = await call('resolve', { exact_reference: exact }); targetDocument.value = resolved.document; emit('target', exact, resolved.document) }
  catch (e) { error.value = '目标说明暂不可读；原关系与草稿已保留。' + e.message }
  finally { loading.value = false }
}
async function selectObject() {
  if (selected.value === '') return
  if (kind.value !== 'database_table') return describeTarget()
  loading.value = true; error.value = ''; table.value = ''
  try { const locator = objects.value[Number(selected.value)].locator; tables.value = (await request('channels/database/tables', { type: locator.type, name: locator.name })).tables || [] }
  catch (e) { error.value = e.message } finally { loading.value = false }
}
async function describeTarget() {
  if (selected.value === '' || (kind.value === 'database_table' && !table.value)) return
  loading.value = true; error.value = ''; targetDocument.value = null
  const locator = { ...objects.value[Number(selected.value)].locator, ...(kind.value === 'database_table' ? { table: table.value } : {}) }
  try {
    let d = await call('describe', { target: { kind: 'channel', locator } })
    if (d.structure.state !== 'known' || !['unbound', 'matched'].includes(d.checks.state)) throw Error('请先从目标对象入口加载结构并核对匹配状态。')
    if (!d.basis.dictionary_ref) {
      const initialized = await call('channels/save-description', {
        locator,
        basis: d.basis,
        editor_changes: {},
        confirmations: d.checks.required_confirmations || []
      })
      if (initialized.status !== 'applied' || !initialized.dictionary_ref) throw Error('目标默认说明创建未完成，请刷新目标对象后重试。')
      d = await call('describe', { target: { kind: 'channel', locator } })
    }
    if (d.structure.state !== 'known' || !d.basis.dictionary_ref || !d.description.document) throw Error('目标默认说明读取未完成，请刷新目标对象后重试。')
    draft.value.dictionary_ref = d.basis.dictionary_ref
    draft.value.pairs.forEach(p => { p.target = '' })
    targetDocument.value = d.description.document; emit('target', d.basis.dictionary_ref, d.description.document)
  } catch (e) { error.value = e.message } finally { loading.value = false }
}
function apply() {
  const pairs = draft.value.pairs
  if (!pairs.length || pairs.some(p => !p.source || !p.target) || new Set(pairs.map(p => p.source)).size !== pairs.length || new Set(pairs.map(p => p.target)).size !== pairs.length) { error.value = '每个关联至少一对字段，同一关联内源字段和目标字段分别不得重复。'; return }
  const fields = new Set(targetDocument.value.fields.map(f => f.field_id))
  if (pairs.some(p => !fields.has(p.target))) { error.value = '目标字段不在该精确版本中；请重新核对。'; return }
  const next = clone(draft.value.baseRelations || props.modelValue), relation = { dictionary_ref: draft.value.dictionary_ref, field_map: Object.fromEntries(pairs.map(p => [p.source, p.target])) }
  if (editing.value < 0) next.push(relation); else next[editing.value] = relation
  emit('update:modelValue', next); cancel()
}
function remove(index) { const next = clone(props.modelValue); next.splice(index, 1); emit('update:modelValue', next); if (draft.value) cancel() }
function cancel() { draft.value = null; targetDocument.value = null; error.value = ''; sessionStorage.removeItem(storageKey()) }
onMounted(async () => { try { const saved = parseJson(sessionStorage.getItem(storageKey()) || 'null'); if (saved) { draft.value = saved.draft; editing.value = saved.editing; kind.value = saved.kind; await catalog(); if (draft.value.dictionary_ref) await resolveTarget(draft.value.dictionary_ref) } else if (props.startNew && !related.value.length) await add() } catch (e) { error.value = '关联草稿读取失败：' + e.message } })
</script>
<style scoped>
.relations { max-width: 100%; margin-top: 8px; font-size: 12px; } .relation { border-bottom: 1px solid var(--border-color); padding: 6px 0; } .pairs, .hint { color: var(--text-secondary); overflow-wrap: anywhere; }
label { display: block; margin: 6px 0; } select { width: 100%; box-sizing: border-box; padding: 6px; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 4px; } select option { color: var(--text-primary); background: var(--bg-card); }
.pair { display: grid; grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr) auto; align-items: center; gap: 4px; margin: 6px 0; }
button { color: var(--text-primary); background: var(--bg-secondary); border: 1px solid var(--border-color); border-radius: 4px; padding: 6px; margin: 4px; cursor: pointer; } button:disabled { opacity: .5; cursor: default; } [role=alert] { color: var(--el-color-danger); }
</style>
