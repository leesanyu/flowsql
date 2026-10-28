<template>
  <section class="channel-dictionary" data-testid="channel-dictionary">
    <fieldset :disabled="busy">
    <h1>通道数据字典</h1><p>{{ locator.kind }} · {{ locator.name }} {{ locator.schema || '' }} {{ locator.table || '' }}</p>
    <p>结构来自本次来源观察；人工说明与标准引用需由你补充。匹配只代表结构与映射一致，业务数据验证未执行。</p>
    <p v-if="error" role="alert" data-testid="channel-error">{{ error }}</p>
    <p v-if="notice" role="status" data-testid="channel-notice">{{ notice }}</p>
    <p data-testid="channel-state">核对状态：{{ check?.state || 'unavailable' }} · {{ check?.binding_ref || '无绑定' }}</p>
    <button :disabled="busy" @click="run(refresh)">刷新核对</button>
    <button :disabled="busy" @click="generate">从来源生成候选</button>
    <button v-if="preview?.source_snapshot.schema_state === 'pending' && locator.kind === 'dataframe'" :disabled="busy" @click="run(loadFrame)">显式加载 DataFrame 结构</button>
    <p v-if="preview?.source_snapshot.schema_state !== 'known'">来源结构：{{ preview?.source_snapshot.schema_state || '不可用' }}；取得完整结构后才能生成和应用。</p>
    <details v-if="preview"><summary>本次结构依据与缺口</summary><pre>{{ json(preview.source_snapshot) }}</pre></details>
    <section v-if="replacements.length" data-testid="channel-replacements"><h2>同名对象已替换</h2>
      <p>当前身份：{{ json(preview.source_snapshot.object_key) }}；旧绑定不会自动迁移。</p>
      <div v-for="item in replacements" :key="item.binding"><p>旧身份：{{ json(item.key) }} · {{ item.binding }} · {{ item.dictionary }}</p>
        <button :disabled="busy" @click="selected = item.dictionary; run(selectDictionary)">复用旧字典 {{ item.dictionary }}</button></div>
    </section>
    <div class="reuse">
      <label>已有字典精确版本 <select v-model="selected" aria-label="已有字典精确版本"><option value="">请选择</option><option v-for="r in dictionaries" :key="r">{{ r }}</option></select></label>
      <label>显式映射（field_id → 来源字段）<textarea v-model="mapping" aria-label="来源字段映射" rows="3" /></label>
      <button :disabled="busy || !selected" @click="run(selectDictionary)">选择已有版本</button>
      <p>先选择版本会显示其字段身份；检查或修改映射后再次选择。复用保留该精确字典，额外源列允许存在。</p>
    </div>
    <template v-if="document">
      <label>字典名称 <input v-model="name" aria-label="通道字典名称" :disabled="dictionaryRevision > 0" /></label>
      <label>绑定名称 <input v-model="bindingName" aria-label="通道绑定名称" :disabled="bindingRevision > 0" /></label>
      <label>标题 <input v-model="document.title" aria-label="通道字典标题" /></label>
      <p>字典版本：{{ savedRef || '尚未保存' }}；基准绑定：{{ preview.binding_ref || '无' }}</p>
      <button :disabled="busy" @click="copyDictionary">另存新字典</button>
      <details v-if="references.length"><summary>共享与历史引用范围（{{ references.length }}）</summary><pre>{{ json(references) }}</pre></details>
      <h2>实体说明</h2><SemanticValueEditor v-model="document.description" field-key="description" label="说明" @validity="valid('description', $event)" />
      <details><summary>行含义、身份、时间与关联</summary><SemanticValueEditor v-for="key in semanticKeys.entity" :key="key" v-model="document.entity[key]" :field-key="key" :label="labels[key]" @validity="valid(key, $event)" /></details>
      <h2>字段与人工语义</h2>
      <details v-for="(f, i) in document.fields" :key="f.field_id" :data-channel-field="i"><summary>{{ f.name }} · {{ f.field_id }} · {{ f.type.name }} · {{ f.nullable ? '可空' : '非空' }}</summary>
        <label>字段标题 <input v-model="f.title" :aria-label="'通道字段标题 ' + i" /></label>
        <SemanticValueEditor v-for="key in semanticKeys.field" :key="key" v-model="f[key]" :field-key="key" :label="labels[key]" :references="standards" @validity="valid(f.field_id + key, $event)" />
      </details>
      <details open><summary>结构差异与生产者声明</summary><pre data-testid="channel-differences">{{ json(preview.differences) }}</pre>
        <label>确认重命名映射（field_id → 新来源字段名）<textarea v-model="renames" aria-label="确认重命名映射" /></label>
        <label>移除字段身份（JSON 数组）<textarea v-model="removals" aria-label="移除字段身份" /></label>
        <button :disabled="busy" @click="adapt">按来源适配候选</button>
        <p>适配保留未变字段的说明；删除与重命名按显式选择处理，内部字段引用需要先修订。来源与人工声明冲突时请在字段中检查双方内容。</p>
        <label v-for="item in preview.required_confirmations" :key="item"><input v-model="confirmations" type="checkbox" :value="item" :aria-label="'确认 ' + item" />确认 {{ item }}</label>
      </details>
      <p>语义缺口 {{ preview.semantic_gaps.length }} 项；未知信息保持未知。</p>
      <button :disabled="busy || invalid.size > 0" @click="run(() => save(false))">保存字典</button>
      <button :disabled="busy || invalid.size > 0" @click="run(() => save(true))">保存字典并应用</button>
      <button :disabled="busy || !savedRef || savedContent !== json(document) || invalid.size > 0" @click="run(apply)">应用已保存版本</button>
    </template>
    </fieldset>
  </section>
</template>
<script setup>
import { ref, reactive, computed, onMounted, onUnmounted } from 'vue'
import { onBeforeRouteLeave } from 'vue-router'
import SemanticValueEditor from './SemanticValueEditor.vue'
import { semanticsCall as call, json, parseJson, clone, semanticKeys, labels } from '../utils/dataSemantics.js'
const props = defineProps({ locator: { type: Object, required: true } })
const busy = ref(false), error = ref(''), notice = ref(''), preview = ref(null), check = ref(null), document = ref(null)
const replacements = ref([])
const dictionaries = ref([]), standards = ref([]), selected = ref(''), mapping = ref('{}'), renames = ref('{}'), removals = ref('[]')
const name = ref(''), bindingName = ref(''), dictionaryRevision = ref(0), bindingRevision = ref(0), confirmations = ref([])
const savedRef = ref(''), savedContent = ref(''), references = ref([]), invalid = reactive(new Set())
const dirty = computed(() => !!document.value && json(document.value) !== savedContent.value)
const valid = (key, ok) => ok ? invalid.delete(key) : invalid.add(key)
const discard = () => !dirty.value || window.confirm('存在未保存的字典内容，确认丢弃？')
onBeforeRouteLeave(() => discard())
function beforeUnload(e) { if (dirty.value) { e.preventDefault(); e.returnValue = '' } }
onMounted(() => { window.addEventListener('beforeunload', beforeUnload); run(async () => { await choices(); await refresh(); await makePreview() }) })
onUnmounted(() => window.removeEventListener('beforeunload', beforeUnload))
async function run(fn) { if (busy.value) return; busy.value = true; error.value = ''; try { await fn() } catch (e) { error.value = e.message } finally { busy.value = false } }
async function all(op, payload) { const result = []; let cursor = ''; do { const p = await call(op, { ...payload, cursor, limit: 100 }); result.push(...p.items); cursor = p.next_cursor } while (cursor); return result }
async function choices() {
  const items = await all('list', {})
  dictionaries.value = []; standards.value = []
  for (const item of items.filter(x => x.kind !== 'binding')) {
    const versions = await all('history', { kind: item.kind, name: item.name })
    ;(item.kind === 'dictionary' ? dictionaries.value : standards.value).push(...versions.map(x => x.exact_reference))
  }
}
function identity(r) { if (!r) return { name: '', revision: 0 }; const at = r.lastIndexOf('@'); return { name: r.slice(r.indexOf('.') + 1, at), revision: Number(r.slice(at + 1)) } }
async function refresh() { check.value = null; check.value = await call('channels/check', { locator: props.locator }) }
async function inspectReferences(r) { references.value = r ? await all('references', { exact_reference: r, scope: 'history' }) : [] }
async function findReplacements(source) {
  replacements.value = []
  for (const item of await all('list', { kind: 'binding' })) {
    const old = await call('resolve', { exact_reference: item.exact_reference })
    const observed = old.document.source_snapshot
    if (!observed || ['provider_id', 'object_id'].every(k => observed.object_key[k] === source.object_key[k])) continue
    if (['kind', 'name', 'type', 'table'].every(k => observed.locator[k] === source.locator[k]) &&
        ['database', 'schema'].every(k => !props.locator[k] || observed.locator[k] === source.locator[k]))
      replacements.value.push({ key: observed.object_key, binding: item.exact_reference, dictionary: old.document.dictionary_ref })
  }
}
async function makePreview(extra = {}, exactDocument = null, exactMap = null) {
  const next = await call('channels/preview', { locator: props.locator, ...extra })
  const base = identity(next.base_dictionary_ref), bound = identity(next.binding_ref)
  let resolved = null
  if (next.base_dictionary_ref) resolved = await call('resolve', { exact_reference: next.base_dictionary_ref })
  preview.value = next; document.value = exactDocument || next.dictionary; mapping.value = json(exactMap || next.field_map)
  name.value = base.name; dictionaryRevision.value = base.revision; bindingName.value = bound.name; bindingRevision.value = bound.revision
  savedRef.value = next.base_dictionary_ref || ''; savedContent.value = resolved ? json(resolved.document) : ''
  confirmations.value = []; invalid.clear(); await inspectReferences(next.base_dictionary_ref); await findReplacements(next.source_snapshot)
}
function generate() { if (discard()) run(async () => { await choices(); await makePreview() }) }
async function selectDictionary() {
  if (!discard()) return
  const resolved = await call('resolve', { exact_reference: selected.value })
  let map = parseJson(mapping.value)
  const ids = resolved.document.fields.map(f => f.field_id)
  if (!Object.keys(map).length || Object.keys(map).some(id => !ids.includes(id))) map = Object.fromEntries(resolved.document.fields.map(f => [f.field_id, f.name]))
  await makePreview({ base_dictionary_ref: selected.value, field_map: map }, clone(resolved.document), map)
}
function adapt() {
  if (!discard()) return
  run(() => makePreview({ ...(preview.value.base_dictionary_ref ? { base_dictionary_ref: preview.value.base_dictionary_ref } : {}), field_map: parseJson(mapping.value), renames: parseJson(renames.value), remove_fields: parseJson(removals.value) }))
}
function copyDictionary() { name.value = ''; dictionaryRevision.value = 0; savedRef.value = ''; savedContent.value = ''; references.value = [] }
async function loadFrame() {
  const response = await fetch('/api/channels/dataframe/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json({ name: props.locator.name }) })
  if (!response.ok) throw Error('加载 DataFrame 结构失败')
  await refresh(); await makePreview()
}
async function apply() {
  if (!savedRef.value || savedContent.value !== json(document.value)) throw Error('请先保存当前字典内容')
  const source = preview.value.source_snapshot
  try {
    const applied = await call('channels/apply', { locator: props.locator, name: bindingName.value, dictionary_ref: savedRef.value, expected_current_revision: bindingRevision.value, field_map: parseJson(mapping.value), source_basis: Object.fromEntries(['object_key', 'source_revision', 'schema_fingerprint'].map(k => [k, source[k]])), confirmations: confirmations.value })
    preview.value.binding_ref = applied.exact_reference; bindingRevision.value = identity(applied.exact_reference).revision
    check.value = applied.check; notice.value = '绑定已应用：' + applied.exact_reference
  } catch (e) { check.value = null; throw e }
}
async function save(andApply) {
  notice.value = ''
  const saved = await call('save', { kind: 'dictionary', name: name.value, expected_current_revision: dictionaryRevision.value, document: document.value })
  savedRef.value = saved.exact_reference; savedContent.value = json(document.value)
  // The public save envelope carries its allocated revision in exact_reference.
  dictionaryRevision.value = identity(saved.exact_reference).revision
  notice.value = '字典已保存：' + saved.exact_reference
  if (andApply) {
    try { await apply(); notice.value = '字典已保存：' + saved.exact_reference + '；' + notice.value }
    catch (e) { notice.value = '字典已保存：' + saved.exact_reference + '；绑定未更新：' + e.message; return }
  }
  await choices()
}
</script>
<style scoped>
fieldset { border: 0; padding: 0; min-width: 0; }
.channel-dictionary { padding: 24px; max-width: 1200px; } label { display: flex; align-items: center; gap: 12px; margin: 10px 0; }
button { margin: 8px 12px 8px 0; padding: 6px 12px; } textarea { min-width: 400px; } pre { white-space: pre-wrap; overflow-wrap: anywhere; }
.reuse { border-block: 1px solid #bbb; margin-block: 16px; padding: 10px; } details { padding: 8px; } [role=alert] { color: #b22; } [role=status] { color: #176735; }
</style>
