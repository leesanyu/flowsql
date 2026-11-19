import assert from 'node:assert/strict'
import test from 'node:test'
import { reactive } from 'vue'
import { operatorDescriptionNotes } from './operatorDescriptionNotes.js'
import { parseJson, prettyJson, json } from './dataSemantics.js'

function observed() {
  return {
    basis: { description_ref: null },
    usage: {
      identity: { purposes: [{ origin: 'author', text: '按配置评估时间序列' }] },
      inputs: [{ port_id: 'datasets', description: '由配置选择数据集、时间列和指标' }],
      parameters: { description: 'config 与 parameters 二选一' },
      outputs: [{ port_id: 'results', description: '评估与预测结果' }, { port_id: 'models', description: '仅配置模型目标时产生' }],
      limitations: [{ origin: 'author', text: '预测行的观测值为空' }],
      examples: [{ origin: 'author', text: "SELECT * FROM source USING operator WITH config='config.demo@1'" }],
      annotations: { inputs: [], parameters: [], outputs: [] }
    }
  }
}

test('author descriptions remain separate from empty local notes', () => {
  const result = operatorDescriptionNotes(observed())
  assert.equal(result.purpose, '')
  assert.equal(result.input_description, '')
  assert.equal(result.parameter_description, '')
  assert.equal(result.output_description, '')
  assert.equal(result.usage_notes, '')
  assert.deepEqual(result.examples, [])
  for (const section of ['inputs', 'parameters', 'outputs']) assert.deepEqual(result[section], [])
})

test('saved edits and explicit clearing override author descriptions', () => {
  const data = observed()
  data.basis.description_ref = 'config.notes@2'
  data.usage.identity.purposes.push({ origin: 'local', text: '维护后的用途' })
  Object.assign(data.usage.annotations, { input_description: '', parameter_description: '参数配置说明', output_description: '' })
  const result = operatorDescriptionNotes(data)
  assert.equal(result.purpose, '维护后的用途')
  assert.equal(result.input_description, '')
  assert.equal(result.parameter_description, '参数配置说明')
  assert.equal(result.output_description, '')
  assert.equal(result.usage_notes, '')
  assert.deepEqual(result.examples, [])
})

test('previously saved empty text stays empty while existing row edits survive', () => {
  const data = observed()
  data.basis.description_ref = 'config.notes@1'
  data.usage.annotations.inputs = [{ name: 'bucket', description: '手工维护的时间列', type: 'int64', requirement: 'optional' }]
  const result = operatorDescriptionNotes(data)
  assert.equal(result.purpose, '')
  assert.equal(result.usage_notes, '')
  assert.deepEqual(result.examples, [])
  assert.deepEqual(result.inputs, data.usage.annotations.inputs)
  data.usage.identity.purposes.push({ origin: 'local', text: '原有用途' })
  assert.equal(operatorDescriptionNotes(data).purpose, '原有用途')
})

test('editing copies preserves saved rows and full author rules', () => {
  const data = reactive(observed())
  data.usage.annotations.inputs.push({ name: 'value', description: 'saved', requirement: 'optional', type: 'double' })
  const result = operatorDescriptionNotes(data)
  result.inputs[0].description = 'draft'
  result.examples[0] = 'draft example'
  assert.equal(data.usage.annotations.inputs[0].description, 'saved')
  assert.ok(data.usage.examples[0].text.startsWith('SELECT'))
  assert.equal(data.usage.outputs[1].description, '仅配置模型目标时产生')
})

test('undeclared descriptions stay empty without name-based defaults', () => {
  const data = observed()
  data.usage.identity.purposes = []
  data.usage.inputs = []
  data.usage.parameters = {}
  data.usage.outputs = []
  data.usage.limitations = []
  data.usage.examples = []
  const result = operatorDescriptionNotes(data)
  for (const key of ['purpose', 'input_description', 'parameter_description', 'output_description', 'usage_notes']) assert.equal(result[key], '')
  assert.deepEqual(result.examples, [])
})

test('pretty rules and copies preserve signed and unsigned 64 bit integers', () => {
  const raw = '{"min":-9223372036854775808,"max":18446744073709551615,"quoted":"18446744073709551615","nested":[{"s":"a\\\"b"}]}'
  const document = parseJson(raw)
  assert.equal(document.min, -9223372036854775808n)
  assert.equal(document.max, 18446744073709551615n)
  assert.deepEqual(parseJson(prettyJson(document)), document)
  assert.deepEqual(parseJson(json(document)), document)
})
