import { labels, semanticKeys, unknown, prettyJson } from './dataSemantics.js'

export function channelReading(observation) {
  const structure = { known: '结构已加载', pending: '结构待加载', unsupported: '不支持结构说明' }
  return {
    reading: {
      mode: '只读', structure_status: structure[observation.structure.state],
      description_status: observation.description.state, description_origin: observation.description.origin,
      notice: observation.structure.state === 'pending' ? '可转到当前对象管理页加载后重新 DESC。' :
        observation.description.state === 'needs_review' ? '结构或绑定已变化，说明待核对。' : '本次读取只描述元数据，不验证业务数据质量。'
    },
    entity_semantics: Object.fromEntries(semanticKeys.entity.map(key => [labels[key], observation.description.document?.entity?.[key] || unknown()])),
    observation
  }
}

export const operatorReading = observation => ({ reading: { mode: '只读', notice: '作者声明与本地补充按本次精确依据展示；完整契约可显式读取。' }, observation })

export function resultReading(context, indexes = null, fieldLabels = {}) {
  if (!context) return { reading: { mode: '只读', notice: '未记录本次说明；需要重新绘图或采集快照取得依据。' }, semantic_context: null }
  const fields = context.fields.filter(field => indexes === null || indexes.includes(field.output_index))
  return {
    reading: {
      mode: '只读', title: '本次结果依据',
      notice: context.error ? '说明读取失败；数据与图表设置已保留。' : '静态定义不证明历史行由当前组件或配置产生。过滤不代表已满足全部有效条件，Top-N 不代表全量。',
      fields: fields.map(field => ({ output_index: field.output_index, label: fieldLabels[field.output_name] || field.output_name,
        state: { known: '已声明', unknown: '未知', conflict: '结构变化，待核对' }[field.state],
        operation: { identity: '直接字段', alias: '别名', aggregate: '聚合', time_bucket: '时间桶', histogram: '直方图', unknown: '无法推导' }[field.operation.kind],
        result_unit: field.result_unit || '未知' }))
    },
    semantic_context: { ...context, fields }
  }
}

export function jsonTokens(value) {
  const raw = prettyJson(value)
  return [...raw.matchAll(/"(?:\\.|[^"\\])*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null|[{}\[\],:]|\s+/g)].map(match => ({
    kind: match[0][0] === '"' ? (raw[match.index + match[0].length] === ':' ? 'key' : 'string') : /^\s+$/.test(match[0]) ? 'space' :
      /^[{}\[\],:]$/.test(match[0]) ? 'punctuation' : match[0] === 'null' ? 'null' : match[0] === 'true' || match[0] === 'false' ? 'boolean' : 'number',
    text: match[0]
  }))
}
