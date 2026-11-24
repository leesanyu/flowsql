<template>
  <section class="channel-description" data-testid="channel-description">
    <template v-if="readOnly">
      <div class="heading"><h3>通道说明 · 只读</h3><button v-if="!initialObservation" :disabled="busy" @click="refresh">重试读取</button></div>
      <p v-if="error" role="alert">{{ error }}</p>
      <ChannelDescriptionReading v-if="observation" :observation="observation" />
    </template>
    <template v-else>
    <div class="heading"><h3>数据说明</h3><div class="heading-actions"><button :disabled="busy || !observation?.description.document" @click="download">下载 JSON</button><button :disabled="busy || observation?.structure.state !== 'known'" @click="save">保存</button><button :disabled="busy" @click="refresh">刷新</button></div></div>
    <p class="subject">{{ locator.name }}<template v-if="locator.table"> / {{ locator.schema ? locator.schema + '.' : '' }}{{ locator.table }}</template></p>
    <p v-if="error" role="alert">{{ error }}</p><p v-if="notice" role="status">{{ notice }}</p>
    <p v-if="busy && !observation" role="status">正在读取当前结构与说明…</p>
    <p v-if="loadingDataFrame" role="status">正在加载 DataFrame，完成后展示当前字典…</p>
    <p v-else-if="observation?.structure.state === 'pending'" role="status">结构尚未加载；点击“刷新”重试加载并读取说明。</p>
    <p v-if="observation" class="reading-basis">{{ descriptionSource }}<template v-if="observation.basis.dictionary_ref"> · {{ observation.basis.dictionary_ref }}</template> · {{ descriptionState }}</p>
    <p v-if="observation?.structure.state === 'unsupported'">{{ observation.structure.reason }}</p>
    <p v-if="observation?.description.state === 'needs_review'" role="status">来源或绑定需要核对；已有说明与草稿已保留。</p>
    <template v-if="observation?.structure.state === 'known'">
      <fieldset :disabled="busy || observation.structure.state !== 'known'">
        <div class="object-notes">
          <div><label for="object-description">对象说明</label><div class="description-editor"><textarea v-auto-grow id="object-description" aria-label="对象说明" :value="value('description')" @input="edit('description', $event.target.value)" /></div></div>
          <div><label for="row-meaning">一行表示什么（可选）</label><div class="description-editor"><textarea v-auto-grow id="row-meaning" aria-label="一行表示什么" :value="value('row_meaning')" @input="edit('row_meaning', $event.target.value)" /></div></div>
        </div>
        <div class="object-summary"><span>行语义</span><SemanticValue :value="observation.description.document?.entity?.row_semantics" /></div>
        <details ref="objectDetails" class="object-semantics" data-testid="object-semantics" @toggle="readObjectDetails">
          <summary>对象语义与关联<template v-if="relations.length">（{{ relations.length }}）</template></summary>
          <dl><template v-for="k in ['identity_fields','revision_field','time_fields','limitations']" :key="k"><dt>{{ labels[k] }}</dt><dd :data-entity-semantic="k"><SemanticValue :value="observation.description.document?.entity?.[k]" :field-names="fieldNames" /></dd></template></dl>
          <h4>关联关系</h4>
          <SemanticValue v-if="!relations.length" :value="observation.description.document?.entity?.relations" />
          <small v-else>{{ declarationStatus(observation.description.document?.entity?.relations) }}</small>
          <article v-for="(relation,i) in relations" :id="'object-relation-' + i" :key="i" class="object-relation" :data-joint-relation="i"><strong>关联 {{ i + 1 }} · {{ relationTargets[relation.dictionary_ref]?.document?.title || relation.dictionary_ref }}</strong>
            <p v-if="relationTargets[relation.dictionary_ref]?.error" role="status">目标暂不可读，原关系已保留：{{ relationTargets[relation.dictionary_ref].error }}</p>
            <p v-for="(id,name) in relation.field_map" :key="name">{{ name }} → {{ relationTargets[relation.dictionary_ref]?.document?.fields.find(f => f.field_id === id)?.name || id }}</p>
            <code>{{ relation.dictionary_ref }}</code>
          </article>
        </details>
        <div v-show="!fieldsCollapsed" class="field-toolbar"><input v-model="filter" type="search" aria-label="搜索字段名" placeholder="搜索字段名…" /><span>{{ visibleFields.length }} / {{ observation.structure.fields?.length || 0 }} 字段</span></div>
        <div class="fields-scroll">
          <table class="fields"><thead><tr><th>字段 / 类型</th><th>单位</th><th>时间</th><th><div class="field-header-actions"><span>关联</span><button type="button" :title="fieldsCollapsed ? '展开字段表' : '收起字段表'" :aria-label="fieldsCollapsed ? '展开字段表' : '收起字段表'" :aria-expanded="!fieldsCollapsed" @click="fieldsCollapsed = !fieldsCollapsed"><el-icon aria-hidden="true"><ArrowDown v-if="fieldsCollapsed" /><ArrowUp v-else /></el-icon></button></div></th></tr></thead>
            <tbody v-show="!fieldsCollapsed"><template v-for="f in visibleFields" :key="f.name">
              <tr class="field-main-row" :data-description-field="f.name">
                <td><strong>{{ f.name }}</strong><span v-if="originalField(f.name)?.title" class="field-title">{{ originalField(f.name).title }}</span><span v-for="k in ['role','measure_kind']" :key="k" class="semantic-tag" v-show="originalField(f.name)?.[k]?.state === 'declared'">{{ labels[k] }}：{{ declared(originalField(f.name)?.[k]) }}</span></td>
                <td class="field-unit">
                  <select v-if="['unix_timestamp', 'duration'].includes(timeConfig(f.name).mode)" :aria-label="f.name + ' 时间单位'" :value="timeConfig(f.name).unit || ''" @change="editTime(f.name, 'unit', $event.target.value)"><option value="">请选择单位</option><option v-for="unit in ['s','ms','us','ns']" :key="unit" :value="unit">{{ unit }}</option></select>
                  <span v-else-if="timeConfig(f.name).mode === 'datetime'">{{ fieldValue(f.name, 'unit') || '精度未指定' }}</span>
                  <span v-else-if="timeConfig(f.name).unsupported">{{ fieldValue(f.name, 'unit') || '单位未声明' }}</span>
                  <input v-else :aria-label="f.name + ' 单位'" :value="fieldValue(f.name, 'unit') || ''" @input="editField(f.name, 'unit', $event.target.value)" />
                </td>
                <td class="field-time">
                  <p v-if="timeConfig(f.name).unsupported" class="supplement" role="status">当前时间定义暂不支持编辑</p>
                  <select v-else class="time-select" :aria-label="f.name + ' 时间类型'" :value="timeConfig(f.name).mode || ''" @change="changeTimeMode(f.name, $event.target.value)"><option v-for="option in timeOptions(f.name)" :key="option.value" :value="option.value">{{ option.label }}</option></select>
                </td>
                <td class="field-relation"><button :aria-label="f.name + ' 关联'" @click="relationField = relationField === f.name ? '' : f.name">关联<template v-if="fieldRelations(f.name).length">（{{ fieldRelations(f.name).length }}）</template></button></td>
              </tr>
              <tr class="field-details-row">
                <td><div class="type" :title="json(f.type)">{{ typeSummary(f.type) }} · {{ f.nullable === null ? '可空性未知' : f.nullable ? '可空' : '非空' }}</div></td>
                <td></td>
                <td class="field-time">
                  <p v-if="timeConfig(f.name).mode === 'unix_timestamp'" class="supplement">基准：Unix epoch<template v-if="timeConfig(f.name).timezone"> · 时区：{{ timeConfig(f.name).timezone }}</template></p>
                  <label v-if="timeConfig(f.name).mode === 'datetime'">时区（可选）<input :aria-label="f.name + ' 时区'" :value="timeConfig(f.name).timezone || ''" placeholder="未指定时区" @input="editTime(f.name, 'timezone', $event.target.value)" /></label>
                  <p v-if="timeFacts(f.name).conflict" role="status">来源时间事实与已有声明不同；原声明保留，请核对后修改。</p>
                  <details v-if="timeConfig(f.name).unsupported" class="time-definition"><summary>查看时间定义</summary><p class="supplement">{{ timeSummary(effectiveTime(f.name)) }}</p></details>
                </td>
                <td class="field-relation"><a v-for="i in fieldRelationIndices(f.name)" :key="i" :href="'#object-relation-' + i" @click.prevent="openRelation(i)">查看关联 {{ i + 1 }}</a></td>
              </tr>
              <tr class="field-description-row"><td colspan="4"><div class="description-editor"><textarea v-auto-grow :aria-label="f.name + ' 字段说明'" placeholder="字段说明" :value="fieldValue(f.name, 'definition')" @input="editField(f.name, 'definition', $event.target.value)" /><p v-if="timeDescription(effectiveTime(f.name), fieldValue(f.name, 'definition'))" class="supplement">时间含义：{{ timeDescription(effectiveTime(f.name), fieldValue(f.name, 'definition')) }}</p><p v-for="note in fieldNotes(f.name, ['value_domain', 'aggregation'])" :key="note" class="supplement">{{ note }}</p></div><p v-for="note in fieldNotes(f.name, ['null_meaning', 'validity'])" :key="note" class="supplement">{{ note }}</p><details class="field-semantics" :data-field-semantics="f.name"><summary>完整字段语义</summary><dl><template v-for="k in semanticKeys.field" :key="k"><dt>{{ labels[k] }}</dt><dd :data-field-semantic="k"><SemanticValue :value="originalField(f.name)?.[k]" :field-names="fieldNames" /></dd></template></dl></details></td></tr>
            </template></tbody>
          </table>
          <p v-if="!fieldsCollapsed && !visibleFields.length && filter">没有匹配的字段；清空搜索可恢复全部字段。</p>
        </div>
      </fieldset>
      <button v-if="changedBasis" :disabled="busy" @click="rebase">核对后使用当前版本继续编辑</button>
    </template>
    <div v-if="relationField" class="relation-modal" role="dialog" aria-modal="true" aria-label="新增关联" tabindex="-1" @click.self="relationField = ''" @keydown.esc="relationField = ''">
      <div class="relation-modal-content">
        <div class="relation-modal-heading"><h4>新增关联</h4><button aria-label="关闭关联编辑" @click="relationField = ''">关闭关联</button></div>
        <ChannelRelationEditor :model-value="relations" :source-fields="observation.structure.fields" :field-name="relationField" :targets="relationTargets" :draft-key="key()" start-new @update:model-value="changes.relations = $event" @target="rememberTarget" />
      </div>
    </div>
    <details v-if="observation"><summary>技术详情（最近读取结果）</summary><p class="supplement">以下 JSON 为最近读取结果，不包含未保存草稿。</p><button @click="copy">复制 JSON</button><pre data-testid="description-json">{{ prettyJson(observation) }}</pre></details>
    </template>
  </section>
</template>
<script setup>
import { computed, h, onMounted, ref, watch } from 'vue'
import { ElMessageBox } from 'element-plus'
import { ArrowDown, ArrowUp } from '@element-plus/icons-vue'
import ChannelRelationEditor from './ChannelRelationEditor.vue'
import SemanticValue from './SemanticValue.vue'
import ChannelDescriptionReading from './ChannelDescriptionReading.vue'
import api from '../api/index.js'
import { semanticsCall as call, clone, json, parseJson, labels, semanticKeys, prettyJson, downloadJson, autoGrow as vAutoGrow } from '../utils/dataSemantics.js'
import { timeEditorConfig, timeDescription, timeSummary } from '../utils/channelDescriptionTime.js'
const props = defineProps({ locator: { type: Object, required: true }, readOnly: Boolean, initialObservation: Object })
const observation = ref(null), basis = ref(null), changes = ref({}), busy = ref(false), error = ref(''), notice = ref(''), filter = ref(''), confirmations = ref([]), relationField = ref(''), relationTargets = ref({})
const fieldsCollapsed = ref(false), objectDetails = ref(null)
const loadingDataFrame = ref(false)
const fieldNames = computed(() => Object.fromEntries((observation.value?.description.document?.fields || []).map(f => [f.field_id, observation.value?.context.binding?.field_map?.[f.field_id] || f.name])))
const descriptionSource = computed(() => ({ bound_dictionary: '已保存绑定字典', producer_default: '生产者默认说明 · 未独立保存' }[observation.value?.description.origin] || '当前读取说明'))
const descriptionState = computed(() => ({ present: '已有说明', missing: '尚无说明', needs_review: '待核对' }[observation.value?.description.state]))
const declarationStatus = v => ({ declared: '已声明', unknown: '未知', not_applicable: '不适用' }[v?.state || 'unknown']) + ' · ' + ({ manual: '人工填写', imported: '导入', producer: '生产者声明', derived: '派生' }[v?.origin] || '来源未声明')
const key = () => 'flowsql.channel-description.' + json(props.locator)
const changedBasis = computed(() => !!basis.value && !!observation.value && json(basis.value) !== json(observation.value.basis))
const visibleFields = computed(() => (observation.value?.structure.fields || []).filter(f => f.name.toLowerCase().includes(filter.value.toLowerCase())))
const declared = v => v?.state === 'declared' ? v.value : ''
const originalField = name => {
  const binding = observation.value?.context.binding
  return observation.value?.description.document?.fields?.find(f => (binding?.field_map?.[f.field_id] || f.name) === name)
}
// The in operator tracks additions to Vue reactive drafts; hasOwn keeps inherited keys out.
const own = (v, k) => !!v && k in v && Object.hasOwn(v, k)
const fieldValue = (name, k) => {
  const edit = changes.value.fields?.[name]
  if (own(edit, k)) return edit[k] ?? ''
  const v = declared(originalField(name)?.[k])
  return k === 'unit' ? v?.code || (originalField(name)?.unit?.state !== 'not_applicable' ? declared(timeFacts(name).unit)?.code : '') || '' : v
}
const value = k => own(changes.value, k) ? changes.value[k] ?? '' : declared(k === 'description' ? observation.value?.description.document?.description : observation.value?.description.document?.entity?.[k])
const readable = v => typeof v === 'string' ? v : Array.isArray(v) ? v.map(readable).join('；') : v && typeof v === 'object' ? typeof v.description === 'string' ? v.description : v.label && v.code ? `${v.label}（${v.code}）` : Object.entries(v).map(([k, x]) => `${k}: ${readable(x)}`).join('；') : String(v ?? '')
const notes = (object, keys) => keys.filter(k => object?.[k]?.state === 'declared').map(k => `${labels[k]}：${readable(object[k].value)}`)
const fieldNotes = (name, keys) => notes(originalField(name), keys)
const typeSummary = type => type ? type.name + (Object.keys(type.parameters || {}).length ? ' ' + readable(type.parameters) : '') : '类型未知'
const relations = computed(() => {
  if (own(changes.value, 'relations')) return changes.value.relations
  const saved = declared(observation.value?.description.document?.entity?.relations)
  const map = observation.value?.context.binding?.field_map || {}
  return Array.isArray(saved) ? saved.map(relation => ({ dictionary_ref: relation.dictionary_ref, field_map: Object.fromEntries(Object.entries(relation.field_map).map(([id, target]) => [map[id] || observation.value.description.document.fields.find(f => f.field_id === id)?.name || id, target])) })) : []
})
const fieldRelations = name => relations.value.filter(r => own(r.field_map, name))
const fieldRelationIndices = name => relations.value.flatMap((r, i) => own(r.field_map, name) ? [i] : [])
function openRelation(index) {
  objectDetails.value.open = true
  document.getElementById('object-relation-' + index).scrollIntoView({ block: 'center', inline: 'nearest' })
}
function readObjectDetails(event) { if (event.target.open) readRelationTargets() }
function rememberTarget(exact, document) { relationTargets.value[exact] = { document } }
async function readRelationTargets() {
  const refs = [...new Set(relations.value.map(r => r.dictionary_ref))]
  await Promise.allSettled(refs.map(async exact => {
    if (relationTargets.value[exact]?.document) return
    try {
      const field_ids = [...new Set(relations.value.filter(r => r.dictionary_ref === exact).flatMap(r => Object.values(r.field_map)))]
      const result = await call('resolve', { exact_reference: exact, view: 'fields', field_ids })
      rememberTarget(exact, { fields: result.fields, title: exact })
    }
    catch (e) { relationTargets.value[exact] = { error: e.message } }
  }))
}
const timeLabels = { unix_timestamp: 'unix时间戳', datetime: '日期时间', duration: '时长' }
const timeFacts = name => observation.value?.context.editor_fields?.[name] || {}
const effectiveTime = name => {
  const saved = originalField(name)?.time_semantics
  return declared(saved) || (saved?.state !== 'not_applicable' ? declared(timeFacts(name).time_semantics) : null)
}
const timeConfig = name => {
  const edits = changes.value.fields?.[name]
  return own(edits, 'time') ? edits.time || {} : timeEditorConfig(effectiveTime(name), timeFacts(name).modes)
}
const timeOptions = name => {
  const current = timeConfig(name), options = [{ value: '', label: '非时间' }]
  for (const mode of ['unix_timestamp', 'datetime', 'duration'])
    if (timeFacts(name).modes?.includes(mode) || current.mode === mode) options.push({ value: mode, label: timeLabels[mode] })
  return options
}
function changeTimeMode(name, mode) {
  const previous = timeConfig(name)
  const unit = previous.unit || fieldValue(name, 'unit')
  const next = !mode ? null : mode === 'datetime' ? { mode, ...(previous.timezone ? { timezone: previous.timezone } : {}) } : { mode, unit: ['s','ms','us','ns'].includes(unit) ? unit : '' }
  changes.value.fields ||= {}; changes.value.fields[name] ||= {}; changes.value.fields[name].time = next
  if (mode) delete changes.value.fields[name].unit
}
function editTime(name, key, v) {
  const current = timeConfig(name), mode = current.mode
  const next = mode === 'datetime' ? { mode, ...(key === 'timezone' && v ? { timezone: v } : {}) } : { mode, unit: key === 'unit' ? v : current.unit }
  changes.value.fields ||= {}; changes.value.fields[name] ||= {}; changes.value.fields[name].time = next
}
function edit(k, v) { changes.value[k] = v }
function editField(name, k, v) { changes.value.fields ||= {}; changes.value.fields[name] ||= {}; changes.value.fields[name][k] = v }
function persist() { if (basis.value) sessionStorage.setItem(key(), json({ basis: basis.value, editor_changes: changes.value })) }
watch(changes, persist, { deep: true })
function differenceText(difference) {
  const name = difference.source_name || '来源'
  switch (difference.kind) {
    case 'added': return `新增字段：${name}`
    case 'removed': return `删除字段：${name}`
    case 'renamed': return `字段重命名：${readable(difference.before)} → ${readable(difference.after)}`
    case 'type_changed': return `${name}：类型 ${typeSummary(difference.before)} → ${typeSummary(difference.after)}`
    case 'nullable_changed': {
      const nullable = value => value === null ? '可空性未知' : value ? '可空' : '非空'
      return `${name}：${nullable(difference.before)} → ${nullable(difference.after)}`
    }
    case 'order_changed': return `字段顺序：${readable(difference.before)} → ${readable(difference.after)}`
    case 'producer_changed': return '来源声明发生变化，已有人工说明保留。'
    case 'identity_changed': return '来源对象已更换，请检查已有说明是否仍适用于当前对象。'
    default: return `${name}：结构信息发生变化。`
  }
}
async function inspectSource() {
  const checks = observation.value.checks || {}
  const previousFields = observation.value.context.binding?.source_snapshot?.fields
  const sourceOrderChanged = !previousFields || json(previousFields.map(field => field.name)) !== json((observation.value.structure.fields || []).map(field => field.name))
  // Stable dictionary field order can differ after additions; compare source observations for real reordering.
  const differences = (checks.differences || []).filter(difference => difference.kind !== 'order_changed' || sourceOrderChanged)
  const required = checks.required_confirmations || []
  // An unavailable native identity is an observation limit, not an observed source change.
  if (differences.length || required.some(key => key !== 'identity')) {
    const message = h('div', { style: { maxHeight: '55vh', overflow: 'auto', overflowWrap: 'anywhere' } }, [
      h('p', differences.length ? '已自动检测当前来源，与已保存说明存在以下差异：' : '已有说明需要按当前来源重新核对。'),
      h('ul', { style: { paddingLeft: '20px' } }, differences.map(difference => h('li', differenceText(difference)))),
      ...(differences.some(difference => difference.kind === 'removed') ? [h('p', '已删除字段将在下次保存时移除，其说明不会迁移到其他字段。')] : []),
      h('p', '现有说明与草稿已保留，查看后可继续编辑；点击“保存”才会更新说明。')
    ])
    await ElMessageBox.alert(message, differences.length ? '来源结构已变化' : '来源说明需要核对', {
      type: 'warning', confirmButtonText: '知道了', showClose: false, closeOnClickModal: false, closeOnPressEscape: false
    })
  }
  confirmations.value = [...required]
}
async function refresh() {
  if (props.readOnly) {
    busy.value = true; error.value = ''
    try { observation.value = props.initialObservation || await call('describe', { target: { kind: 'channel', locator: props.locator } }) }
    catch (e) { error.value = e.message } finally { busy.value = false }
    return
  }
  busy.value = true; error.value = ''; confirmations.value = []
  try {
    let loadDataFrame = false
    try {
      observation.value = await call('describe', { target: { kind: 'channel', locator: props.locator } })
      loadDataFrame = props.locator.kind === 'dataframe' && observation.value.structure.state === 'pending'
    } catch (e) {
      if (props.locator.kind !== 'dataframe' || !e.message.startsWith('SOURCE_UNAVAILABLE：CSV_LOAD_FAILED')) throw e
      loadDataFrame = true
    }
    if (loadDataFrame) {
      loadingDataFrame.value = true
      try {
        await api.previewDfChannel(props.locator.name, 1, 1)
      } catch (e) {
        error.value = '加载 DataFrame 失败：' + (e.response?.data?.code || e.response?.data?.error || e.message)
        return
      } finally { loadingDataFrame.value = false }
      observation.value = await call('describe', { target: { kind: 'channel', locator: props.locator } })
    }
    if (!basis.value || !Object.keys(changes.value).length) basis.value = clone(observation.value.basis)
    if (changedBasis.value) notice.value = '当前来源或说明版本已变化；原稿保留，请核对差异后继续。'
    if (objectDetails.value?.open) await readRelationTargets()
    await inspectSource()
  } catch (e) { error.value = e.message } finally { busy.value = false }
}
async function download() {
  busy.value = true; error.value = ''
  const observed = observation.value
  try {
    const reference = observed.basis.dictionary_ref
    if (reference) {
      const exported = await call('export', { exact_reference: reference, format: 'json' })
      downloadJson(reference, exported.content); notice.value = '已下载已保存字典：' + reference
    } else {
      downloadJson((props.locator.table || props.locator.name) + '.未独立保存', prettyJson(observed.description.document))
      notice.value = '已下载最近读取的生产者默认说明（未独立保存），不含草稿。'
    }
  } catch (e) { error.value = e.message } finally { busy.value = false }
}
function rebase() {
  if (!window.confirm('已核对当前结构和说明？保留原稿中的语义修改，按当前版本重新提交；缺失字段不会自动迁移。')) return
  basis.value = clone(observation.value.basis); notice.value = '已使用当前版本作为依据；请检查保留的原稿。'; persist()
}
async function save() {
  busy.value = true; error.value = ''; notice.value = ''
  try {
    const r = await call('channels/save-description', { locator: props.locator, basis: basis.value, editor_changes: changes.value, confirmations: confirmations.value })
    if (r.status !== 'applied') { notice.value = `定义已保存：${r.dictionary_ref}；尚未应用：${r.error?.code} ${r.error?.message}。原稿已保留，可重试。`; persist(); return }
    notice.value = '说明已保存'; changes.value = {}; basis.value = r.basis; sessionStorage.removeItem(key())
    await refresh()
  } catch (e) {
    persist()
    if (/^(SOURCE_CHANGED|CONFIRMATION_REQUIRED)[：:]/.test(e.message)) await refresh()
    error.value = e.message
  } finally { busy.value = false }
}
async function copy() { try { await navigator.clipboard.writeText(json(observation.value)); notice.value = 'JSON 已复制' } catch { notice.value = '可在技术详情中选择并复制 JSON' } }
onMounted(async () => {
  if (props.readOnly) { await refresh(); return }
  let draftError = ''
  try {
    const draft = parseJson(sessionStorage.getItem(key()) || 'null')
    if (draft) {
      const object = value => value && typeof value === 'object' && !Array.isArray(value)
      if (!object(draft.basis) || !object(draft.editor_changes) || Object.keys(draft).some(k => !['basis', 'editor_changes'].includes(k))) throw Error('invalid draft')
      basis.value = draft.basis; changes.value = draft.editor_changes
    }
  } catch { draftError = '本地草稿无法读取，请先备份后重新编辑。' }
  await refresh()
  if (draftError) error.value = draftError
})
</script>
<style scoped>
.channel-description { color: var(--text-primary); min-width: 0; max-width: 100%; }
.heading { flex-wrap: wrap; display: flex; align-items: center; justify-content: space-between; gap: 8px; } h3 { margin: 0; }
.heading-actions { flex-wrap: wrap; display: flex; align-items: center; gap: 8px; flex-shrink: 0; } .heading-actions button { margin: 0; }
.subject, pre { overflow-wrap: anywhere; } .subject { margin: 4px 0 12px; }
label { display: block; margin: 6px 0; } .object-notes { display: grid; grid-template-columns: 2fr 1fr; gap: 12px; }
input:not([type=checkbox]), textarea, select { box-sizing: border-box; max-width: 100%; width: 100%; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 4px; padding: 6px; }
select option { color: var(--text-primary); background: var(--bg-card); }
input::placeholder { color: var(--text-secondary); opacity: 1; } textarea { min-height: 52px; resize: vertical; overflow-y: hidden; } fieldset { border: 0; padding: 0; min-width: 0; }
.description-editor { min-width: 0; border: 1px solid var(--border-color); border-radius: 4px; background: var(--bg-card); }
.description-editor:focus-within { outline: 1px solid var(--el-color-primary); }
.description-editor textarea { display: block; border: 0; background: transparent; outline: none; }
.description-editor .supplement { margin: 0; padding: 0 6px 6px; }
.field-toolbar { display: flex; align-items: center; gap: 12px; margin: 10px 0; color: var(--text-secondary); font-size: 12px; } .field-toolbar input { width: 240px; }
.fields-scroll { max-width: 100%; overflow: auto; } .fields { min-width: 620px; width: 100%; table-layout: fixed; border-collapse: collapse; }
th, td { text-align: left; vertical-align: top; padding: 8px; border-bottom: 1px solid var(--border-color); overflow-wrap: anywhere; } th:first-child { width: 29%; } th:nth-child(2) { width: 18%; } th:nth-child(3) { width: 29%; } th:last-child { width: 24%; }
.fields th { vertical-align: middle; } .field-header-actions { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
.field-header-actions button { display: inline-flex; align-items: center; justify-content: center; flex-shrink: 0; width: 28px; height: 28px; margin: 0; padding: 0; border-color: transparent; background: transparent; font-size: 16px; }
.field-header-actions button:not(:disabled):hover { background: var(--el-fill-color-light); border-color: var(--border-color); }
.field-main-row td { border-bottom: 0; padding-bottom: 6px; vertical-align: middle; }
.field-main-row input, .field-main-row select, .field-main-row button { display: block; height: 32px; margin: 0; padding: 4px 8px; font: inherit; }
.field-main-row strong, .field-main-row .field-unit > span { display: block; }
.field-details-row td { border-bottom: 0; padding-top: 0; padding-bottom: 6px; }
.field-details-row .type, .field-details-row .supplement, .field-details-row label { margin: 0 0 6px; }
.field-description-row td { padding-top: 0; } .field-description-row textarea { width: 100%; }
.field-description-row textarea::placeholder { color: var(--text-secondary); opacity: 1; }
.relation-modal { position: fixed; inset: 0; z-index: 3000; display: flex; align-items: center; justify-content: center; padding: 24px; background: rgb(0 0 0 / 78%); }
.relation-modal-content { width: min(720px, calc(100vw - 48px)); max-height: min(720px, calc(100vh - 48px)); overflow: auto; box-sizing: border-box; padding: 16px; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 8px; box-shadow: 0 20px 60px rgb(0 0 0 / 45%); }
.relation-modal-heading { display: flex; align-items: center; justify-content: space-between; gap: 12px; } .relation-modal-heading h4 { margin: 0; font-size: 16px; }
.type, .supplement { color: var(--text-secondary); font-size: 12px; line-height: 1.5; white-space: pre-wrap; overflow-wrap: anywhere; } .supplement { margin: 6px 0; }
pre { white-space: pre-wrap; max-height: 280px; overflow: auto; background: var(--bg-secondary); padding: 10px; }
button { color: var(--text-primary); background: var(--bg-secondary); border: 1px solid var(--border-color); border-radius: 4px; padding: 7px 12px; margin: 6px 8px 6px 0; cursor: pointer; } button:disabled { opacity: .5; cursor: default; }
[role=alert] { color: var(--el-color-danger); } [role=status] { color: var(--el-color-primary); } details { margin: 12px 0; } summary { cursor: pointer; }
.reading-basis { color: var(--text-secondary); font-size: 12px; overflow-wrap: anywhere; }
.object-summary { display: flex; gap: 12px; align-items: baseline; margin: 12px 0; }
.object-semantics { padding: 10px; background: var(--bg-secondary); border-radius: 6px; }
dl { display: grid; grid-template-columns: 120px minmax(0, 1fr); gap: 10px; } dt { color: var(--text-secondary); } dd { margin: 0; min-width: 0; }
.object-relation { padding: 12px 0; border-top: 1px solid var(--border-color); overflow-wrap: anywhere; } .object-relation p { margin: 4px 0; }
.field-title, .semantic-tag { display: block; color: var(--text-secondary); font-size: 12px; margin-top: 4px; }
.field-relation a { display: block; color: var(--el-color-primary); margin-bottom: 4px; }
@media (max-width: 700px) { .object-notes { grid-template-columns: minmax(0, 1fr); } dl { grid-template-columns: minmax(0, 1fr); } dt { margin-top: 8px; } .heading-actions { gap: 4px; } }
</style>
