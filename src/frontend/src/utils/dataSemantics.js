export const unknown = () => ({ state: 'unknown', value: null, origin: 'manual' })
export const semanticKeys = {
  standard: ['definition', 'scope', 'quantity', 'representation', 'unit', 'value_domain'],
  entity: ['row_meaning', 'row_semantics', 'identity_fields', 'revision_field', 'time_fields', 'relations', 'limitations'],
  field: ['definition', 'role', 'measure_kind', 'unit', 'time_semantics', 'null_meaning', 'validity', 'aggregation', 'value_domain', 'standard_refs']
}
const semantics = (keys) => Object.fromEntries(keys.map(k => [k, unknown()]))
export function blankDocument(kind) {
  const schema_id = `flowsql.data-${kind}.v1`
  if (kind === 'standard') return { schema_id, title: '', ...semantics(semanticKeys.standard) }
  if (kind === 'dictionary') return { schema_id, title: '', description: unknown(), entity: semantics(semanticKeys.entity), fields: [] }
  return { schema_id, object_key: { provider_id: '', object_id: '' }, object_label: '', dictionary_ref: '', field_map: {}, declared_schema: null }
}
export const blankField = () => ({ field_id: '', name: '', title: '', type: { system: 'opaque', name: '', parameters: {} }, nullable: true, ...semantics(semanticKeys.field) })
// Validate syntax before rewriting integer tokens; quoted strings are left intact.
export function parseJson(raw) {
  JSON.parse(raw)
  let prefix = '__flowsql_integer__'
  while (raw.includes(prefix)) prefix += '_'
  const safe = raw.replace(/"(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g, token => {
    if (token[0] === '"' || /[.eE]/.test(token)) return token
    return Number.isSafeInteger(Number(token)) ? token : JSON.stringify(prefix + token)
  })
  return JSON.parse(safe, (_, v) => typeof v === 'string' && v.startsWith(prefix) ? BigInt(v.slice(prefix.length)) : v)
}
export function json(value) {
  if (typeof value === 'bigint') return String(value)
  if (typeof value === 'number' && !Number.isFinite(value)) throw Error('不接受非有限数值')
  if (Array.isArray(value)) return '[' + value.map(json).join(',') + ']'
  if (value && typeof value === 'object') return '{' + Object.entries(value).map(([k, v]) => JSON.stringify(k) + ':' + json(v)).join(',') + '}'
  return JSON.stringify(value)
}
export const clone = v => parseJson(json(v))
const structure = d => (d.fields || []).map(({ field_id, name, type, nullable }) => ({ field_id, name, type, nullable }))
export function structuralDiff(before, after) {
  const a = json(structure(before)), b = json(structure(after))
  return a === b ? '' : `原结构：${a}\n新结构：${b}`
}
export async function semanticsCall(operation, payload) {
  const response = await fetch('/api/semantics/v1/' + operation, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: json(payload), signal: AbortSignal.timeout(30000)
  })
  const result = parseJson(await response.text())
  if (!response.ok) throw Error(`${result.error?.code || response.status}：${result.error?.message || '请求失败'} ${result.error?.field_path || ''}`)
  return result.data
}
export async function saveAndApply(call, command, binding) {
  const saved = await call('save', command)
  if (!binding) return { saved }
  try {
    const applied = await call('save', { kind: 'binding', name: binding.name, expected_current_revision: binding.revision,
      document: { ...binding.document, dictionary_ref: saved.exact_reference } })
    return { saved, applied }
  } catch (applyError) { return { saved, applyError } }
}
export const labels = {
  definition: '定义', scope: '适用范围', quantity: '量的概念', representation: '表示规则', unit: '单位', value_domain: '取值域',
  description: '说明', row_meaning: '一行表示什么', row_semantics: '行语义', identity_fields: '身份字段', revision_field: '修订字段',
  time_fields: '时间字段', relations: '关联关系', limitations: '使用局限', role: '字段角色', measure_kind: '度量类型',
  time_semantics: '时间语义', null_meaning: 'NULL 含义', validity: '有效条件', aggregation: '聚合规则', standard_refs: '标准版本'
}
export const choices = { row_semantics: ['event', 'snapshot', 'cumulative', 'delta', 'aggregate'], role: ['identity', 'dimension', 'time', 'measure', 'state'], measure_kind: ['gauge', 'cumulative', 'delta', 'ratio', 'statistic'] }
export const complexDefaults = {
  unit: { code: '', label: '' }, value_domain: { kind: 'enum', items: [{ code: '', label: '' }] }, identity_fields: [], time_fields: [], relations: [], limitations: [],
  standard_refs: [], time_semantics: { kind: 'instant', unit: unknown(), epoch: unknown(), timezone: unknown(), description: unknown() },
  validity: { mode: 'descriptive', description: '', field_ids: [] }, aggregation: []
}
