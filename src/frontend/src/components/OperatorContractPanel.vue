<template>
  <section class="operator-contract" data-testid="operator-contract">
    <h3>输入输出契约</h3>
    <p v-if="error" role="alert" data-testid="contract-error">{{ error }}</p>
    <p>声明状态：<strong>{{ component.contract_state || '加载中' }}</strong>；作者版本：{{ component.operator_version || 'unknown' }}</p>
    <p class="identity">实际组件：{{ component.component_revision || 'unknown' }}</p>
    <p v-if="component.contract_state === 'missing'">该历史算子未声明契约，仍可按原入口使用；兼容性保持未确定。</p>
    <p v-if="component.contract_state === 'unavailable'">已声明的契约暂不可用，执行会报告错误。</p>
    <label v-if="versions.length">契约精确版本
      <select v-model="selectedReference" aria-label="契约精确版本" @change="readDefinition">
        <option v-for="version in versions" :key="version" :value="version">{{ version }}</option>
      </select>
    </label>
    <p v-if="selectedReference && selectedReference !== component.contract_ref">正在查看历史声明。预检会重新取得当前组件及其关联版本。</p>
    <section class="notes" data-testid="operator-description">
      <h4>补充说明</h4><p>当前组件的本地文字说明；保存不会改变执行契约、算法或默认参数。</p>
      <p v-if="notesNotice" role="status">{{ notesNotice }}</p>
      <label>用途<textarea v-model="notes.purpose" aria-label="算子用途" rows="2" /></label>
      <label>使用注意<textarea v-model="notes.usage_notes" aria-label="算子使用注意" rows="3" /></label>
      <label>调用示例（每段一个示例）<textarea v-model="examplesText" aria-label="算子调用示例" rows="4" /></label>
      <button type="button" :disabled="busy || !notesBasis" @click="saveNotes">保存补充说明</button>
      <details v-if="notesChanged"><summary>当前组件已保存的补充说明</summary><pre>{{ pretty(observedDescription.description.notes) }}</pre></details>
      <button type="button" v-if="notesChanged" :disabled="busy" @click="reviewNotes">核对当前组件后继续编辑</button>
    </section>
    <p v-if="declarationMode === 'python'">静态声明随源码发布。激活中的算子请先从列表去激活，再进入源码编辑。</p>
    <button type="button" v-if="declarationMode === 'python'" :disabled="busy || !!component.active" :title="component.active ? '请先去激活' : '使用原源码编辑和组件版本检查'" @click="emit('maintain-declaration')">编辑源码与静态声明</button>
    <template v-if="declarationMode === 'cpp'"><p>声明随所属插件发布；上传新插件后按原流程去激活并删除旧插件、激活新插件，再核对当前算子的声明。</p><button type="button" :disabled="busy" @click="emit('maintain-declaration')">更新所属插件</button></template>
    <template v-if="definition">
      <h4>{{ definition.title }}</h4>
      <details open><summary>参数约束、默认值与影响</summary>
        <ContractSchemaView :schema="definition.configuration.invocation_schema" />
        <p v-if="definition.configuration.normalized_config_schema">有效配置：{{ pretty(definition.configuration.normalized_config_schema) }}</p>
        <p v-if="definition.configuration.decoded_parameters">参数解码：{{ pretty(definition.configuration.decoded_parameters) }}</p>
        <p>影响范围：{{ pretty(definition.configuration.affects) }}</p>
      </details>
      <details open><summary>输入角色与字段选择规则</summary>
        <section v-for="input in definition.inputs" :key="input.port_id" class="contract-group">
          <h5>{{ input.port_id }} · {{ input.shape }} · 输入数量 {{ input.min_count }}～{{ input.max_count }}</h5>
          <p v-if="input.repeat_from_config_pointer">按配置 {{ input.repeat_from_config_pointer }} 展开；实体键 {{ input.item_key_pointer }}</p>
          <div class="contract-table"><table><thead><tr><th>角色</th><th>必需</th><th>类型 / 可空性</th><th>字段选择规则</th><th>语义要求</th></tr></thead><tbody>
            <tr v-for="role in input.roles" :key="role.role_id"><td>{{ role.role_id }}</td><td>{{ role.required ? '是' : '否' }}</td><td>{{ pretty(role.types) }} / {{ role.nullable === null ? '未知' : role.nullable ? '可空' : '非空' }}</td><td>{{ pretty(role.selector) }}</td><td>{{ pretty(role.semantic_requirements) }}</td></tr>
          </tbody></table></div>
          <details v-if="input.role_templates"><summary>按配置展开的角色与条件分支</summary><pre>{{ pretty(input.role_templates) }}</pre></details>
        </section>
      </details>
      <details v-for="output in definition.outputs" :key="output.port_id" open :data-contract-output="output.port_id">
        <summary>输出 {{ output.port_id }}（{{ output.shape }} / {{ output.mode }}）</summary>
        <p>可用条件：{{ pretty(output.availability || '未声明') }}</p>
        <p v-if="output.structure_ref">结构资源：{{ output.structure_ref }}</p>
        <ContractSchemaView v-if="resource(output.structure_ref)" :schema="resource(output.structure_ref)" />
        <div class="contract-table"><table v-if="output.rules.length"><thead><tr><th>生成规则</th><th>来源与字段</th></tr></thead><tbody><tr v-for="(rule,i) in output.rules" :key="i"><td>{{ rule.kind }}</td><td>{{ pretty(rule) }}</td></tr></tbody></table></div>
        <details v-if="output.row_cases" open><summary>不同行类别与 NULL / 有效性约束</summary><div v-for="row in output.row_cases" :key="row.case_id"><h5>{{ row.case_id }} · {{ pretty(row.when) }}</h5><p v-for="(rules,field) in row.field_rules" :key="field">{{ field }}：{{ pretty(rules) }}</p></div></details>
        <details v-if="output.variants?.length"><summary>条件输出变体</summary><section v-for="(variant,i) in output.variants" :key="i"><h5>变体 {{ i + 1 }}</h5><pre>{{ pretty(variant) }}</pre></section></details>
        <p v-if="output.context_semantics">必要解释上下文：{{ pretty(output.context_semantics) }}</p>
      </details>
      <details v-if="definition.resources"><summary>结构资源与嵌套配置</summary><details v-for="(schema,name) in definition.resources" :key="name"><summary>{{ name }}</summary><ContractSchemaView :schema="schema" /></details></details>
      <h4>适用限制</h4><ul><li v-for="(limit,i) in definition.limitations" :key="i">{{ limit }}</li></ul>
      <p>声明、结构检查和业务数据验证分别记录；Schema 中的 default 不会自动配置算法。</p>
      <details><summary>技术详情与完整作者声明 JSON</summary>
        <p>补充说明精确版本：{{ observedDescription?.basis.description_ref || '无' }}</p>
        <pre data-testid="contract-configuration">{{ pretty(definition.configuration) }}</pre><pre data-testid="contract-inputs">{{ pretty(definition.inputs) }}</pre><pre>{{ pretty(definition) }}</pre>
      </details>
    </template>
    <button type="button" :disabled="busy" @click="refresh">刷新声明与来源列表</button>
    <h4>实际对象预检</h4>
    <p>选择已有数据集并填写原有算法参数。来源结构和字段映射由服务端取得；预检不读取懒加载业务行，不保存执行计划。</p>
    <label>选择实际数据集
      <select v-model="selectedSource" aria-label="选择实际数据集" @change="chooseSource">
        <option value="">手动填写 F2 定位符</option>
        <option v-for="source in sources" :key="source" :value="source">dataframe.{{ source }}</option>
      </select>
    </label>
    <label>输入来源定位符（按输入顺序的 JSON 数组）
      <textarea v-model="draft.inputs" aria-label="输入来源定位符" rows="4" spellcheck="false" />
    </label>
    <label>已有算法参数（JSON 对象，值与 SQL WITH 一样为字符串）
      <textarea v-model="draft.parameters" aria-label="已有算法参数" rows="4" spellcheck="false" />
    </label>
    <label>输出目标定位符（可选）
      <textarea v-model="draft.output" aria-label="输出目标定位符" rows="2" spellcheck="false" />
    </label>
    <p>不指定目标时，依赖目标的副输出保持未确定。F2 的具体表目标使用普通表输出路径。</p>
    <button type="button" :disabled="busy" @click="check">检查实际对象</button>
    <div v-if="result" data-testid="contract-check-result">
      <p>检查状态：<strong>{{ result.check.state }}</strong>；业务数据验证：{{ result.check.data_validation }}；输出观察：{{ result.check.output_check }}</p>
      <p>本次契约：{{ result.component.contract_ref || 'missing' }}；组件：{{ result.component.component_revision || 'unknown' }}</p>
      <details open><summary>系统解析的字段映射</summary><pre>{{ pretty(result.check.role_map || {}) }}</pre></details>
      <details open><summary>缺口与判定依据</summary><pre>{{ pretty(result.check.reasons || []) }}</pre></details>
      <details open><summary>各输出预览与精确上下文</summary><pre>{{ pretty(result.check.output_previews || []) }}</pre></details>
      <details><summary>生产有效配置和来源快照</summary><pre>{{ pretty(result.resolved_invocation) }}</pre></details>
    </div>
  </section>
</template>
<script setup>
import { computed, onMounted, reactive, ref, watch } from 'vue'
import ContractSchemaView from './ContractSchemaView.vue'
const props = defineProps({ fullName: { type: String, required: true }, declarationMode: { type: String, default: '' } })
const emit = defineEmits(['maintain-declaration'])
const notes = reactive({ purpose: '', usage_notes: '', examples: [] }), examplesText = ref(''), notesBasis = ref(null), observedDescription = ref(null), notesNotice = ref('')
const notesKey = () => 'flowsql.operator-description.' + props.fullName
const notesChanged = computed(() => !!notesBasis.value && !!observedDescription.value && (notesBasis.value.component_revision !== observedDescription.value.basis.component_revision || notesBasis.value.description_ref !== observedDescription.value.basis.description_ref))
const resource = reference => reference?.startsWith('#/resources/') ? definition.value?.resources?.[reference.slice(12)] : null
function rememberNotes() { if (notesBasis.value) sessionStorage.setItem(notesKey(), pretty({ notes, examplesText: examplesText.value, basis: notesBasis.value })) }
watch(notes, rememberNotes); watch(examplesText, rememberNotes)
async function readNotes() {
  observedDescription.value = await post('semantics/v1/describe', { target: { kind: 'operator', name: props.fullName } })
  if (!notesBasis.value) { notesBasis.value = observedDescription.value.basis; Object.assign(notes, observedDescription.value.description.notes || { purpose: '', usage_notes: '', examples: [] }); examplesText.value = notes.examples.join('\n\n') }
  if (notesChanged.value) notesNotice.value = '组件版本已变化；旧说明和本地原稿不会自动套用，请核对后明确保存。'
}
async function saveNotes() {
  busy.value = true; error.value = ''; notesNotice.value = ''
  try {
    const result = await post('semantics/v1/operators/save-description', { name: props.fullName, expected_component_revision: notesBasis.value.component_revision, expected_description_ref: notesBasis.value.description_ref, notes: { purpose: notes.purpose, usage_notes: notes.usage_notes, examples: examplesText.value.split(/\n\s*\n/).filter(v => v.trim()) } })
    notesBasis.value = null; sessionStorage.removeItem(notesKey()); await readNotes(); notesNotice.value = `补充说明已保存：${result.description_ref}`
  } catch(e) { error.value = e.message; rememberNotes() } finally { busy.value = false }
}
function reviewNotes() {
  if (!window.confirm('已核对当前组件和作者声明？将保留原稿并使用当前版本作为新保存依据。')) return
  notesBasis.value = observedDescription.value.basis; rememberNotes(); notesNotice.value = '已保留原稿，请核对后保存。'
}
const component = ref({}), definition = ref(null), versions = ref([]), sources = ref([])
const selectedReference = ref(''), selectedSource = ref(''), result = ref(null), error = ref(''), busy = ref(false)
const key = () => 'flowsql.operator-check.' + props.fullName
const draft = reactive({ inputs: '[]', parameters: '{}', output: '' })
const pretty = value => JSON.stringify(value, null, 2)
async function post(path, payload) {
  const response = await fetch('/api/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
  const body = await response.json()
  if (!response.ok) throw Error(typeof body.error === 'object' ? `${body.error.code}: ${body.error.message}` : body.error || `HTTP ${response.status}`)
  return body.data || body
}
async function readDefinition() {
  error.value = ''
  try { definition.value = (await post('semantics/v1/resolve', { exact_reference: selectedReference.value })).document }
  catch (e) { definition.value = null; error.value = e.message }
}
async function refresh() {
  busy.value = true; error.value = ''
  try {
    component.value = await post('operators/detail', { name: props.fullName })
    await readNotes()
    const response = await fetch('/api/channels/dataframe')
    if (!response.ok) throw Error(`来源列表 HTTP ${response.status}`)
    sources.value = (await response.json()).channels.map(item => item.name)
    versions.value = []
    if (component.value.contract_ref) {
      let cursor = ''
      do {
        const page = await post('semantics/v1/history', { kind: 'operator_contract', name: props.fullName, limit: 100, cursor })
        versions.value.push(...page.items.map(item => item.exact_reference)); cursor = page.next_cursor
      } while (cursor)
      if (!versions.value.includes(selectedReference.value)) selectedReference.value = component.value.contract_ref
      await readDefinition()
    } else definition.value = null
  } catch (e) { error.value = e.message }
  finally { busy.value = false }
}
function chooseSource() {
  if (selectedSource.value) draft.inputs = pretty([{ kind: 'dataframe', name: selectedSource.value }])
}
async function check() {
  busy.value = true; error.value = ''; result.value = null
  try {
    const dot = props.fullName.indexOf('.')
    const locators = JSON.parse(draft.inputs)
    if (!Array.isArray(locators) || !locators.length) throw Error('请选择至少一个实际数据集')
    const parameters = JSON.parse(draft.parameters)
    if (!parameters || Array.isArray(parameters) || typeof parameters !== 'object' || Object.values(parameters).some(value => typeof value !== 'string')) throw Error('算法参数必须是字符串值的 JSON 对象')
    const payload = { operator: { category: props.fullName.slice(0, dot), name: props.fullName.slice(dot + 1) }, parameters,
      inputs: locators.map((locator, input_index) => ({ input_index, locator })) }
    if (draft.output.trim()) payload.output_locator = JSON.parse(draft.output)
    result.value = await post('semantics/v1/operators/check', payload)
  } catch (e) { error.value = e.message }
  finally { busy.value = false }
}
function restore() {
  result.value = null; selectedReference.value = ''; notesBasis.value = null; observedDescription.value = null; notesNotice.value = ''
  Object.assign(notes, { purpose: '', usage_notes: '', examples: [] }); examplesText.value = ''
  try { const saved = JSON.parse(sessionStorage.getItem(notesKey()) || 'null'); if (saved) { Object.assign(notes, saved.notes); examplesText.value = saved.examplesText; notesBasis.value = saved.basis } } catch {}
  Object.assign(draft, { inputs: '[]', parameters: '{}', output: '' })
  try { const saved = JSON.parse(sessionStorage.getItem(key()) || 'null'); if (saved) Object.assign(draft, saved) } catch {}
  refresh()
}
watch(draft, () => sessionStorage.setItem(key(), JSON.stringify(draft)))
watch(() => props.fullName, restore)
onMounted(restore)
</script>
<style scoped>
.operator-contract { margin: 16px 0; padding: 16px; border: 1px solid var(--border-color); border-radius: 6px; }
label { display: block; margin: 12px 0; }
textarea, select { display: block; box-sizing: border-box; width: 100%; margin-top: 6px; }
textarea, pre, .identity { font-family: monospace; overflow-wrap: anywhere; }
pre { white-space: pre-wrap; max-height: 320px; overflow: auto; background: var(--bg-secondary); padding: 10px; }
textarea, select, button { color: var(--text-primary); background: var(--bg-primary); border: 1px solid var(--border-color); border-radius: 4px; }
.contract-table { max-width: 100%; overflow: auto; } table { width: 100%; min-width: 600px; border-collapse: collapse; } th, td { border: 1px solid var(--border-color); text-align: left; vertical-align: top; padding: 8px; overflow-wrap: anywhere; } .contract-group { min-width: 0; } summary { cursor: pointer; }
button { cursor: pointer; margin: 8px 8px 8px 0; padding: 6px 12px; }
[role=alert] { color: var(--el-color-danger); }
</style>
