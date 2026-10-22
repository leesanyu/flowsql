import test from 'node:test'
import assert from 'node:assert/strict'
import { controls, editChart } from '../../exploration/src/chart.js'
const fields = [{ fid: 'a', analyticType: 'dimension' }, { fid: 'b', analyticType: 'measure' }, { fid: 'c', analyticType: 'measure' }]
const chart = { config: { geoms: ['bar'], defaultAggregated: true }, layout: { primaryColor: '#123456' },
  encodings: { columns: [fields[0]], rows: [{ ...fields[1], aggName: 'sum' }], dimensions: [fields[0]], measures: fields.slice(1),
    color: [fields[0]], filters: [{ ...fields[1], rule: { type: 'range', value: [10, 20] } }] } }
test('quick edit preserves native filters, colors and aggregation while switching chart types', () => {
  const next = editChart(chart, { mark: 'line' }, fields)
  assert.equal(controls(next).mark, 'line'); assert.equal(controls(next).agg, 'sum')
  assert.deepEqual(next.encodings.filters, chart.encodings.filters)
  assert.deepEqual(next.encodings.color, chart.encodings.color)
  assert.deepEqual(next.layout, chart.layout)
  assert.equal(controls(chart).mark, 'bar')
})
test('single axis or aggregate edit does not replace other native encodings', () => {
  const next = editChart(editChart(chart, { y: 'c' }, fields), { agg: 'max' }, fields)
  assert.equal(controls(next).y, 'c'); assert.equal(controls(next).agg, 'max')
  assert.deepEqual(next.encodings.columns, chart.encodings.columns)
  assert.deepEqual(next.encodings.filters, chart.encodings.filters)
})
test('simple filter preserves advanced rules and removing it restores advanced rules', () => {
  const next = editChart(chart, { filter: 'a', filterValue: 'alpha' }, fields)
  assert.equal(controls(next).filterValue, 'alpha')
  assert.deepEqual(editChart(next, { filter: '' }, fields).encodings.filters, chart.encodings.filters)
  assert.throws(() => editChart(chart, { x: 'missing' }, fields), /字段结构/)
})

test('native nonaggregated state is represented by the simple aggregate control', () => {
  const raw = editChart(chart, { agg: 'none' }, fields)
  assert.equal(controls(raw).agg, 'none')
  assert.equal(controls(editChart(raw, { agg: 'max' }, fields)).agg, 'max')
})

import { chartKinds, chartKind, chartProblem, countField, histogramWorkflow, histogramRows } from '../../exploration/src/chart.js'
import { statisticsWorkflow } from '../../exploration/src/chart.js'
test('distinct count accepts a text measure and restores a numeric field when changing to sum', () => {
  const next = editChart(editChart(chart, { agg: 'distinctCount' }, fields), { y: 'a' }, fields)
  assert.equal(controls(next).agg, 'distinctCount')
  assert.equal(next.encodings.rows[0].analyticType, 'measure')
  assert.equal(next.encodings.rows[0].semanticType, 'quantitative')
  assert.equal(chartProblem(next, fields), '')
  assert.equal(controls(editChart(next, { agg: 'sum' }, fields)).y, 'b')
  const scatter = editChart(next, { mark: 'point' }, fields)
  assert.equal(chartProblem(scatter, fields), '')
})
test('source calendar bucket retains filters, aliases, sort and Top-N without mutating the workflow', () => {
  const next = editChart(chart, { timeBucket: 'month' }, fields)
  const workflow = { workflow: [{ type: 'filter', filters: [] }, { type: 'view', query: [{ op: 'aggregate', groupBy: ['a'], measures: [] }] }, { type: 'sort', by: ['a'], sort: 'ascending' }], limit: 2 }
  const result = statisticsWorkflow(next, workflow)
  assert.deepEqual(result.workflow[1].query[0].groupBy, [{ field: 'a', unit: 'month', asFieldKey: 'a' }])
  assert.deepEqual(result.workflow[0], workflow.workflow[0]); assert.equal(result.limit, 2)
  assert.deepEqual(workflow.workflow[1].query[0].groupBy, ['a'])
  assert.equal(controls(next).timeBucket, 'month')
  assert.equal(editChart(next, { agg: 'none' }, fields).config.flowsqlTimeBucket, undefined)
  assert.equal(editChart(next, { mark: 'point' }, fields).config.flowsqlTimeBucket, undefined)
  assert.equal(editChart(next, { x: 'b' }, fields).config.flowsqlTimeBucket, undefined)
  assert.equal(editChart(next, { mark: 'line' }, fields).config.flowsqlTimeBucket.unit, 'month')
  assert.throws(() => statisticsWorkflow(next, {workflow:[{type:'view',query:[{op:'raw',fields:['a']}]}]}), /时间桶/)
})
const allFields = [...fields, { fid: 'group', analyticType: 'dimension' }, countField]
test('all 13 chart kinds construct a valid role-based chart without losing filters/layout', () => {
  assert.equal(Object.keys(chartKinds).length, 13)
  for (const kind of Object.keys(chartKinds)) {
    const next = editChart(chart, { mark: kind }, allFields)
    assert.equal(chartKind(next), kind)
    assert.equal(chartProblem(next, allFields), '', kind)
    assert.deepEqual(next.encodings.filters, chart.encodings.filters)
    assert.deepEqual(next.layout, chart.layout)
    assert.equal(chartKind(editChart(next, { mark: 'line' }, allFields)), 'line')
  }
})
test('histogram needs exactly one numeric input, generates frequency and preserves bin settings', () => {
  const next = editChart(editChart(chart, { mark: 'histogram' }, allFields), { x: 'c', bins: 7 }, allFields)
  assert.equal(controls(next).x, 'c')
  assert.deepEqual(next.config.flowsqlHistogram, { field: 'c', bins: 7 })
  assert.equal(chartKinds.histogram.roles.length, 1)
  assert.equal(next.encodings.rows[0].name, '频数')
  for (const bins of [0, 101, 1.1, NaN]) assert.match(chartProblem(editChart(next, { bins }, allFields), allFields), /箱数/)
  const regular = editChart(next, { mark: 'pie' }, allFields)
  assert.equal(regular.config.flowsqlHistogram, undefined)
  assert.equal(regular.encodings.columns.length, 0); assert.equal(regular.encodings.rows.length, 0)
  assert.equal(regular.encodings.theta[0].analyticType, 'measure')
})
test('missing fields report the required role instead of silently querying a different field', () => {
  const next = editChart(chart, { mark: 'histogram' }, [fields[0]])
  assert.equal(chartProblem(next, [fields[0]]), '请选择数值字段')
})
test('histogram bridge preserves source filters and uses database frequencies', () => {
  const next = editChart(chart, { mark: 'histogram' }, allFields)
  const workflow = { workflow: [{ type: 'filter', filters: [{ fid: 'a' }] }, { type: 'view', query: [{ op: 'aggregate', measures: [{ asFieldKey: 'frequency' }] }] }] }
  const native = histogramWorkflow(next, workflow)
  assert.deepEqual(native.workflow[0], workflow.workflow[0])
  assert.deepEqual(native.workflow[1].query, [{ op: 'histogram', field: 'b', bins: 10 }])
  assert.equal(histogramRows(next, workflow, [{ bin_index: 0, bin_lower: 1, bin_upper: 2, bin_count: 99 }])[0].frequency, 99)
  assert.equal(histogramWorkflow(chart, workflow), workflow)
})

test('scatter defaults use distinct measures and grouped charts use distinct categories', () => {
  const scatter = editChart(chart, { mark: 'point' }, allFields)
  assert.notEqual(controls(scatter).x, controls(scatter).y)
  const grouped = editChart(chart, { mark: 'grouped_bar' }, allFields)
  assert.notEqual(controls(grouped).x, controls(grouped).series)
  assert.match(chartProblem(editChart(scatter, { x: controls(scatter).y }, allFields), allFields), /不同/)
})

test('editing one field preserves additional native facets and measures', () => {
  const native = structuredClone(chart)
  native.encodings.columns.push(allFields[3])
  native.encodings.rows.push({ ...fields[2], aggName: 'max' })
  const edited = editChart(native, { y: 'b' }, allFields)
  assert.deepEqual(edited.encodings.columns, native.encodings.columns)
  assert.deepEqual(edited.encodings.rows[1], native.encodings.rows[1])
})
