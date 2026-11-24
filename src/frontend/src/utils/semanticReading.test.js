import test from 'node:test'
import assert from 'node:assert/strict'
import { channelReading, resultReading, jsonTokens } from './semanticReading.js'
import { prettyJson, parseJson, semanticKeys, labels } from './dataSemantics.js'

test('pending/missing observation and every unknown entity property stay inside JSON', () => {
  const observation = { subject: { kind: 'channel' }, structure: { state: 'pending', fields: [] }, description: { state: 'missing', origin: 'none', document: null }, basis: {}, context: {} }
  const result = channelReading(observation)
  assert.equal(result.observation, observation)
  assert.equal(result.reading.structure_status, '结构待加载')
  assert.match(result.reading.notice, /当前对象管理页/)
  assert.deepEqual(Object.keys(result.entity_semantics), semanticKeys.entity.map(key => labels[key]))
  for (const value of Object.values(result.entity_semantics)) assert.deepEqual(value, { state: 'unknown', value: null, origin: 'manual' })
  assert.equal(observation.description.document, null)
})

test('selected output index preserves duplicate names, units, errors and captured sources', () => {
  const sources = [{ basis: { dictionary_ref: 'dictionary.example@2' }, description: { document: { fields: [] } } }]
  const context = { state: 'partial', observed_at: 123, scope: 'LIMIT 2', subject: { id: 'old' }, sources, error: { code: 'SOURCE_UNAVAILABLE', message: 'offline' }, fields: [0, 1].map(output_index => ({ output_index, output_name: 'same', state: 'known', operation: { kind: 'aggregate', function: 'count', configuration: {} }, result_unit: '个' })) }
  const result = resultReading(context, [1])
  assert.equal(result.semantic_context.sources, sources)
  assert.equal(result.semantic_context.fields.length, 1)
  assert.equal(result.semantic_context.fields[0].output_index, 1)
  assert.equal(result.semantic_context.fields[0].result_unit, '个')
  assert.equal(result.semantic_context.error, context.error)
  assert.equal(context.fields.length, 2)
  assert.match(result.reading.notice, /数据与图表设置已保留/)
  assert.equal(resultReading(null).semantic_context, null)
})

test('JSON highlighting keeps exact integers, all tokens and untrusted text', () => {
  const value = { unsafe: '<img src=x onerror="window.__unsafe=true">', integer: 18446744073709551615n, decimal: -1.25e-7, yes: true, no: false, unknown: null, text: 'quote " and newline\n' }
  const tokens = jsonTokens(value)
  assert.equal(tokens.map(t => t.text).join(''), prettyJson(value))
  assert.deepEqual(parseJson(tokens.map(t => t.text).join('')), value)
  for (const kind of ['key', 'string', 'number', 'boolean', 'null', 'punctuation']) assert.ok(tokens.some(t => t.kind === kind), kind)
  assert.ok(tokens.some(t => t.kind === 'string' && t.text.includes('<img')))
  assert.ok(tokens.some(t => t.kind === 'number' && t.text === '18446744073709551615'))
})
