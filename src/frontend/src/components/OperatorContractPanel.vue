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
    <template v-if="definition">
      <p>{{ definition.title }}</p>
      <details open><summary>参数约束、默认值与影响</summary><pre data-testid="contract-configuration">{{ pretty(definition.configuration) }}</pre></details>
      <details open><summary>输入角色与字段选择规则</summary><pre data-testid="contract-inputs">{{ pretty(definition.inputs) }}</pre></details>
      <details v-for="output in definition.outputs" :key="output.port_id" open>
        <summary>输出 {{ output.port_id }}（{{ output.shape }} / {{ output.mode }}）</summary>
        <pre>{{ pretty(output) }}</pre>
      </details>
      <details v-if="definition.resources"><summary>闭合资源与必要解释上下文</summary><pre>{{ pretty(definition.resources) }}</pre></details>
      <p>未知项与适用限制：{{ pretty(definition.limitations) }}</p>
      <p>声明、结构检查和业务数据验证分别记录；Schema 中的 default 不会自动配置算法。</p>
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
import { onMounted, reactive, ref, watch } from 'vue'
const props = defineProps({ fullName: { type: String, required: true } })
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
  result.value = null; selectedReference.value = ''
  Object.assign(draft, { inputs: '[]', parameters: '{}', output: '' })
  try { const saved = JSON.parse(sessionStorage.getItem(key()) || 'null'); if (saved) Object.assign(draft, saved) } catch {}
  refresh()
}
watch(draft, () => sessionStorage.setItem(key(), JSON.stringify(draft)))
watch(() => props.fullName, restore)
onMounted(restore)
</script>
<style scoped>
.operator-contract { margin: 16px 0; padding: 16px; border: 1px solid #dcdfe6; border-radius: 6px; }
label { display: block; margin: 12px 0; }
textarea, select { display: block; box-sizing: border-box; width: 100%; margin-top: 6px; }
textarea, pre, .identity { font-family: monospace; overflow-wrap: anywhere; }
pre { white-space: pre-wrap; max-height: 320px; overflow: auto; background: #f7f8fa; padding: 10px; }
button { cursor: pointer; margin: 8px 8px 8px 0; padding: 6px 12px; }
[role=alert] { color: #b42318; }
</style>
