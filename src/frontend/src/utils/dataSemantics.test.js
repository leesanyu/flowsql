import test from 'node:test'
import assert from 'node:assert/strict'
import { blankDocument, parseJson, json, structuralDiff, saveAndApply } from './dataSemantics.js'

test('新建只声明必要结构，未知语义不填默认值', () => {
  const d = blankDocument('dictionary')
  assert.equal(d.entity.row_semantics.state, 'unknown')
  assert.equal(d.entity.row_semantics.value, null)
  assert.deepEqual(d.fields, [])
})
test('HTTP/表单 JSON 保留大整数、字符串和科学计数法', () => {
  const raw = '{"n":18446744073709551615,"m":-9223372036854775808,"text":"123\\\"x","x":1.5e3,"list":[null,true]}'
  const d = parseJson(raw)
  assert.equal(d.n, 18446744073709551615n)
  assert.equal(d.m, -9223372036854775808n)
  assert.equal(d.x, 1500)
  assert.deepEqual(parseJson(json(d)), d)
  assert.throws(() => parseJson('{"n":01}'))
  assert.throws(() => json({ x: NaN }))
})
test('语义补充不改变结构，结构修改可见', () => {
  const a = { fields: [{ field_id: 'id', name: 'id', type: { system: 'opaque', name: 'text', parameters: {} }, nullable: false }] }
  const b = structuredClone(a); b.fields[0].definition = { state: 'declared', value: '身份', origin: 'manual' }
  assert.equal(structuralDiff(a, b), '')
  b.fields[0].name = 'renamed'
  assert.match(structuralDiff(a, b), /renamed/)
})
test('定义保存成功且应用冲突，保留新版本并分别报告', async () => {
  const calls = []
  const call = async (op, payload) => {
    calls.push(payload)
    if (payload.kind === 'binding') throw new Error('REVISION_CONFLICT')
    return { exact_reference: 'dictionary.orders@2', revision: 2 }
  }
  const result = await saveAndApply(call, { kind: 'dictionary' }, { name: 'orders', revision: 1, document: { dictionary_ref: 'dictionary.orders@1' } })
  assert.equal(result.saved.exact_reference, 'dictionary.orders@2')
  assert.match(result.applyError.message, /REVISION_CONFLICT/)
  assert.equal(calls[1].expected_current_revision, 1)
  assert.equal(calls[1].document.dictionary_ref, 'dictionary.orders@2')
})
test('定义保存失败不能发起应用', async () => {
  let count = 0
  await assert.rejects(saveAndApply(async () => { count++; throw Error('storage') }, {}, {}))
  assert.equal(count, 1)
})
