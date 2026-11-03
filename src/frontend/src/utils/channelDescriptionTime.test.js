import test from 'node:test'
import assert from 'node:assert/strict'
import { timeEditorConfig, timeDescription, timeSummary } from './channelDescriptionTime.js'

const declared = value => ({ state: 'declared', value, origin: 'imported' })
const instant = epoch => ({ kind: 'instant', unit: declared({ code: 'ns', label: '纳秒' }), epoch: declared(epoch), timezone: declared('UTC'), description: declared('采集参考时间，不代表写库时间。') })

test('新旧明确 Unix 基准均可读取，保留单位时区且不改原声明', () => {
  for (const epoch of ['1970-01-01T00:00:00Z', 'Unix epoch']) {
    const value = instant(epoch), before = structuredClone(value)
    assert.deepEqual(timeEditorConfig(value, ['unix_timestamp', 'duration']), { mode: 'unix_timestamp', unit: 'ns', timezone: 'UTC' })
    assert.deepEqual(value, before)
  }
})
test('普通字段不按名字或单位推断时间，未知/不适用保持空配置', () => {
  assert.deepEqual(timeEditorConfig(null, ['unix_timestamp', 'duration']), {})
  assert.deepEqual(timeEditorConfig(undefined), {})
  assert.deepEqual(timeEditorConfig({ unit: declared({ code: 'ns' }) }, ['unix_timestamp']), { unsupported: true })
})
test('时长与原生日期时间使用已有声明，不补造基准时区', () => {
  assert.deepEqual(timeEditorConfig({ kind: 'duration', unit: declared({ code: 'us' }) }, ['duration']), { mode: 'duration', unit: 'us' })
  assert.deepEqual(timeEditorConfig({ kind: 'instant' }, ['datetime']), { mode: 'datetime' })
  assert.deepEqual(timeEditorConfig(instant('Unix epoch'), ['datetime']), { mode: 'datetime', unit: 'ns', timezone: 'UTC' })
})
test('自定义基准或单位不伪装为非时间，摘要仅含人可读事实', () => {
  const value = instant('custom epoch'); value.unit = declared({ code: 'minute', label: '分钟' })
  assert.deepEqual(timeEditorConfig(value, ['unix_timestamp', 'duration']), { unsupported: true })
  assert.equal(timeSummary(value), '时间点；单位：minute；基准：custom epoch；时区：UTC')
  assert.deepEqual(timeEditorConfig(instant('1970-01-01'), ['unix_timestamp']), { unsupported: true })
  assert.doesNotMatch(timeSummary(value), /state|value|origin|imported/)
})
test('字段正文已包含时间含义时不重复；修改正文后仍能看到原时间含义', () => {
  const value = instant('Unix epoch')
  assert.equal(timeDescription(value, value.description.value), '')
  assert.equal(timeDescription(value, '说明前缀：' + value.description.value + '\n补充。'), '')
  assert.equal(timeDescription(value, '人工补充'), value.description.value)
  assert.equal(timeDescription(value, '采集参考时间，\n不代表写库时间。'), '')
  assert.equal(timeDescription({ description: { state: 'unknown', value: null } }, ''), '')
})
