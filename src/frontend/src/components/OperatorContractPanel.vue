<template>
  <section class="operator-contract" data-testid="operator-contract">
    <header>
      <div class="identity"><h3>{{ fullName }}</h3><p v-if="usage?.identity.title && usage.identity.title !== fullName">{{ usage.identity.title }}</p></div>
      <div class="toolbar">
        <button type="button" class="primary" :disabled="busy || !!historical || !notesBasis" @click="saveNotes">保存说明</button>
        <button type="button" class="refresh" :disabled="busy" @click="refresh">刷新</button>
      </div>
    </header>
    <p v-if="error" role="alert" data-testid="contract-error">{{ error }}</p>
    <p v-if="notice" class="notice" role="status">{{ notice }}</p>
    <p v-if="historical" role="status">正在查看历史声明：{{ historical.exact_reference }}。只读。<button type="button" @click="returnCurrent">返回当前版本</button></p>
    <div v-if="usage" class="usage-view" data-testid="operator-usage">
      <p v-if="historical" v-for="item in authorPurposes" :key="item.source_pointer" class="author-description" data-testid="author-purpose">{{ item.text }}</p>
      <div v-if="!historical" data-testid="operator-description">
        <label class="purpose">用途<textarea v-model="notes.purpose" aria-label="算子用途" rows="2" maxlength="30000" /></label>
        <div v-if="notesChanged" class="conflict" role="status">
          <p>组件或说明版本已变化；草稿已保留，请核对后保存。</p>
          <details><summary>当前已保存的说明</summary><pre>{{ pretty(currentNotes()) }}</pre></details>
          <button type="button" :disabled="busy" @click="reviewNotes">核对当前组件后继续编辑</button>
        </div>
      </div>
      <section data-usage-section="inputs" class="contract-section">
        <h4>输入</h4>
        <label v-if="!historical" class="section-description">输入说明<textarea v-model="notes.input_description" aria-label="输入说明" rows="2" maxlength="30000" /></label>
        <div v-if="usage.inputs.length" class="declaration">
          <div v-for="input in usage.inputs.filter(input => historical || !input.description)" :key="input.port_id" class="input-port">
            <p v-if="input.description" class="author-description" data-testid="author-input">{{ input.description }}</p>
            <template v-else><h5><code>{{ input.port_id }}</code><span>{{ input.shape }} · {{ input.min_count === input.max_count ? input.min_count : input.min_count + '～' + input.max_count }} 个输入</span></h5>
            <div v-for="role in unlinkedRoles(input)" :key="role.role_id" class="role">
              <strong>{{ role.role_id }}</strong> · {{ role.required ? '必需' : '可选' }}<span v-if="roleText(role)"> · {{ roleText(role) }}</span>
              <p>字段选择：{{ pretty(role.selector) }}</p>
            </div>
            </template>
          </div>
          <OperatorContractDetail v-for="d in visibleInputDetails" :key="detailKey(d)" :reference="d.exact_reference" :pointer="d.resource_pointer" :label="detailLabel(d)" />
        </div>
        <OperatorDescriptionTable v-if="!historical" section="inputs" v-model="notes.inputs" />
      </section>
      <section data-usage-section="parameters" class="contract-section">
        <h4>参数</h4>
        <label v-if="!historical" class="section-description">参数说明<textarea v-model="notes.parameter_description" aria-label="参数说明" rows="2" maxlength="30000" /></label>
        <p v-if="historical && usage.parameters.description" class="author-description" data-testid="author-parameters">{{ usage.parameters.description }}</p>
        <div v-else-if="!usage.parameters.description && (usage.parameters.items.length || detailsFor('parameters').length || usage.parameters.additional_parameters === 'forbidden')" class="declaration">
          <div class="contract-table" v-if="usage.parameters.items.length"><table><thead><tr><th>参数</th><th>传值方式</th><th>要求与条件</th></tr></thead><tbody>
            <tr v-for="p in usage.parameters.items" :key="p.name">
              <td><strong>{{ p.name }}</strong><div v-for="link in p.input_roles" :key="link.port_id + link.role_id">输入 {{ link.port_id }} / {{ link.role_id }}<span v-if="roleText(findRole(link))">：{{ roleText(findRole(link)) }}</span></div></td>
              <td>{{ p.wire_encoding === 'json' ? 'JSON 文本，作为字符串提交' : '字符串' }}<p v-if="Object.hasOwn(p.schema, 'default')">默认：{{ pretty(p.schema.default) }}<small>仅提示，调用时不自动填入</small></p></td>
              <td><span class="requirement">{{ p.required === null ? '按组级条件必填' : p.required ? '必填' : '可省略' }}</span><ContractSchemaView :schema="p.schema" /></td>
            </tr>
          </tbody></table></div>
          <p v-if="usage.parameters.additional_parameters === 'forbidden'">不接受额外参数。</p>
          <OperatorContractDetail v-for="d in detailsFor('parameters')" :key="detailKey(d)" :reference="d.exact_reference" :pointer="d.resource_pointer" :label="detailLabel(d)" />
        </div>
        <OperatorDescriptionTable v-if="!historical" section="parameters" v-model="notes.parameters" />
      </section>
      <section data-usage-section="outputs" class="contract-section">
        <h4>输出</h4>
        <label v-if="!historical" class="section-description">输出说明<textarea v-model="notes.output_description" aria-label="输出说明" rows="3" maxlength="30000" /></label>
        <div v-if="usage.outputs.length" class="declaration">
          <div v-for="output in usage.outputs.filter(o => historical && o.description)" :key="readingKey + output.port_id" :data-contract-output="output.port_id" class="author-output"><strong>{{ output.port_id }}</strong><p class="author-description">{{ output.description }}</p></div>
          <details v-for="(output,index) in usage.outputs.filter(o => !o.description)" :key="readingKey + output.port_id" :open="index === 0" :data-contract-output="output.port_id">
            <summary><strong>{{ output.port_id }}</strong> · {{ output.shape }}<span v-if="output.summary"> · {{ output.summary }}</span></summary>
            <div class="contract-table" v-if="output.fields.length"><table><thead><tr><th>字段</th><th>类型</th><th>含义与条件</th></tr></thead><tbody><tr v-for="f in output.fields" :key="f.name"><td>{{ f.name }}</td><td>{{ fieldType(f.declaration) }}<small v-if="fieldNullable(f)">{{ fieldNullable(f) }}</small></td><td><ContractSchemaView v-if="f.format === 'json_schema'" :schema="f.declaration" /><template v-else>{{ f.declaration.description || '' }}<div v-for="(value,key) in fieldExtras(f.declaration)" :key="key">{{ key }}：{{ pretty(value) }}</div></template></td></tr></tbody></table></div>
            <p v-if="output.fields_state === 'dynamic'">包含动态字段，展开结构与规则查看。</p>
            <details v-if="output.availability"><summary>产生条件</summary><pre>{{ pretty(output.availability) }}</pre></details>
            <details v-if="output.row_cases"><summary>不同行类别与 NULL / 有效性约束</summary><div v-for="row in output.row_cases" :key="row.case_id"><h5>{{ row.case_id }}</h5><p>条件：{{ pretty(row.when) }}</p><p v-for="(rules,field) in row.field_rules" :key="field">{{ field }}：{{ pretty(rules) }}</p></div></details>
            <details v-if="output.context_semantics"><summary>必要解释上下文</summary><pre>{{ pretty(output.context_semantics) }}</pre></details>
            <OperatorContractDetail v-for="d in outputDetails(output)" :key="detailKey(d)" :reference="d.exact_reference" :pointer="d.resource_pointer" :label="detailLabel(d)" />
          </details>
        </div>
        <OperatorDescriptionTable v-if="!historical" section="outputs" v-model="notes.outputs" />
      </section>
      <details class="conditions" data-testid="usage-conditions">
        <summary>使用限制</summary>
        <ul v-if="historical && authorLimitations.length"><li v-for="(item,i) in authorLimitations" :key="i">{{ item.text }}</li></ul>
        <template v-if="!historical">
          <label>使用限制<textarea v-model="notes.usage_notes" aria-label="算子使用注意" rows="3" maxlength="30000" /></label>
        </template>
        <details v-if="!historical && observed.required_context_refs.length"><summary>必要语义上下文</summary><pre>{{ pretty(observed.required_context_refs) }}</pre></details>
      </details>
      <details class="conditions" data-testid="usage-examples">
        <summary>调用示例</summary>
        <template v-if="!historical">
          <label>调用示例（空行分隔）<textarea v-model="examplesText" aria-label="算子调用示例" rows="3" /></label>
        </template>
        <button v-if="!historical && examplesText" type="button" @click="copy(examplesText)">复制示例</button>
        <div v-if="historical" v-for="(item,i) in usage.examples" :key="i"><pre>{{ item.text }}</pre><button type="button" @click="copy(item.text)">复制示例</button></div>
      </details>
      <footer class="save-actions">
        <button type="button" :disabled="busy || !!historical" @click="cancelNotes">恢复已保存内容</button>
      </footer>
    </div>
    <details :key="readingKey" class="technical" data-testid="contract-technical" @toggle="readFull">
      <summary>技术详情（已保存内容）</summary>
      <button type="button" @click="openHistory">历史版本</button>
      <section v-if="showHistory" class="history" data-testid="contract-history">
        <h5>历史版本（只读）</h5>
        <button v-for="item in versions" :key="item.exact_reference" type="button" @click="readHistory(item.exact_reference)">{{ item.exact_reference }}</button>
        <button v-if="historyCursor" type="button" :disabled="historyBusy" @click="loadHistory">更多历史版本</button>
        <p v-if="!versions.length && !historyBusy">没有历史记录。</p>
        <button type="button" @click="showHistory = false">收起历史列表</button>
      </section>
      <template v-if="technicalDocument">
        <pre data-testid="contract-full">{{ pretty(technicalDocument) }}</pre>
        <button type="button" @click="copy(pretty(technicalDocument))">复制 JSON</button>
      </template>
    </details>
  </section>
</template>
<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import ContractSchemaView from './ContractSchemaView.vue'
import { operatorDescriptionNotes } from '../utils/operatorDescriptionNotes.js'
import OperatorContractDetail from './OperatorContractDetail.vue'
import OperatorDescriptionTable from './OperatorDescriptionTable.vue'
const props = defineProps({ fullName: { type: String, required: true }, declarationMode: { type: String, default: '' } })
const name = props.fullName, pretty = value => JSON.stringify(value, null, 2)
const notesKey = 'flowsql.operator-description.inline-editors.' + name
const observed = ref(null), historical = ref(null), fullRead = ref(null)
const busy = ref(false), error = ref(''), notice = ref('')
const notes = reactive({ purpose: '', usage_notes: '', input_description: '', parameter_description: '', output_description: '', inputs: [], parameters: [], outputs: [] }), examplesText = ref(''), notesBasis = ref(null)
const versions = ref([]), showHistory = ref(false), historyCursor = ref(''), historyBusy = ref(false)
let disposed = false, readId = 0, historyId = 0, noteRevision = 0
onBeforeUnmount(() => { disposed = true; readId++; historyId++ })
const notesChanged = computed(() => notesBasis.value && observed.value &&
  (notesBasis.value.component_revision !== observed.value.basis.component_revision || notesBasis.value.description_ref !== observed.value.basis.description_ref))
const readingKey = computed(() => historical.value?.exact_reference || pretty(observed.value?.basis))
const technicalDocument = computed(() => historical.value || fullRead.value)
const usage = computed(() => historical.value ? historicalUsage(historical.value) : observed.value?.usage)
const authorPurposes = computed(() => usage.value?.identity.purposes.filter(item => item.origin === 'author') || [])
const visibleInputDetails = computed(() => detailsFor('inputs').filter(d => usage.value.inputs.some(input => !input.description && (d.resource_pointer === input.source_pointer || d.resource_pointer.startsWith(input.source_pointer + '/')))))
const authorLimitations = computed(() => usage.value?.limitations.filter(item => item.origin === 'author') || [])
const nullable = value => typeof value !== 'boolean' ? '' : value ? '可空' : '非空'
const fieldNullable = f => nullable(f.format === 'physical' ? f.declaration.nullable : Array.isArray(f.declaration.type) ? f.declaration.type.includes('null') : typeof f.declaration.type === 'string' ? f.declaration.type === 'null' : null)
const fieldType = d => d.type ? (typeof d.type === 'string' ? d.type : pretty(d.type)) : ''
const fieldExtras = d => Object.fromEntries(Object.entries(d).filter(([k]) => !['name','type','nullable','description'].includes(k)))
const roleText = role => [role.types.map(type => type.system + '.' + type.name + (Object.keys(type.parameters).length ? ' ' + pretty(type.parameters) : '')).join(' / '), nullable(role.nullable), Object.keys(role.semantic_requirements).length ? pretty(role.semantic_requirements) : ''].filter(Boolean).join('；')
const findRole = link => usage.value.inputs.find(input => input.port_id === link.port_id).roles.find(role => role.role_id === link.role_id)
const unlinkedRoles = input => input.roles.filter(role => !usage.value.parameters.items.some(p => p.input_roles.some(link => link.port_id === input.port_id && link.role_id === role.role_id)))
const detailsFor = section => {
  if (!historical.value) return observed.value?.details.filter(d => d.section === section) || []
  const doc = historical.value.document
  if (doc.schema_id !== 'flowsql.operator-data-contract.v1') return []
  const paths = section === 'parameters' ? ['/configuration'] : section === 'inputs' ? doc.inputs.map((_, i) => '/inputs/' + i) : doc.outputs.flatMap((output,i) => ['/outputs/' + i, ...(output.structure_ref?.startsWith('#/') ? [output.structure_ref.slice(1)] : [])])
  return paths.map(resource_pointer => ({ section, resource_pointer, exact_reference: historical.value.exact_reference, reason: '历史完整规则' }))
}
const detailKey = d => d.exact_reference + d.resource_pointer
const detailLabel = d => d.reason + ' · ' + d.resource_pointer.split('/').at(-1)
function outputDetails(output) {
  return detailsFor('outputs').filter(d => d.resource_pointer === output.source_pointer || d.resource_pointer.startsWith(output.source_pointer + '/') || d.resource_pointer === output.structure_pointer || output.fields.some(f => f.source_pointer.startsWith(d.resource_pointer + '/')))
}
function currentNotes() {
  return operatorDescriptionNotes(observed.value)
}
function resetNotes() { const { examples, ...saved } = currentNotes(); Object.assign(notes, saved); examplesText.value = examples.join('\n\n'); notesBasis.value = observed.value.basis }
function rememberNotes() { if (notesBasis.value) sessionStorage.setItem(notesKey, pretty({ notes: { ...notes }, examplesText: examplesText.value, basis: notesBasis.value })) }
watch([notes, examplesText], () => { noteRevision++; rememberNotes() }, { deep: true, flush: 'sync' })
async function post(path, payload) {
  const response = await fetch('/api/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
  const body = await response.json()
  if (!response.ok) throw Error(typeof body.error === 'object' ? `${body.error.code}: ${body.error.message}` : body.error || `HTTP ${response.status}`)
  return body.data || body
}
async function refresh() {
  const id = ++readId; busy.value = true; error.value = ''
  try {
    const data = await post('semantics/v1/describe', { target: { kind: 'operator', name } })
    if (disposed || id !== readId) return
    observed.value = data; fullRead.value = null
    if (!notesBasis.value) resetNotes()
  } catch (e) { if (!disposed && id === readId) error.value = e.message }
  finally { if (!disposed && id === readId) busy.value = false }
}
async function saveNotes() {
  const revision = noteRevision, basis = notesBasis.value
  const saved = { ...JSON.parse(pretty(notes)), examples: examplesText.value.split(/\n\s*\n/).filter(v => v.trim()) }
  busy.value = true; error.value = ''
  try {
    const response = await post('semantics/v1/operators/save-description', { name, expected_component_revision: basis.component_revision, expected_description_ref: basis.description_ref, notes: saved })
    if (disposed) return
    notesBasis.value = { ...basis, description_ref: response.description_ref }; rememberNotes()
    await refresh()
    if (disposed) return
    if (revision === noteRevision) resetNotes()
    notice.value = '说明已保存。'
  } catch (e) { if (!disposed) { await refresh(); error.value = e.message; rememberNotes() } }
  finally { if (!disposed) busy.value = false }
}
function cancelNotes() { resetNotes(); rememberNotes(); notice.value = '已恢复最近保存的说明。' }
function reviewNotes() { notesBasis.value = observed.value.basis; rememberNotes(); notice.value = '已显式改用当前依据，原稿保留；请核对后保存。' }
async function openHistory() { showHistory.value = true; if (!versions.value.length) await resetHistory() }
async function resetHistory() { historyId++; versions.value = []; historyCursor.value = ''; await loadHistory() }
async function loadHistory() {
  const id = ++historyId; historyBusy.value = true
  try { const page = await post('semantics/v1/history', { kind: 'operator_contract', name, limit: 10, cursor: historyCursor.value }); if (!disposed && id === historyId) { versions.value.push(...page.items); historyCursor.value = page.next_cursor } }
  catch (e) { if (!disposed && id === historyId) error.value = e.message }
  finally { if (!disposed && id === historyId) historyBusy.value = false }
}
async function readHistory(reference) {
  const id = ++readId; busy.value = true; error.value = ''
  try { const data = await post('semantics/v1/resolve', { exact_reference: reference }); if (!disposed && id === readId) { historical.value = data; fullRead.value = null } }
  catch (e) { if (!disposed && id === readId) error.value = e.message }
  finally { if (!disposed && id === readId) busy.value = false }
}
async function returnCurrent() { historical.value = null; await refresh() }
async function readFull(event) {
  if (!event.target.open || historical.value || fullRead.value || !observed.value) return
  const id = readId, basis = pretty(observed.value.basis)
  try { const data = await post('semantics/v1/describe', { target: { kind: 'operator', name }, view: 'full' }); if (!disposed && id === readId) { if (pretty(data.basis) !== basis) throw Error('版本已变化，请刷新后重读完整契约。'); fullRead.value = data } }
  catch (e) { if (!disposed && id === readId) error.value = e.message }
}
async function copy(text) { try { await navigator.clipboard.writeText(text); notice.value = '已复制。' } catch (e) { notice.value = '复制未完成：' + e.message } }
function historicalUsage(record) {
  const doc = record.document
  return { identity: { name, title: doc.title, author_version: doc.operator.version, purposes: doc.description ? [{ text: doc.description, origin: 'author', source_pointer: '/description' }] : [] }, inputs: doc.inputs,
    parameters: { description: doc.configuration.description, items: Object.entries(doc.configuration.invocation_schema.properties || {}).map(([key, schema]) => ({ name: key, schema, required: doc.configuration.invocation_schema.required?.includes(key) ? true : null, wire_encoding: doc.configuration.decoded_parameters?.some(p => p.key === key) ? 'json' : 'string', input_roles: [] })) },
    outputs: doc.outputs.map((output,i) => ({ ...output, source_pointer: '/outputs/' + i, structure_pointer: output.structure_ref?.slice(1), fields: [], summary: '历史输出；完整规则可按需展开' })), limitations: doc.limitations.map(text => ({ text, origin: 'author' })), examples: (doc.examples || []).map(text => ({ text, origin: 'author' })) }
}
onMounted(() => {
  const savedNotes = JSON.parse(sessionStorage.getItem(notesKey) || 'null')
  if (savedNotes) { Object.assign(notes, savedNotes.notes); examplesText.value = savedNotes.examplesText; notesBasis.value = savedNotes.basis }
  refresh()
})
</script>
<style scoped>
.operator-contract { margin: 16px 0; padding: 20px 24px; border: 1px solid var(--border-color); border-radius: 10px; min-width: 0; color: var(--text-primary); background: var(--bg-primary); font-size: 14px; line-height: 1.6; }
.toolbar { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; flex-shrink: 0; } .toolbar button { margin: 0; font-size: 13px; }
header { display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; }
h3 { margin: 0; font-size: 17px; line-height: 1.5; } .identity p { margin: 4px 0 0; color: var(--text-secondary); font-size: 13px; }
h4 { margin: 0 0 14px; font-size: 16px; font-weight: 600; } h5 { margin: 0 0 8px; font-size: 14px; } .input-port + .input-port { margin-top: 12px; } .input-port h5 { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; } .input-port h5 span { color: var(--text-secondary); font-weight: 400; }
.usage-view { margin-top: 20px; }
.section-description { margin-bottom: 14px; }
.author-description { white-space: pre-line; margin: 0 0 12px; } .author-output + .author-output { margin-top: 16px; } .author-output > p { margin: 4px 0 0; }
.contract-section { padding: 20px 0; border-top: 1px solid var(--border-color); } .declaration { margin-bottom: 12px; } .purpose { margin-bottom: 20px; } .role { padding: 6px 0; } .role p { margin: 4px 0; }
label { display: block; font-size: 13px; color: var(--text-secondary); } textarea { display: block; box-sizing: border-box; width: 100%; min-height: 40px; margin: 6px 0 0; padding: 9px 12px; resize: vertical; font: inherit; color: var(--text-primary); background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 6px; }
textarea:focus { outline: 2px solid var(--el-color-primary-light-7); border-color: var(--el-color-primary); } label + label { margin-top: 14px; }
pre { white-space: pre-wrap; overflow-wrap: anywhere; max-height: 340px; overflow: auto; background: var(--bg-secondary); padding: 12px; border-radius: 6px; }
.contract-table { max-width: 100%; overflow: auto; } table { width: 100%; min-width: 500px; border-collapse: collapse; font-size: 13px; } th, td { border-bottom: 1px solid var(--border-color); text-align: left; vertical-align: top; padding: 10px 12px; overflow-wrap: anywhere; } th { color: var(--text-secondary); background: var(--bg-secondary); font-weight: 500; } td p { margin: 6px 0; } small { display: block; font-size: 12px; color: var(--text-secondary); }
section, details, .identity { min-width: 0; overflow-wrap: anywhere; } details { padding: 8px 0; } summary { cursor: pointer; } .conditions { border-top: 1px solid var(--border-color); padding: 12px 0; } .conditions > label:first-of-type { margin-top: 12px; } .technical { border-top: 1px solid var(--border-color); padding: 14px 0 0; margin-top: 16px; font-size: 13px; color: var(--text-secondary); } .history button { display: block; text-align: left; max-width: 100%; overflow-wrap: anywhere; }
button { cursor: pointer; margin: 4px 8px 4px 0; padding: 7px 14px; color: var(--text-primary); background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 5px; font: inherit; line-height: 1.4; } button:disabled { cursor: default; opacity: .5; } .refresh { margin: 0; font-size: 13px; } .primary { background: var(--el-color-primary); border-color: var(--el-color-primary); color: white; } .save-actions { display: flex; align-items: center; flex-wrap: wrap; gap: 4px; padding-top: 12px; } [role=alert] { color: var(--el-color-danger); } .notice { color: var(--el-color-success); font-size: 13px; } .conflict { padding: 12px; margin-bottom: 16px; background: var(--bg-secondary); border-radius: 6px; }
@media (max-width: 700px) { .operator-contract { padding: 16px; } }
</style>
