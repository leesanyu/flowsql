const role = (key, label, kind, optional = false) => ({ key, label, kind, optional })
const category = role('x', '类别 / 时间', 'any')
const measure = role('y', '数值', 'measure')
const series = role('series', '系列', 'dimension')
export const chartKinds = {
  bar: { label: '柱状图', roles: [category, measure] },
  stacked_bar: { label: '堆叠柱状图', roles: [category, measure, series] },
  grouped_bar: { label: '分组柱状图', roles: [category, measure, series] },
  line: { label: '折线图', roles: [category, measure] },
  area: { label: '面积图', roles: [category, measure] },
  point: { label: '散点图', roles: [role('x', '横轴数值', 'measure'), role('y', '纵轴数值', 'measure')] },
  circle: { label: '圆点图', roles: [category, measure] },
  pie: { label: '饼图', roles: [role('x', '分类', 'dimension'), measure] },
  heatmap: { label: '热力图', roles: [role('x', '列分类', 'dimension'), role('group', '行分类', 'dimension'), role('y', '颜色值 / 行数', 'measure')] },
  table: { label: '表格', roles: [role('x', '分类', 'any', true), measure] },
  highlight_table: { label: '突出显示表格', roles: [role('x', '分类', 'dimension'), measure] },
  boxplot: { label: '箱线图', roles: [role('y', '数值', 'measure'), role('x', '分组', 'dimension', true)] },
  histogram: { label: '直方图', roles: [role('x', '数值字段', 'measure')] }
}
for (const [id, kind] of Object.entries(chartKinds)) kind.id = id
export const marks = Object.fromEntries(Object.entries(chartKinds).map(([id, kind]) => [id, kind.label]))
export const aggregates = ['count', 'distinctCount', 'sum', 'mean', 'min', 'max']
const counted = (aggregate) => ['count', 'distinctCount'].includes(aggregate)
export const countField = { fid: 'gw_count_fid', name: '行数', semanticType: 'quantitative', analyticType: 'measure', aggName: 'count' }
export const histogramBin = 'gw_histogram_bin'
export function chartKind(chart) {
  const e = chart.encodings, geom = chart.config.geoms[0]
  if (chart.config.flowsqlHistogram && e.columns[0]?.fid === histogramBin) return 'histogram'
  if (geom === 'arc') return 'pie'
  if (geom === 'rect') return 'heatmap'
  if (geom === 'text') return 'highlight_table'
  if (geom === 'bar' && chart.config.flowsqlBarKind) return chart.config.flowsqlBarKind
  if (geom === 'bar' && chart.config.stack && e.color?.[0]?.analyticType === 'dimension') return chart.config.stack === 'none' ? 'grouped_bar' : 'stacked_bar'
  return geom
}
export function controls(chart) {
  const e = chart.encodings, kind = chartKind(chart), filter = e.filters[0]
  const numeric = kind === 'pie' ? e.theta?.[0] : kind === 'heatmap' || kind === 'highlight_table' ? e.color?.[0] : e.rows[0]
  return { x: kind === 'histogram' ? chart.config.flowsqlHistogram.field : kind === 'pie' ? e.color?.[0]?.fid || '' : kind === 'highlight_table' ? e.rows[0]?.fid || '' : e.columns[0]?.fid || '',
    y: numeric?.fid || '', series: e.color?.[0]?.fid || '', group: e.rows[0]?.fid || '', mark: kind,
    bins: chart.config.flowsqlHistogram?.bins ?? 10,
    timeBucket: chart.config.flowsqlTimeBucket?.unit || 'none',
    agg: chart.config.defaultAggregated ? numeric?.aggName || 'mean' : 'none',
    filter: filter?.rule?.type === 'one of' ? filter.fid : '', filterValue: filter?.rule?.type === 'one of' ? filter.rule.value[0] ?? '' : '' }
}
export function roleFields(fields, role, kind, aggregate) {
  return fields.filter((f) => (role.kind === 'any' || f.analyticType === role.kind || role.key === 'y' && counted(aggregate)) &&
    !(f.fid === countField.fid && (role.key !== 'y' || ['point', 'boxplot', 'circle'].includes(kind))))
}
export function chartProblem(chart, fields) {
  const kind = chartKind(chart), definition = chartKinds[kind], values = controls(chart)
  if (!definition) return '此图表类型尚不支持'
  for (const role of definition.roles) {
    if (!values[role.key] && role.optional) continue
    if (!roleFields(fields, role, kind, values.agg).some((f) => f.fid === values[role.key])) return `请选择${role.label}`
  }
  if (['point', 'circle'].includes(kind) && values.x === values.y) return '请选择不同的两个字段'
  if ((['stacked_bar', 'grouped_bar'].includes(kind) && values.x === values.series) || (kind === 'heatmap' && values.x === values.group)) return '请选择不同的分类字段'
  if (kind === 'histogram' && (!Number.isInteger(values.bins) || values.bins < 1 || values.bins > 100)) return '箱数须为 1～100 的整数'
  return ''
}

// One chart snapshot owns type, field roles, filters and layout in both editing modes.
export function editChart(chart, patch, fields) {
  const next = structuredClone(chart), e = next.encodings, current = controls(chart)
  const field = (fid) => {
    if (!fid) return null
    const found = [...fields, countField, ...(e.dimensions || []), ...(e.measures || [])].find((f) => f.fid === fid)
    if (!found) throw new Error('字段结构已变化，请重新选择字段')
    return structuredClone(found)
  }
  const kind = patch.mark || current.mark
  if (!chartKinds[kind]) throw new Error('此图表类型尚不支持')
  const switching = kind !== current.mark
  const values = { ...current, ...patch }
  if (switching) {
    if (['point', 'circle', 'boxplot'].includes(kind)) values.agg = 'none'
    for (const role of chartKinds[kind].roles) {
      const options = roleFields(fields, role, kind, values.agg).filter((f) => !(['point', 'circle'].includes(kind) &&
        (role.key === 'x' && f.fid === values.y || role.key === 'y' && f.fid === values.x)))
      if (!options.some((f) => f.fid === values[role.key]) || ['series', 'group'].includes(role.key) && values[role.key] === values.x)
        values[role.key] = role.optional ? '' : (options.find((f) => !['series', 'group'].includes(role.key) || f.fid !== values.x)?.fid || '')
    }
  }
  if ('agg' in patch && patch.agg !== 'none' && !counted(patch.agg) && field(values.y)?.analyticType === 'dimension')
    values.y = fields.find((f) => f.analyticType === 'measure')?.fid || ''
  const assigned = (fid, aggregate = false) => fid ? [{ ...field(fid), ...(aggregate ? {
    analyticType: 'measure', ...(counted(values.agg) ? { semanticType: 'quantitative' } : {}),
    aggName: fid === countField.fid ? 'count' : values.agg === 'none' ? 'mean' : values.agg } : {}) }] : []
  const channel = (key, value) => switching ? value : [...value, ...(e[key] || []).slice(1)]
  const rebuild = switching || ['x', 'y', 'series', 'group', 'bins', 'agg'].some((key) => key in patch)
  if (rebuild) {
    const compatible = ['bar', 'stacked_bar', 'grouped_bar', 'line', 'area', 'point', 'circle', 'boxplot'].includes(kind) && ['bar', 'stacked_bar', 'grouped_bar', 'line', 'area', 'point', 'circle', 'boxplot'].includes(current.mark)
    if (switching && !compatible) for (const key of ['columns', 'rows', 'color', 'theta', 'size', 'shape', 'opacity', 'details', 'radius', 'text']) e[key] = []
    delete next.config.flowsqlHistogram
    e.columns = channel('columns', assigned(values.x)); e.rows = channel('rows', assigned(values.y, true))
    if (kind === 'histogram') {
      next.config.flowsqlHistogram = { field: values.x, bins: Number(values.bins) }
      e.dimensions = [...(e.dimensions || []).filter((f) => f.fid !== histogramBin), { fid: histogramBin, name: '数值区间', semanticType: 'ordinal', analyticType: 'dimension' }]
      e.measures = [...(e.measures || []).filter((f) => f.fid !== countField.fid), countField]
      e.columns = [{ fid: histogramBin, name: '数值区间', semanticType: 'ordinal', analyticType: 'dimension' }]
      e.rows = [{ ...countField, name: '频数', aggName: 'sum' }]
    } else if (kind === 'pie') {
      e.color = channel('color', assigned(values.x)); e.theta = channel('theta', assigned(values.y, true)); e.columns = []; e.rows = []
    } else if (kind === 'heatmap') {
      e.rows = channel('rows', assigned(values.group)); e.color = channel('color', assigned(values.y, true))
    } else if (kind === 'highlight_table') {
      e.rows = channel('rows', assigned(values.x)); e.columns = []; e.color = channel('color', assigned(values.y, true)); e.text = channel('text', assigned(values.y, true))
    } else if (kind === 'stacked_bar' || kind === 'grouped_bar') {
      e.color = assigned(values.series)
      if (kind === 'grouped_bar') e.columns = [...assigned(values.x), ...assigned(values.series), ...(switching ? [] : (e.columns || []).slice(2))]
    }
  }
  if ('mark' in patch) {
    delete next.config.flowsqlBarKind
    if (['bar', 'stacked_bar', 'grouped_bar'].includes(kind)) next.config.flowsqlBarKind = kind
    next.config.geoms = [{ histogram: 'bar', stacked_bar: 'bar', grouped_bar: 'bar', pie: 'arc', heatmap: 'rect', highlight_table: 'text' }[kind] || kind]
    next.config.defaultAggregated = !['point', 'circle', 'boxplot'].includes(kind)
    next.config.stack = kind === 'grouped_bar' ? 'none' : 'stack'
  }
  if ('agg' in patch) {
    next.config.defaultAggregated = patch.agg !== 'none'
    for (const channel of ['rows', 'theta', 'color', 'text'])
      for (const f of e[channel] || []) if (f.analyticType === 'measure' && patch.agg !== 'none') f.aggName = f.fid === countField.fid ? 'count' : patch.agg
  }
  if ('filter' in patch || 'filterValue' in patch) {
    const fid = patch.filter ?? current.filter
    e.filters = fid ? [{ ...field(fid), rule: { type: 'one of', value: [patch.filterValue ?? current.filterValue] } },
      ...e.filters.slice(current.filter ? 1 : 0)] : e.filters.slice(current.filter ? 1 : 0)
  }
  if ('timeBucket' in patch) {
    if (patch.timeBucket === 'none') delete next.config.flowsqlTimeBucket
    else {
      next.config.flowsqlTimeBucket = { field: values.x, unit: patch.timeBucket }
      for (const key of ['columns', 'rows', 'color']) for (const f of e[key] || [])
        if (f.fid === values.x && f.analyticType === 'dimension') f.semanticType = 'nominal'
    }
  }
  if (next.config.flowsqlTimeBucket && (values.x !== next.config.flowsqlTimeBucket.field ||
    !next.config.defaultAggregated || ['histogram', 'point', 'circle', 'boxplot'].includes(kind))) delete next.config.flowsqlTimeBucket
  return next
}

export function statisticsWorkflow(chart, workflow) {
  const bucket = chart.config.flowsqlTimeBucket
  if (!bucket) return workflow
  const next = structuredClone(workflow)
  const query = next.workflow.find((s) => s.type === 'view')?.query[0]
  if (query?.op !== 'aggregate') throw new Error('时间桶需要聚合视图，请启用聚合或取消时间分桶')
  query.groupBy = query.groupBy.map((fid) => fid === bucket.field ? { ...bucket, asFieldKey: fid } : fid)
  return next
}

// Translate only the histogram view; the database computes every bin and its frequency.
export function histogramWorkflow(chart, workflow) {
  if (chartKind(chart) !== 'histogram') return workflow
  const view = workflow.workflow.find((s) => s.type === 'view')?.query[0]
  if (view?.op !== 'aggregate') throw new Error('直方图使用自动频数，请通过字段设置调整')
  const filters = workflow.workflow.filter((s) => s.type === 'filter')
  return { ...workflow, workflow: [...filters, { type: 'view', query: [{ op: 'histogram', ...chart.config.flowsqlHistogram }] }] }
}
export function histogramRows(chart, workflow, rows) {
  if (chartKind(chart) !== 'histogram') return rows
  const view = workflow.workflow.find((s) => s.type === 'view')?.query[0]
  const alias = view?.measures?.[0]?.asFieldKey || 'gw_count_fid_sum'
  return rows.map((r) => ({ [histogramBin]: `${r.bin_lower} – ${r.bin_upper}${r.bin_index === chart.config.flowsqlHistogram.bins - 1 || r.bin_lower === r.bin_upper ? '（含右端）' : ''}`, [alias]: r.bin_count }))
}
