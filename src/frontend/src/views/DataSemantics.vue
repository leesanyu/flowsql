<template>
  <ChannelDictionaryEditor v-if="datasetLocator" :key="json(datasetLocator)" :locator="datasetLocator" />
  <section v-else class="semantics-page">
    <header><h1>标准与字典</h1><p>维护业务定义和人工结构声明；未知信息可留空，保存不代表业务数据已通过验证。</p></header>
    <p v-if="error" role="alert" data-testid="error">{{ error }}</p>
    <p v-if="notice" role="status" data-testid="notice">{{ notice }}</p>
    <div class="toolbar">
      <label>类型 <select v-model="filterKind" aria-label="筛选类型"><option value="">全部</option><option v-for="(label, key) in kinds" :key="key" :value="key">{{ label }}</option></select></label>
      <input v-model="query" aria-label="搜索关键词" placeholder="名称、字段或说明" @keyup.enter="run(() => list())" />
      <button :disabled="busy" @click="run(() => list())">搜索</button>
      <button v-for="(label, key) in kinds" :key="key" :disabled="busy" @click="create(key)">新建{{ label }}</button>
    </div>
    <div class="workspace">
      <aside>
        <ul class="object-list"><li v-for="item in items" :key="item.exact_reference">
          <button :disabled="busy" @click="open(item.exact_reference)">{{ item.title || item.name }}</button>
          <small>{{ item.exact_reference }} · 缺口 {{ item.semantic_gap_count }}</small>
        </li></ul>
        <button v-if="nextCursor" :disabled="busy" @click="run(() => list(true))">更多定义</button>
      </aside>
      <article v-if="document">
        <section v-if="!kinds[kind]" data-testid="operator-semantics-readonly"><p>算子契约和使用依据为只读精确记录。静态声明通过算子注册或上传维护。</p><pre>{{ json(document) }}</pre></section>
        <div class="toolbar"><strong>{{ kinds[kind] || kind }} · {{ exact || '尚未保存' }}</strong><span>{{ editing ? '编辑中' : '只读版本' }}</span>
          <button v-if="!editing && kinds[kind]" :disabled="busy" @click="run(editCurrent)">编辑当前版本</button>
          <button v-if="!editing && kinds[kind]" :disabled="busy" @click="run(restore)">复制为新修订</button>
          <button v-if="!editing && kinds[kind]" :disabled="busy" @click="copyNew">另存新名称</button>
        </div>
        <label>名称 <input v-model="name" aria-label="名称" :disabled="!editing || revision > 0" placeholder="app.requests" /></label>
        <label>{{ kind === 'binding' ? '对象标签' : '标题' }} <input v-if="kind !== 'binding'" v-model="document.title" aria-label="标题" :disabled="!editing" /><input v-else v-model="document.object_label" aria-label="对象标签" :disabled="!editing" /></label>
        <template v-if="kind === 'standard'">
          <h2>标准定义</h2><SemanticValueEditor v-for="key in semanticKeys.standard" :key="key" v-model="document[key]" :field-key="key" :label="labels[key]" :disabled="!editing" @validity="valid(key, $event)" />
        </template>
        <template v-if="kind === 'dictionary'">
          <h2>实体语义</h2><SemanticValueEditor v-model="document.description" field-key="description" label="说明" :disabled="!editing" @validity="valid('description', $event)" />
          <details><summary>行粒度、身份、时间、关联与局限</summary>
            <SemanticValueEditor v-for="key in semanticKeys.entity" :key="key" v-model="document.entity[key]" :field-key="key" :label="labels[key]" :disabled="!editing" @validity="valid(key, $event)" />
          </details>
          <h2>字段结构声明</h2>
          <label v-if="editing"><input v-model="structureEditing" type="checkbox" aria-label="启用结构编辑" />显式编辑结构（身份、名称、类型、可空性）</label>
          <p>以下结构由人工或导入声明，未读取实际数据源。</p>
          <table><thead><tr><th>field_id</th><th>来源字段名</th><th>标题</th><th>类型系统 / 类型</th><th>可空</th><th>操作</th></tr></thead>
            <tbody><tr v-for="(f, i) in document.fields" :key="i" :data-field="i">
              <td><input v-model="f.field_id" :aria-label="'字段身份 ' + i" :disabled="!editing || !structureEditing" /></td>
              <td><input v-model="f.name" :aria-label="'字段名 ' + i" :disabled="!editing || !structureEditing" /></td>
              <td><input v-model="f.title" :aria-label="'字段标题 ' + i" :disabled="!editing" /></td>
              <td><select v-model="f.type.system" :aria-label="'类型系统 ' + i" :disabled="!editing || !structureEditing"><option>arrow</option><option>database</option><option>opaque</option></select>
                <input v-model="f.type.name" :aria-label="'类型名称 ' + i" :disabled="!editing || !structureEditing" />
                <input :value="json(f.type.parameters)" :aria-label="'类型参数 ' + i" :disabled="!editing || !structureEditing" @change="parameters(i, $event.target.value)" /></td>
              <td><input v-model="f.nullable" type="checkbox" :aria-label="'可空 ' + i" :disabled="!editing || !structureEditing" /></td>
              <td><button v-if="editing && structureEditing" @click="document.fields.splice(i, 1); invalid.clear()">移除</button></td>
            </tr></tbody>
          </table>
          <button v-if="editing && structureEditing" @click="document.fields.push(blankField())">添加字段</button>
          <pre v-if="diff" data-testid="structure-diff">{{ diff }}</pre>
          <h2>字段语义</h2><details v-for="(f, i) in document.fields" :key="i" :data-semantics="i"><summary>{{ f.title || f.name || '新字段' }} · {{ f.field_id }}</summary>
            <SemanticValueEditor v-for="key in semanticKeys.field" :key="key" v-model="f[key]" :field-key="key" :label="labels[key]" :references="standards" :disabled="!editing" @validity="valid(i + '/' + key, $event)" />
          </details>
        </template>
        <template v-if="kind === 'binding'">
          <h2>人工登记对象与映射</h2>
          <label>Provider 身份 <input v-model="document.object_key.provider_id" aria-label="Provider 身份" :disabled="!editing || revision > 0" /></label>
          <label>数据集身份 <input v-model="document.object_key.object_id" aria-label="数据集身份" :disabled="!editing || revision > 0" /></label>
          <label>字典精确版本 <select v-model="document.dictionary_ref" aria-label="字典精确版本" :disabled="!editing"><option value="">请选择</option><option v-for="ref in dictionaryChoices" :key="ref">{{ ref }}</option></select></label>
          <label>字段映射（field_id → 来源字段名）<textarea v-model="mappingText" aria-label="字段映射" :disabled="!editing" rows="4" /></label>
          <label>声明结构（null 或 name/type/nullable 列表）<textarea v-model="schemaText" aria-label="声明结构" :disabled="!editing" rows="4" /></label>
          <p v-if="snapshot">声明结构检查：{{ snapshot.structure_check }}；实际数据验证：未执行</p>
        </template>
        <template v-if="editing">
          <label>变更说明 <input v-model="changeNote" aria-label="变更说明" /></label>
          <div v-if="kind === 'dictionary'"><label>保存后应用到对象 <select v-model="applyReference" aria-label="应用绑定"><option value="">仅保存定义</option><option v-for="b in bindings" :key="b.exact_reference" :value="b.exact_reference">{{ b.title }} · {{ b.exact_reference }}</option></select></label><p>定义保存与对象应用分别提交；应用失败会保留已保存版本。</p></div>
          <button :disabled="busy || invalid.size > 0" @click="run(() => save(false))">保存新版本</button>
          <button v-if="kind === 'dictionary' && applyReference" :disabled="busy || invalid.size > 0" @click="run(() => save(true))">保存并应用</button>
          <button v-if="kind === 'dictionary' && exact && applyReference && !dirty" :disabled="busy" @click="run(applyOnly)">应用到对象</button>
        </template>
        <section v-if="snapshot"><h2>缺失信息（{{ snapshot.semantic_gaps.length }}）</h2><details><summary>查看未知属性</summary><ul><li v-for="g in snapshot.semantic_gaps" :key="g.path">{{ g.path }}：未知</li></ul></details>
          <details><summary>精确引用上下文</summary><pre>{{ json(snapshot.reference_context) }}</pre></details>
        </section>
        <section v-if="exact"><h2>引用与历史</h2><p>修改共享定义前，请查看下列使用范围；另存新名称可保留共享定义。</p>
          <label>引用范围 <select v-model="referenceScope" aria-label="引用范围" @change="run(() => references())"><option value="current">当前</option><option value="history">历史</option></select></label>
          <ul data-testid="references"><li v-for="r in refs" :key="r.source_reference + r.field_path">{{ r.source_reference }} · {{ r.field_path }} · {{ r.is_current ? '当前' : '历史' }}</li></ul>
          <button v-if="refsCursor" @click="run(() => references(true))">更多引用</button>
          <ul data-testid="history"><li v-for="h in history" :key="h.exact_reference"><button @click="open(h.exact_reference)">{{ h.exact_reference }}</button> {{ h.change_note }} · {{ new Date(h.created_at).toLocaleString() }}</li></ul>
          <button v-if="historyCursor" @click="run(() => histories(true))">更多历史</button>
        </section>
        <section><h2>JSON / YAML 交换</h2>
          <label>格式 <select v-model="format" aria-label="交换格式"><option value="json">JSON</option><option value="yaml">YAML</option></select></label>
          <input type="file" accept=".json,.yaml,.yml" aria-label="选择导入文件" @change="readFile" />
          <textarea v-model="exchangeText" aria-label="导入导出内容" rows="7" placeholder="粘贴完整文档或导出信封" />
          <button :disabled="busy || !editing" @click="run(previewImport)">校验导入预览</button>
          <button v-if="importPreview" :disabled="busy" @click="adoptImport">采用预览</button>
          <button v-if="exact" :disabled="busy" @click="run(exportDocument)">导出当前版本</button>
          <button v-if="exportedContent" @click="download">下载导出文件</button>
          <pre v-if="importPreview" data-testid="import-preview">{{ json(importPreview) }}</pre>
          <p>预览不会保存。来源版本仅作追溯；保存时由系统分配版本。</p>
        </section>
      </article>
      <p v-else>选择一项定义，或新建标准、字典及人工绑定。</p>
    </div>
  </section>
</template>
<script setup>
import { ref, computed, reactive, onMounted, onUnmounted } from 'vue'
import { useRoute, useRouter, onBeforeRouteLeave } from 'vue-router'
import SemanticValueEditor from '../components/SemanticValueEditor.vue'
import ChannelDictionaryEditor from '../components/ChannelDictionaryEditor.vue'
import { blankDocument, blankField, semanticKeys, labels, json, parseJson, clone, structuralDiff, semanticsCall as call, saveAndApply } from '../utils/dataSemantics.js'
const route = useRoute(), router = useRouter()
const datasetLocator = computed(() => { try { return route.query.dataset ? parseJson(String(route.query.dataset)) : null } catch { return null } })
const kinds = { standard: '标准', dictionary: '字典', binding: '绑定' }
const filterKind = ref(''), query = ref(''), items = ref([]), nextCursor = ref(''), busy = ref(false), error = ref(''), notice = ref('')
const kind = ref('standard'), name = ref(''), document = ref(null), original = ref(null), revision = ref(0), exact = ref(''), editing = ref(false), structureEditing = ref(false)
const changeNote = ref(''), baseRevision = ref(0), snapshot = ref(null), mappingText = ref('{}'), schemaText = ref('null'), baseline = ref('')
const standards = ref([]), dictionaries = ref([]), bindings = ref([]), applyReference = ref(''), refs = ref([]), refsCursor = ref(''), referenceScope = ref('current'), history = ref([]), historyCursor = ref('')
const format = ref('json'), exchangeText = ref(''), importPreview = ref(null), exportedContent = ref(''), invalid = reactive(new Set())
const dictionaryChoices = computed(() => [...new Set([...dictionaries.value, document.value?.dictionary_ref].filter(Boolean))])
const state = () => json({ name: name.value, document: document.value, mapping: mappingText.value, schema: schemaText.value, note: changeNote.value })
const dirty = computed(() => editing.value && (state() !== baseline.value || invalid.size > 0))
const diff = computed(() => kind.value === 'dictionary' && original.value ? structuralDiff(original.value, document.value) : '')
const valid = (key, ok) => ok ? invalid.delete(key) : invalid.add(key)
const discard = () => !dirty.value || window.confirm('存在未保存内容，确认离开并丢弃？')
onBeforeRouteLeave(() => discard())
function beforeUnload(e) { if (dirty.value) { e.preventDefault(); e.returnValue = '' } }
onMounted(() => { if (datasetLocator.value) return; window.addEventListener('beforeunload', beforeUnload); run(async () => { await list(); await choices(); if (route.query.ref) await load(String(route.query.ref)) }) })
onUnmounted(() => window.removeEventListener('beforeunload', beforeUnload))
async function run(fn) { if (busy.value) return; busy.value = true; error.value = ''; try { await fn() } catch (e) { error.value = e.message } finally { busy.value = false } }
async function all(op, payload) { const result = []; let cursor = ''; do { const p = await call(op, { ...payload, cursor, limit: 100 }); result.push(...p.items); cursor = p.next_cursor } while (cursor); return result }
async function choices() {
  const listed = await all('list', {})
  const standardRefs = [], dictionaryRefs = []
  for (const item of listed.filter(x => x.kind === 'standard' || x.kind === 'dictionary')) {
    const versions = await all('history', { kind: item.kind, name: item.name })
    ;(item.kind === 'standard' ? standardRefs : dictionaryRefs).push(...versions.map(v => v.exact_reference))
  }
  standards.value = standardRefs; dictionaries.value = dictionaryRefs; bindings.value = listed.filter(x => x.kind === 'binding')
}
async function list(more = false) {
  const result = await call('list', { ...(filterKind.value ? { kind: filterKind.value } : {}), q: query.value, cursor: more ? nextCursor.value : '', limit: 50 })
  items.value = more ? [...items.value, ...result.items] : result.items; nextCursor.value = result.next_cursor
}
function identity(ref) { const dot = ref.indexOf('.'), at = ref.lastIndexOf('@'); return { kind: ref.slice(0, dot), name: ref.slice(dot + 1, at), revision: Number(ref.slice(at + 1)) } }
function resetForm(d) { document.value = clone(d); original.value = clone(d); mappingText.value = json(d.field_map || {}); schemaText.value = json(d.declared_schema ?? null); changeNote.value = ''; invalid.clear(); importPreview.value = null; exchangeText.value = ''; exportedContent.value = ''; baseline.value = state() }
async function load(ref) {
  const result = await call('resolve', { exact_reference: ref }), id = identity(ref)
  kind.value = id.kind; name.value = id.name; revision.value = id.revision; exact.value = ref; snapshot.value = result; editing.value = false; structureEditing.value = false; baseRevision.value = 0; applyReference.value = ''
  resetForm(result.document); await router.replace({ query: { ref } }); await histories(); await references()
}
function open(ref) { if (discard()) run(() => load(ref)) }
function create(k) { if (!discard()) return; kind.value = k; name.value = ''; revision.value = 0; exact.value = ''; baseRevision.value = 0; snapshot.value = null; editing.value = true; structureEditing.value = true; refs.value = []; history.value = []; applyReference.value = ''; notice.value = ''; error.value = ''; resetForm(blankDocument(k)); router.replace({ query: {} }); run(choices) }
async function editCurrent() {
  const current = await call('history', { kind: kind.value, name: name.value, limit: 1 })
  if (current.items[0].exact_reference !== exact.value) throw Error('这是历史版本。请打开当前版本编辑，或选择“复制为新修订”。')
  editing.value = true; baseline.value = state(); await choices()
}
async function restore() { const current = await call('history', { kind: kind.value, name: name.value, limit: 1 }); baseRevision.value = revision.value; revision.value = current.items[0].revision; editing.value = true; baseline.value = ''; await choices() }
function copyNew() { name.value = ''; revision.value = 0; baseRevision.value = 0; exact.value = ''; editing.value = true; structureEditing.value = true; baseline.value = ''; refs.value = []; history.value = []; snapshot.value = null; router.replace({ query: {} }) }
function parameters(i, raw) { try { document.value.fields[i].type.parameters = parseJson(raw); invalid.delete('parameters' + i) } catch { invalid.add('parameters' + i); error.value = '类型参数 JSON 无效，输入尚未保存' } }
async function selectedBinding() { const r = await call('resolve', { exact_reference: applyReference.value }); return { ...identity(applyReference.value), document: r.document } }
async function save(apply) {
  if (kind.value === 'binding') { document.value.field_map = parseJson(mappingText.value); document.value.declared_schema = parseJson(schemaText.value) }
  if (diff.value && !structureEditing.value) throw Error('结构已变化，请显式启用结构编辑并核对差异')
  const binding = apply ? await selectedBinding() : null
  const result = await saveAndApply(call, { kind: kind.value, name: name.value, expected_current_revision: revision.value, ...(baseRevision.value ? { base_revision: baseRevision.value } : {}), change_note: changeNote.value, document: document.value }, binding)
  notice.value = `定义已保存：${result.saved.exact_reference}` + (result.applyError ? `；绑定未更新：${result.applyError.message}` : result.applied ? `；绑定已更新：${result.applied.exact_reference}` : '；未应用到对象')
  revision.value = result.saved.revision; exact.value = result.saved.exact_reference; baseRevision.value = 0; original.value = clone(document.value); baseline.value = state()
  // Save is already committed. A refresh failure must not turn it into a failed-save claim.
  try { snapshot.value = await call('resolve', { exact_reference: exact.value }); document.value = clone(snapshot.value.document); original.value = clone(document.value); baseline.value = state(); await router.replace({ query: { ref: exact.value } }); await list(); await histories(); await references(); await choices() }
  catch (e) { error.value = `定义已保存，刷新未完成：${e.message}` }
}
async function applyOnly() {
  const b = await selectedBinding()
  try { const r = await call('save', { kind: 'binding', name: b.name, expected_current_revision: b.revision, document: { ...b.document, dictionary_ref: exact.value } }); notice.value = `绑定已更新：${r.exact_reference}`; await references(); await choices() }
  catch (e) { notice.value = `定义已保存：${exact.value}；绑定未更新：${e.message}` }
}
async function histories(more = false) { const r = await call('history', { kind: kind.value, name: name.value, cursor: more ? historyCursor.value : '', limit: 20 }); history.value = more ? [...history.value, ...r.items] : r.items; historyCursor.value = r.next_cursor }
async function references(more = false) { const r = await call('references', { exact_reference: exact.value, scope: referenceScope.value, cursor: more ? refsCursor.value : '', limit: 20 }); refs.value = more ? [...refs.value, ...r.items] : r.items; refsCursor.value = r.next_cursor }
async function previewImport() { importPreview.value = await call('import', { kind: kind.value, name: name.value, format: format.value, content: exchangeText.value }) }
function adoptImport() { if (kind.value === 'dictionary' && structuralDiff(document.value, importPreview.value.document) && !structureEditing.value) { error.value = '导入改变结构，请先启用结构编辑并核对差异'; return } document.value = clone(importPreview.value.document); mappingText.value = json(document.value.field_map || {}); schemaText.value = json(document.value.declared_schema ?? null); snapshot.value = { ...importPreview.value, reference_context: [] }; importPreview.value = null }
async function readFile(e) { const f = e.target.files[0]; if (f) { if (f.size > 8 * 1024 * 1024) { error.value = '文件超过 8 MiB'; return } exchangeText.value = await f.text(); format.value = /\.ya?ml$/i.test(f.name) ? 'yaml' : 'json' } }
async function exportDocument() { const r = await call('export', { exact_reference: exact.value, format: format.value }); exchangeText.value = r.content; exportedContent.value = r.content }
function download() { const url = URL.createObjectURL(new Blob([exportedContent.value], { type: 'text/plain;charset=utf-8' })); const link = window.document.createElement('a'); link.href = url; link.download = `${kind.value}.${name.value}.${format.value}`; link.click(); URL.revokeObjectURL(url) }
</script>
<style scoped>
.semantics-page { max-width: 1500px; margin: auto; } h1 { margin: 0; } h2 { font-size: 18px; margin-top: 24px; }
header p, small { color: var(--text-secondary); } .toolbar { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; margin: 16px 0; }
.workspace { display: grid; grid-template-columns: 260px minmax(0, 1fr); gap: 24px; } aside { border-right: 1px solid var(--border-color, #ddd); padding-right: 16px; }
.object-list { list-style: none; padding: 0; } .object-list li { margin-bottom: 18px; } small { display: block; overflow-wrap: anywhere; }
label { display: flex; align-items: center; gap: 10px; margin: 10px 0; } input, select, textarea, button { font: inherit; border: 1px solid var(--border-color, #aaa); border-radius: 5px; padding: 7px; color: var(--text-primary); background: var(--bg-card, #fff); }
textarea { width: 100%; box-sizing: border-box; } input:not([type=checkbox]):not([type=file]) { min-width: 0; } button { cursor: pointer; } button:disabled { cursor: default; opacity: .6; }
table { width: 100%; border-collapse: collapse; } td, th { padding: 5px; text-align: left; border-bottom: 1px solid var(--border-color, #ddd); } td input:not([type=checkbox]) { width: 100%; box-sizing: border-box; } td select { max-width: 100%; }
details { margin: 12px 0; padding: 8px; border: 1px solid var(--border-color, #ddd); border-radius: 6px; } summary { cursor: pointer; } pre { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 350px; overflow: auto; background: var(--bg-page); padding: 12px; }
[role=alert] { color: #b42318; white-space: pre-wrap; } [role=status] { background: #e8f3ec; color: #18552f; padding: 14px; }
@media (max-width: 900px) { .workspace { grid-template-columns: 1fr; } aside { border-right: none; } }
</style>
