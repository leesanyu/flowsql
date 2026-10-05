// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import test from 'node:test'
import { normalizeStreamOptions } from './streamChannel.js'

const schema = [
  { key: 'backend', type: 'enum', required: true, enum_values: ['af_packet', 'pfring_classic', 'af_xdp_copy_skb'] },
  { key: 'interfaces', type: 'array', required: true },
  { key: 'promiscuous', type: 'bool' },
  { key: 'snaplen', type: 'int', has_range: true, min_value: 1, max_value: 65535 },
  { key: 'buffer_mib', type: 'int', has_range: true, min_value: 1, max_value: 4294967295 }
]
const options = { backend: 'af_packet', promiscuous: true, snaplen: 65535, buffer_mib: 64 }

test('NetAdapter saves five writable fields and excludes queried runtime diagnostics', () => {
  assert.deepEqual(normalizeStreamOptions(schema, {
    ...options, observation_domain_id: 123, budget_bytes: 67108864, readers: [{ task_id: 'running' }]
  }, { interfaces: ' eth1,eth2 ' }), { ...options, interfaces: ['eth1', 'eth2'] })
  for (const backend of schema[0].enum_values) {
    assert.equal(normalizeStreamOptions(schema, { ...options, backend }, { interfaces: 'eth1' }).backend, backend)
  }
})

test('required capture input and unsupported backend fail before submission', () => {
  assert.throws(() => normalizeStreamOptions(schema, options, { interfaces: ' , ' }), /interfaces/)
  assert.throws(() => normalizeStreamOptions(schema, { ...options, backend: 'zero_copy' },
    { interfaces: 'eth1' }), /backend/)
})

test('capture numeric bounds reject fractions, zero and out-of-range values', () => {
  for (const snaplen of [0, 65536, 1.5, NaN]) {
    assert.throws(() => normalizeStreamOptions(schema, { ...options, snaplen }, { interfaces: 'eth1' }), /snaplen/)
  }
  for (const buffer_mib of [0, -1, 1.5, 4294967296]) {
    assert.throws(() => normalizeStreamOptions(schema, { ...options, buffer_mib }, { interfaces: 'eth1' }), /buffer_mib/)
  }
  assert.equal(normalizeStreamOptions(schema, { ...options, snaplen: 1, promiscuous: false },
    { interfaces: 'eth1' }).promiscuous, false)
})


test('Channels page loads capture defaults, preserves failed drafts and saves only writable fields', async () => {
  const { readFileSync } = await import('node:fs')
  const vue = await import('vue')
  const { parse, compileScript } = await import('@vue/compiler-sfc')
  const { transform } = await import('esbuild')
  const utils = await import('./streamChannel.js')
  const calls = [], messages = []
  let failSave = true
  const definition = { channel_type: 'netadapter', allowed_roles: ['source'], option_schema: schema.map(field => ({
    ...field, default_value: { backend: 'af_packet', interfaces: '[]', promiscuous: 'true',
      snaplen: '65535', buffer_mib: '64' }[field.key]
  })) }
  const mockApi = {
    listDbChannels: async () => ({ data: [] }),
    listDfChannels: async () => ({ data: { channels: [] } }),
    listStreamChannels: async () => ({ data: { channels: [] } }),
    getStreamDefinitions: async () => ({ data: { definitions: [definition,
      { channel_type: 'ring', allowed_roles: ['source', 'sink', 'both'], option_schema: [] }] } }),
    addStreamChannel: async payload => { calls.push(['add', payload]); if (failSave) throw new Error('backend unavailable') },
    modifyStreamChannel: async payload => { calls.push(['modify', payload]) },
    removeStreamChannel: async (...args) => { calls.push(['remove', ...args]) },
    PCAP_UPLOAD_DEFAULTS: {}, PCAP_REPLAY_SPEEDS: []
  }
  const { descriptor } = parse(readFileSync(new URL('../views/Channels.vue', import.meta.url), 'utf8'))
  const script = compileScript(descriptor, { id: 'capture-form-test' })
  const output = await transform(script.content, { format: 'cjs', loader: 'js' })
  const module = { exports: {} }
  const require = name => {
    if (name === 'vue') return vue
    if (name === '../api') return mockApi
    if (name === '../utils/streamChannel.js') return utils
    if (name === './ConfigChannels.vue') return {}
    if (name === '@element-plus/icons-vue') return {}
    if (name === 'element-plus') return { ElMessage: Object.fromEntries(
      ['error', 'warning', 'success'].map(key => [key, text => messages.push([key, text])])),
      ElMessageBox: { confirm: async () => {} } }
    throw new Error(`unexpected Channels dependency: ${name}`)
  }
  new Function('require', 'module', 'exports', output.code)(require, module, module.exports)
  let page
  const renderer = vue.createRenderer({
    createElement: () => ({}), createText: () => ({}), createComment: () => ({}),
    insert() {}, remove() {}, setText() {}, setElementText() {}, patchProp() {},
    parentNode: () => null, nextSibling: () => null
  })
  const app = renderer.createApp({ setup() {
    page = module.exports.default.setup({}, { expose() {} })
    return () => vue.h('div')
  } })
  app.mount({})
  try {
    await new Promise(resolve => setImmediate(resolve))
    await page.openAddStreamDialog()
    assert.equal(page.streamForm.value.type, 'ring')
    assert.deepEqual(page.creatableStreamDefinitions.value.map(item => item.channel_type), ['ring'])
    assert.equal(page.streamDialogTitle.value, '新增 Stream 通道')
    await page.openAddNetAdapterDialog()
    assert.equal(page.streamForm.value.type, 'netadapter')
    assert.equal(page.streamDialogTitle.value, '新增网卡采集通道')
    assert.equal(page.streamForm.value.role, 'source')
    assert.deepEqual(page.streamForm.value.options, { ...options, interfaces: [] })
    page.streamForm.value.name = 'edge'
    page.streamArrayInputs.value.interfaces = 'eth1,eth2'
    await page.submitStreamForm()
    assert.equal(page.showStreamDialog.value, true)
    assert.equal(page.streamArrayInputs.value.interfaces, 'eth1,eth2')
    assert.match(messages.at(-1)[1], /backend unavailable/)
    failSave = false
    await page.submitStreamForm()
    assert.equal(page.showStreamDialog.value, false)
    const row = { type: 'netadapter', name: 'edge', role: 'source', in_use: false,
      option_json: { ...options, interfaces: ['eth1'], observation_domain_id: 123, readers: [] } }
    await page.openEditStreamDialog(row)
    assert.equal(page.streamDialogTitle.value, '编辑网卡采集通道')
    page.streamForm.value.options.snaplen = 128
    await page.submitStreamForm()
    assert.deepEqual(calls.at(-1), ['modify', { type: 'netadapter', name: 'edge', role: 'source',
      options: { ...options, interfaces: ['eth1'], snaplen: 128 } }])
    page.viewStreamConfig(row)
    assert.equal(page.streamConfigFields.value.length, 7)
    assert.ok(!page.streamConfigFields.value.some(field => field.key === 'readers'))
    const previous = calls.length
    await page.openEditStreamDialog({ ...row, in_use: true })
    await page.removeStream({ ...row, in_use: true })
    assert.equal(calls.length, previous)
    assert.match(messages.at(-1)[1], /停止采集任务/)
    await page.removeStream(row)
    assert.deepEqual(calls.at(-1), ['remove', 'netadapter', 'edge'])
    page.showStreamDialog.value = false
    page.streamDefinitions.value = page.creatableStreamDefinitions.value
    await page.openAddNetAdapterDialog()
    assert.equal(page.showStreamDialog.value, false)
    assert.match(messages.at(-1)[1], /未提供网卡采集通道/)
  } finally { app.unmount() }
})
