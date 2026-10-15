// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
import assert from 'node:assert/strict'
import test from 'node:test'
import { connectionDraft, channelDraft, buildConnection, buildChannel, blockedReason, kafkaError } from './kafkaChannel.js'
test('conditional auth payload drops hidden fields and never reuses masked passwords', () => {
  const d = connectionDraft({ name: 'auth', bootstrap_servers: ['localhost:9092'], security_protocol: 'SASL_SSL', sasl_mechanism: 'PLAIN', sasl: { username: 'alice' }, credential_bindings: { password: { configured: true } } })
  assert.deepEqual(buildConnection(d, 7), { name: 'auth', bootstrap_servers: ['localhost:9092'], security_protocol: 'SASL_SSL', tls: {}, sasl_mechanism: 'PLAIN', sasl: { username: 'alice' }, expected_revision: 7 })
  d.secrets.password = { operation: 'replace', mode: 'value', value: '********' }
  assert.throws(() => buildConnection(d), /遮罩/)
  d.security_protocol = 'PLAINTEXT'
  assert.deepEqual(buildConnection(d), { name: 'auth', bootstrap_servers: ['localhost:9092'], security_protocol: 'PLAINTEXT' })
})
test('mechanism changes, references and clear obey the active secret contract', () => {
  const d = connectionDraft({ name: 'auth', bootstrap_servers: ['localhost:9092'], security_protocol: 'SASL_SSL' })
  d.sasl_mechanism = 'OAUTHBEARER'; d.sasl.token_endpoint = 'https://idp/token'; d.sasl.client_id = 'client'
  d.secrets.client_secret = { operation: 'replace', mode: 'reference', reference: 'env:oauth' }
  const payload = buildConnection(d)
  assert.equal(payload.credential_bindings.client_secret.reference, 'env:oauth')
  assert.equal(payload.sasl.username, undefined)
  d.secrets.client_secret.operation = 'clear'
  assert.throws(() => buildConnection(d), /不能清除/)
})
test('source/sink payloads, revision and busy/referenced guards remain independent', () => {
  const d = channelDraft({ name: 'events', connection_name: 'auth', role: 'sink', topic: 'events', consumer_group: 'old' })
  assert.equal(buildChannel(d, 4).consumer_group, undefined)
  assert.equal(buildChannel(d, 4).expected_revision, 4)
  d.role = 'both'; assert.equal(buildChannel(d).consumer_group, 'old')
  d.consumer_group = ''; assert.throws(() => buildChannel(d), /消费组/)
  assert.match(blockedReason({ in_use: true }, 'channels'), /正在使用/)
  assert.match(blockedReason({ references: ['events'] }, 'connections', true), /events/)
  assert.match(kafkaError({ response: { status: 409 } }), /草稿已保留/)
})
test('structured source edit preserves explicit schema, units and offset policy without retaining hidden directions', () => {
  const source = { encoding: 'json', entity: 'metrics', fields: [{ name: 'time', path: '/time', type: 'int64', nullable: false, unit: 'unix_ms' }], offset_reset: 'error', commit_policy: 'none', max_batch_rows: 17, max_message_bytes: 2048 }
  const row = { name: 'events', connection_name: 'auth', role: 'both', topic: 'events', consumer_group: 'test', payload_kind: 'structured', payload_options: { source } }
  const draft = channelDraft(row)
  assert.deepEqual(buildChannel(draft, 4).payload_options.source, source)
  draft.source.fields[0].name = 'changed'
  assert.equal(source.fields[0].name, 'time')
  draft.source.fields.push({ ...draft.source.fields[0] })
  assert.throws(() => buildChannel(draft), /字段/)
  draft.role = 'sink'
  assert.equal(buildChannel(draft).payload_options, undefined)
  draft.role = 'both'; draft.payload_kind = 'packet'
  assert.equal(buildChannel(draft).payload_options, undefined)
})

import { readFileSync } from 'node:fs'
import * as vue from 'vue'
import { parse, compileScript } from '@vue/compiler-sfc'
import { transform } from 'esbuild'
import * as utils from './kafkaChannel.js'
const node = (tag, text = '') => ({ tag, text, props: {}, parent: null, children: [] })
const text = (n) => n.text + n.children.map(text).join('')
const find = (n, predicate) => predicate(n) ? n : n.children.map((child) => find(child, predicate)).find(Boolean)
const settle = async () => { for (let i = 0; i < 3; ++i) { await new Promise(setImmediate); await vue.nextTick() } }
async function mountPage(api) {
  const { descriptor } = parse(readFileSync(new URL('../views/KafkaChannels.vue', import.meta.url), 'utf8'))
  const output = await transform(compileScript(descriptor, { id: 'kafka-test', inlineTemplate: true }).content, { format: 'cjs' })
  const module = { exports: {} }, messages = []
  const require = (name) => name === 'vue' ? vue : name === '../api' ? api : name === '../utils/kafkaChannel.js' ? utils : name === 'element-plus'
    ? { ElMessage: Object.fromEntries(['error', 'success', 'warning'].map((k) => [k, (v) => messages.push(v)])), ElMessageBox: { confirm: async () => {} } }
    : assert.fail(name)
  new Function('require', 'module', 'exports', output.code)(require, module, module.exports)
  const renderer = vue.createRenderer({
    createElement: (tag) => node(tag), createText: (v) => node('#text', v), createComment: (v) => node('#comment', v),
    setText: (n, v) => { n.text = v }, setElementText: (n, v) => { n.text = v; n.children = [] },
    patchProp: (n, k, _old, v) => { n.props[k] = v },
    insert: (n, parent, anchor = null) => { if (n.parent) n.parent.children.splice(n.parent.children.indexOf(n), 1); n.parent = parent; const index = anchor ? parent.children.indexOf(anchor) : -1; parent.children.splice(index < 0 ? parent.children.length : index, 0, n) },
    remove: (n) => { if (n.parent) n.parent.children.splice(n.parent.children.indexOf(n), 1); n.parent = null },
    parentNode: (n) => n.parent, nextSibling: (n) => n.parent?.children[n.parent.children.indexOf(n) + 1] || null
  })
  const root = node('root'), app = renderer.createApp(module.exports.default, { searchText: '' })
  const container = vue.defineComponent({ setup (_, { attrs, slots }) { return () => vue.h('box', attrs, slots.default?.()) } })
  const formItem = vue.defineComponent({ props: ['label'], setup (props, { slots }) { vue.provide('field', props.label); return () => vue.h('field', {}, slots.default?.()) } })
  const input = vue.defineComponent({ props: ['modelValue'], setup (props, { attrs }) { const field = vue.inject('field', ''); return () => vue.h('input', { ...attrs, field, value: props.modelValue }) } })
  const overlay = vue.defineComponent({ props: ['modelValue'], setup (props, { slots }) { return () => props.modelValue ? vue.h('overlay', {}, [slots.default?.(), slots.footer?.()]) : null } })
  const table = vue.defineComponent({ props: ['data'], setup (props, { slots }) { vue.provide('rows', vue.toRef(props, 'data')); return () => vue.h('table', {}, slots.default?.()) } })
  const column = vue.defineComponent({ setup (_, { slots }) { const rows = vue.inject('rows'); return () => vue.h('column', {}, rows.value.flatMap((row) => slots.default?.({ row }) || [])) } })
  for (const name of ['el-button', 'el-alert', 'el-form', 'el-tag', 'el-option', 'el-tab-pane']) app.component(name, container)
  app.component('el-form-item', formItem); app.component('el-input', input); app.component('el-select', input); app.component('el-tabs', input); app.component('el-input-number', input)
  app.component('el-dialog', overlay); app.component('el-drawer', overlay); app.component('el-table', table); app.component('el-table-column', column); app.directive('loading', {})
  app.mount(root); await settle()
  const click = async (label) => { const target = find(root, (n) => n.tag === 'box' && typeof n.props.onClick === 'function' && text(n) === label); assert.ok(target, `button ${label}`); assert.ok(!target.props.disabled); await target.props.onClick(); await settle() }
  const change = async (field, value) => { const input = find(root, (n) => n.tag === 'input' && n.props.field === field); assert.ok(input, field); input.props['onUpdate:modelValue'](value); await settle() }
  return { root, app, click, change, messages }
}
const definitions = { security_protocols: ['PLAINTEXT', 'SASL_SSL'], roles: ['source', 'sink', 'both'], sasl_mechanisms: [{ name: 'PLAIN', available: true }], mechanism_fields: { PLAIN: ['username', 'password'] }, credential_aliases: { files: [], environments: [] } }
test('Vue page keeps a failed draft, removes sink group, and cancels verification on unmount', async () => {
  const calls = [], channels = [], connections = [{ name: 'auth', revision: 1, security_protocol: 'PLAINTEXT' }]
  let conflict = true
  const api = {
    kafkaDefinitions: async () => ({ data: definitions }), kafkaQuery: async (kind) => ({ data: { items: kind === 'connections' ? connections : channels } }),
    kafkaSave: async (kind, payload) => { calls.push(['save', kind, payload]); if (conflict) throw { response: { status: 409 } }; channels.push({ ...payload, revision: 1, adapter_status: { sink: 'not_installed' } }) },
    kafkaTestStart: async (payload) => { calls.push(['start', payload]); return { data: { test_id: 'lease' } } },
    kafkaTestQuery: async () => ({ data: { status: 'running', stage: 'network' } }),
    kafkaTestCancel: async (id) => { calls.push(['cancel', id]) }
  }
  const page = await mountPage(api)
  try {
    await page.click('新建 Kafka 通道'); await page.change('通道名称', 'events'); await page.change('连接配置', 'auth'); await page.change('Topic', 'events')
    await page.change('消费组', 'previous'); await page.change('角色', 'sink')
    assert.ok(!find(page.root, (n) => n.props.field === '消费组'))
    await page.click('保存通道')
    assert.equal(calls.at(-1)[2].consumer_group, undefined)
    assert.ok(page.messages.at(-1).includes('草稿已保留'))
    assert.ok(find(page.root, (n) => n.props.field === '通道名称' && n.props.value === 'events'))
    conflict = false; await page.click('保存通道'); await page.click('验证')
    assert.equal(calls.find((c) => c[0] === 'start')[1].expected_revision, 1)
  } finally { page.app.unmount(); await settle() }
  assert.ok(calls.some((c) => c[0] === 'cancel' && c[1] === 'lease'))
})
test('Vue secret input is blank with configured keep and hidden auth is omitted on protocol change', async () => {
  const saved = [], row = { name: 'auth', revision: 4, bootstrap_servers: ['localhost:9092'], security_protocol: 'SASL_SSL', sasl_mechanism: 'PLAIN', sasl: { username: 'alice' }, credential_bindings: { password: { configured: true } } }
  const page = await mountPage({ kafkaDefinitions: async () => ({ data: definitions }), kafkaQuery: async (kind) => ({ data: { items: kind === 'connections' ? [row] : [] } }), kafkaSave: async (_kind, payload) => { saved.push(payload) }, kafkaTestCancel: async () => {} })
  try {
    await page.change('', 'connections'); await page.click('编辑')
    assert.ok(text(page.root).includes('已配置（不回显）'))
    assert.ok(!find(page.root, (n) => n.props.type === 'password'))
    await page.change('安全协议', 'PLAINTEXT'); await page.click('保存连接')
    assert.deepEqual(saved[0], { name: 'auth', expected_revision: 4, bootstrap_servers: ['localhost:9092'], security_protocol: 'PLAINTEXT' })
  } finally { page.app.unmount() }
})
test('Vue structured source form saves edited entity and preserves field units and limits', async () => {
  const saved = [], source = { encoding: 'json', entity: 'metrics', fields: [{ name: 'time', path: '/time', type: 'int64', nullable: false, unit: 'unix_ms' }], offset_reset: 'error', commit_policy: 'none', max_batch_rows: 17, max_message_bytes: 2048 }
  const row = { name: 'events', revision: 3, connection_name: 'auth', role: 'source', topic: 'events', consumer_group: 'test', payload_kind: 'structured', payload_options: { source }, adapter_status: { source: 'ready' } }
  const page = await mountPage({ kafkaDefinitions: async () => ({ data: definitions }), kafkaQuery: async (kind) => ({ data: { items: kind === 'channels' ? [row] : [{ name: 'auth' }] } }), kafkaSave: async (_kind, payload) => { saved.push(payload) }, kafkaTestCancel: async () => {} })
  try {
    await page.click('编辑'); await page.change('实体名称', 'renamed'); await page.change('无有效位点', 'latest'); await page.click('保存通道')
    assert.equal(saved[0].expected_revision, 3)
    assert.deepEqual(saved[0].payload_options.source, { ...source, entity: 'renamed', offset_reset: 'latest' })
  } finally { page.app.unmount() }
})
