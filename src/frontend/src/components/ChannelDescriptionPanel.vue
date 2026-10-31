<template>
  <section class="channel-description" data-testid="channel-description">
    <div class="heading"><h3>数据说明</h3><div class="heading-actions"><button :disabled="busy || !!legacy || observation?.structure.state !== 'known'" @click="save">保存</button><button :disabled="busy" @click="refresh">刷新</button></div></div>
    <p class="subject">{{ locator.name }}<template v-if="locator.table"> / {{ locator.schema ? locator.schema + '.' : '' }}{{ locator.table }}</template></p>
    <p v-if="error" role="alert">{{ error }}</p><p v-if="notice" role="status">{{ notice }}</p>
    <p v-if="busy && !observation" role="status">正在读取当前结构与说明…</p>
    <p v-if="busy && observation?.structure.state === 'pending' && locator.kind === 'dataframe'" role="status">正在加载 DataFrame…</p>
    <p v-if="observation?.structure.state === 'unsupported'">{{ observation.structure.reason }}</p>
    <p v-if="observation?.description.state === 'needs_review'" role="status">来源或绑定需要核对；已有说明与草稿已保留。</p>
    <div v-if="legacy && observation?.structure.state === 'known'" role="status">
      <p>检测到旧版草稿，已完整保留。请先保存旧版草稿，再填写新的说明。</p>
      <details><summary>查看旧版草稿</summary><pre>{{ json(legacy) }}</pre></details>
      <button :disabled="busy" @click="save">保存旧版草稿</button>
    </div>
    <template v-if="observation?.structure.state === 'known'">
      <fieldset :disabled="busy || !!legacy || observation.structure.state !== 'known'">
        <div class="object-notes">
          <label>对象说明<textarea aria-label="对象说明" :value="value('description')" @input="edit('description', $event.target.value)" /></label>
          <label>一行表示什么（可选）<textarea aria-label="一行表示什么" :value="value('row_meaning')" @input="edit('row_meaning', $event.target.value)" /></label>
        </div>
        <p v-for="note in entityNotes" :key="note" class="supplement">{{ note }}</p>
        <div v-show="!fieldsCollapsed" class="field-toolbar"><input v-model="filter" type="search" aria-label="搜索字段名" placeholder="搜索字段名…" /><span>{{ visibleFields.length }} / {{ observation.structure.fields?.length || 0 }} 字段</span></div>
        <div class="fields-scroll">
          <table class="fields"><thead><tr><th>字段 / 类型</th><th>单位</th><th>时间</th><th><div class="field-header-actions"><span>关联</span><button type="button" :title="fieldsCollapsed ? '展开字段表' : '收起字段表'" :aria-label="fieldsCollapsed ? '展开字段表' : '收起字段表'" :aria-expanded="!fieldsCollapsed" @click="fieldsCollapsed = !fieldsCollapsed"><el-icon aria-hidden="true"><ArrowDown v-if="fieldsCollapsed" /><ArrowUp v-else /></el-icon></button></div></th></tr></thead>
            <tbody v-show="!fieldsCollapsed"><template v-for="f in visibleFields" :key="f.name">
              <tr class="field-main-row" :data-description-field="f.name">
                <td><strong>{{ f.name }}</strong></td>
                <td class="field-unit">
                  <select v-if="['unix_timestamp', 'duration'].includes(timeConfig(f.name).mode)" :aria-label="f.name + ' 时间单位'" :value="timeConfig(f.name).unit || ''" @change="editTime(f.name, 'unit', $event.target.value)"><option value="">请选择单位</option><option v-for="unit in ['s','ms','us','ns']" :key="unit" :value="unit">{{ unit }}</option></select>
                  <span v-else-if="timeConfig(f.name).mode === 'datetime'">{{ fieldValue(f.name, 'unit') || '精度未指定' }}</span>
                  <input v-else :aria-label="f.name + ' 单位'" :value="fieldValue(f.name, 'unit') || ''" @input="editField(f.name, 'unit', $event.target.value)" />
                </td>
                <td class="field-time">
                  <select class="time-select" :aria-label="f.name + ' 时间类型'" :value="timeSelection(f.name)" @change="changeTimeMode(f.name, $event.target.value)"><option v-for="option in timeOptions(f.name)" :key="option.value" :value="option.value" :disabled="option.disabled">{{ option.label }}</option></select>
                </td>
                <td class="field-relation"><button :aria-label="f.name + ' 关联'" @click="relationField = relationField === f.name ? '' : f.name">关联<template v-if="fieldRelations(f.name).length">（{{ fieldRelations(f.name).length }}）</template></button></td>
              </tr>
              <tr class="field-details-row">
                <td><div class="type" :title="json(f.type)">{{ typeSummary(f.type) }} · {{ f.nullable === null ? '可空性未知' : f.nullable ? '可空' : '非空' }}</div></td>
                <td></td>
                <td class="field-time">
                  <p v-if="timeConfig(f.name).mode === 'unix_timestamp'" class="supplement">基准：1970-01-01T00:00:00Z</p>
                  <label v-if="timeConfig(f.name).mode === 'datetime'">时区（可选）<input :aria-label="f.name + ' 时区'" :value="timeConfig(f.name).timezone || ''" placeholder="未指定时区" @input="editTime(f.name, 'timezone', $event.target.value)" /></label>
                  <p v-if="timeFacts(f.name).conflict" role="status">来源时间事实与已有声明不同；原声明保留，请核对后修改。</p>
                  <p v-if="timeConfig(f.name).unsupported" class="supplement">已有时间表示暂不支持简化编辑，已保留：{{ readable(originalField(f.name).time_semantics.value) }}。选择新解释才会替换。</p>
                </td>
                <td class="field-relation"><p v-for="(relation, i) in fieldRelations(f.name)" :key="i" class="supplement">{{ relationTitle(relation) }}</p></td>
              </tr>
              <tr class="field-description-row"><td colspan="4"><textarea :aria-label="f.name + ' 字段说明'" placeholder="字段说明" :value="fieldValue(f.name, 'definition')" @input="editField(f.name, 'definition', $event.target.value)" /><p v-for="note in fieldNotes(f.name)" :key="note" class="supplement">{{ note }}</p></td></tr>
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
    <details v-if="observation"><summary>技术详情（最近读取结果）</summary><p class="supplement">以下 JSON 为最近读取结果，不包含未保存草稿。</p><button @click="copy">复制 JSON</button><pre data-testid="description-json">{{ json(observation) }}</pre></details>
  </section>
</template>
<script setup>
import { computed, h, onMounted, ref, watch } from 'vue'
import { ElMessageBox } from 'element-plus'
import { ArrowDown, ArrowUp } from '@element-plus/icons-vue'
import ChannelRelationEditor from './ChannelRelationEditor.vue'
import { semanticsCall as call, clone, json, parseJson, labels } from '../utils/dataSemantics.js'
const props = defineProps({ locator: { type: Object, required: true } })
const observation = ref(null), basis = ref(null), changes = ref({}), legacy = ref(null), busy = ref(false), error = ref(''), notice = ref(''), filter = ref(''), confirmations = ref([]), relationField = ref(''), relationTargets = ref({})
const fieldsCollapsed = ref(false)
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
const entityNotes = computed(() => notes(observation.value?.description.document?.entity, ['limitations', 'row_semantics']))
const fieldNotes = name => notes(originalField(name), ['null_meaning', 'validity', 'value_domain', 'aggregation'])
const typeSummary = type => type ? type.name + (Object.keys(type.parameters || {}).length ? ' ' + readable(type.parameters) : '') : '类型未知'
const relations = computed(() => {
  if (own(changes.value, 'relations')) return changes.value.relations
  const saved = declared(observation.value?.description.document?.entity?.relations)
  const map = observation.value?.context.binding?.field_map || {}
  return Array.isArray(saved) ? saved.map(relation => ({ dictionary_ref: relation.dictionary_ref, field_map: Object.fromEntries(Object.entries(relation.field_map).map(([id, target]) => [map[id] || observation.value.description.document.fields.find(f => f.field_id === id)?.name || id, target])) })) : []
})
const fieldRelations = name => relations.value.filter(r => own(r.field_map, name))
const relationTitle = relation => {
  const target = relationTargets.value[relation.dictionary_ref]
  return (target?.document?.title || '历史关联') + ' · ' + Object.entries(relation.field_map).map(([name, id]) => `${name} → ${target?.document?.fields.find(f => f.field_id === id)?.name || '目标字段'}`).join('；') + (target?.error ? ' · 目标暂不可读，原关系已保留' : '')
}
function rememberTarget(exact, document) { relationTargets.value[exact] = { document } }
async function readRelationTargets() {
  const refs = [...new Set(relations.value.map(r => r.dictionary_ref))]
  await Promise.allSettled(refs.map(async exact => {
    if (relationTargets.value[exact]?.document) return
    try { const result = await call('resolve', { exact_reference: exact }); rememberTarget(exact, result.document) }
    catch (e) { relationTargets.value[exact] = { error: e.message } }
  }))
}
const timeLabels = { unix_timestamp: 'unix时间戳', datetime: '日期时间', duration: '时长' }
const timeFacts = name => observation.value?.context.editor_fields?.[name] || {}
const timeConfig = name => {
  const edits = changes.value.fields?.[name]
  if (own(edits, 'time')) return edits.time || {}
  const saved = originalField(name)?.time_semantics
  const v = declared(saved) || (saved?.state !== 'not_applicable' ? declared(timeFacts(name).time_semantics) : null)
  if (!v) return {}
  const unit = declared(v.unit)?.code, timezone = declared(v.timezone) || ''
  if (v.kind === 'duration' && ['s','ms','us','ns'].includes(unit)) return { mode: 'duration', unit }
  if (v.kind === 'instant' && timeFacts(name).modes?.includes('datetime')) return { mode: 'datetime', ...(unit ? { unit } : {}), ...(timezone ? { timezone } : {}) }
  if (v.kind === 'instant' && declared(v.epoch) === '1970-01-01T00:00:00Z' && ['s','ms','us','ns'].includes(unit)) return { mode: 'unix_timestamp', unit }
  return { unsupported: true }
}
const timeSelection = name => timeConfig(name).unsupported ? '__preserved__' : timeConfig(name).mode || ''
const timeOptions = name => {
  const current = timeConfig(name), options = [{ value: '', label: '非时间' }]
  for (const mode of ['unix_timestamp', 'datetime', 'duration'])
    if (timeFacts(name).modes?.includes(mode) || current.mode === mode) options.push({ value: mode, label: timeLabels[mode] })
  if (current.unsupported) options.push({ value: '__preserved__', label: '已有时间表示（保留）', disabled: true })
  return options
}
function changeTimeMode(name, mode) {
  if (mode === '__preserved__') return
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
function persist() { if (basis.value) sessionStorage.setItem(key(), json({ basis: basis.value, editor_changes: changes.value, ...(legacy.value ? { changes: legacy.value } : {}) })) }
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
  busy.value = true; error.value = ''; confirmations.value = []
  try {
    observation.value = await call('describe', { target: { kind: 'channel', locator: props.locator } })
    if (props.locator.kind === 'dataframe' && observation.value.structure.state === 'pending') {
      await load()
      observation.value = await call('describe', { target: { kind: 'channel', locator: props.locator } })
    }
    if (!basis.value || (!Object.keys(changes.value).length && !legacy.value)) basis.value = clone(observation.value.basis)
    if (changedBasis.value) notice.value = '当前来源或说明版本已变化；原稿保留，请核对差异后继续。'
    await readRelationTargets()
    await inspectSource()
  } catch (e) { error.value = e.message } finally { busy.value = false }
}
async function load() {
  const r = await fetch('/api/channels/dataframe/preview', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json({ name: props.locator.name, page: 1, page_size: 1 }), signal: AbortSignal.timeout(30000) })
  if (!r.ok) throw Error('加载 DataFrame 结构失败，请点击“刷新”重试。')
}
function rebase() {
  if (!window.confirm('已核对当前结构和说明？保留原稿中的语义修改，按当前版本重新提交；缺失字段不会自动迁移。')) return
  basis.value = clone(observation.value.basis); notice.value = '已使用当前版本作为依据；请检查保留的原稿。'; persist()
}
async function save() {
  busy.value = true; error.value = ''; notice.value = ''
  try {
    const r = await call('channels/save-description', { locator: props.locator, basis: basis.value, ...(legacy.value ? { changes: legacy.value } : { editor_changes: changes.value }), confirmations: confirmations.value })
    if (r.status !== 'applied') { notice.value = `定义已保存：${r.dictionary_ref}；尚未应用：${r.error?.code} ${r.error?.message}。原稿已保留，可重试。`; persist(); return }
    notice.value = '说明已保存'; changes.value = {}; legacy.value = null; basis.value = r.basis; sessionStorage.removeItem(key())
    await refresh()
  } catch (e) {
    persist()
    if (/^(SOURCE_CHANGED|CONFIRMATION_REQUIRED)[：:]/.test(e.message)) await refresh()
    error.value = e.message
  } finally { busy.value = false }
}
async function copy() { try { await navigator.clipboard.writeText(json(observation.value)); notice.value = 'JSON 已复制' } catch { notice.value = '可在技术详情中选择并复制 JSON' } }
onMounted(() => { try { const d = parseJson(sessionStorage.getItem(key()) || 'null'); if (d) { basis.value = d.basis; changes.value = d.editor_changes || {}; legacy.value = d.changes && Object.keys(d.changes).length ? d.changes : null } } catch {} refresh() })
</script>
<style scoped>
.channel-description { color: var(--text-primary); min-width: 0; max-width: 100%; }
.heading { display: flex; align-items: center; justify-content: space-between; gap: 8px; } h3 { margin: 0; }
.heading-actions { display: flex; align-items: center; gap: 8px; flex-shrink: 0; } .heading-actions button { margin: 0; }
.subject, pre { overflow-wrap: anywhere; } .subject { margin: 4px 0 12px; }
label { display: block; margin: 6px 0; } .object-notes { display: grid; grid-template-columns: 2fr 1fr; gap: 12px; }
input:not([type=checkbox]), textarea, select { box-sizing: border-box; max-width: 100%; width: 100%; color: var(--text-primary); background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 4px; padding: 6px; }
select option { color: var(--text-primary); background: var(--bg-card); }
input::placeholder { color: var(--text-secondary); opacity: 1; } textarea { min-height: 52px; resize: vertical; } fieldset { border: 0; padding: 0; min-width: 0; }
.field-toolbar { display: flex; align-items: center; gap: 12px; margin: 10px 0; color: var(--text-secondary); font-size: 12px; } .field-toolbar input { width: 240px; }
.fields-scroll { max-width: 100%; overflow: auto; } .fields { width: 100%; table-layout: fixed; border-collapse: collapse; }
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
</style>
