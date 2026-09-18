// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { createRenderer, defineComponent, h, inject, nextTick, provide, toRef } from 'vue'
import * as vue from 'vue'
import { parse, compileScript } from '@vue/compiler-sfc'
import { transform } from 'esbuild'

import * as configChannelUtils from './configChannel.js'

import {
  CONFIG_CONTENT_MAX_BYTES,
  buildConfigPublishPayload,
  configDownloadName,
  configReference,
  decodeConfigBytes,
  decodeUtf8Bytes,
  configChannelErrorMessage,
  decodeUtf8Base64,
  encodeUtf8Base64,
  formatFromFilename,
  validateConfigDraft
} from './configChannel.js'

test('UTF-8 content round trips through control-plane Base64', () => {
  const content = '规则：中\n<value>✓</value>'
  assert.equal(decodeUtf8Base64(encodeUtf8Base64(content)), content)
  assert.equal(decodeUtf8Base64(encodeUtf8Base64('\ufeff<root/>')), '\ufeff<root/>')
  assert.equal(decodeUtf8Bytes(new Uint8Array([0xef, 0xbb, 0xbf, 0x7b, 0x7d])), '\ufeff{}')
  assert.deepEqual([...decodeConfigBytes(encodeUtf8Base64('中'))], [...new TextEncoder().encode('中')])
})

test('publish payload keeps system revision read-only and includes optional metadata', () => {
  const draft = {
    name: 'corp-apps', format: 'yaml', schema_id: 'scope-v1', content: 'apps: []\n',
    original_filename: 'apps.yml', change_note: 'add rules'
  }
  assert.equal(validateConfigDraft(draft), '')
  assert.deepEqual(buildConfigPublishPayload(draft, 7, 3), {
    name: 'corp-apps', expected_current_revision: 7, format: 'yaml', schema_id: 'scope-v1',
    content_base64: 'YXBwczogW10K', base_revision: 3,
    original_filename: 'apps.yml', change_note: 'add rules'
  })
})

test('draft validation enforces names, metadata, nonempty UTF-8 bytes, and 512 KiB', () => {
  const valid = { name: 'a', format: 'json', schema_id: 'v1', content: '{}' }
  assert.equal(validateConfigDraft(valid), '')
  assert.equal(validateConfigDraft({ ...valid, name: 'A' }), '通道名称格式无效')
  assert.equal(validateConfigDraft({ ...valid, schema_id: '' }), 'Schema ID 为必填项')
  assert.equal(validateConfigDraft({ ...valid, content: '' }), '配置内容不能为空')
  assert.equal(validateConfigDraft({ ...valid, content: '\ud800' }), '配置内容必须是有效 UTF-8 文本')
  assert.equal(validateConfigDraft({ ...valid, content: 'x'.repeat(CONFIG_CONTENT_MAX_BYTES + 1) }),
    '配置内容不能超过 512 KiB')
  assert.equal(validateConfigDraft({ ...valid, content: '中'.repeat(174763) }),
    '配置内容不能超过 512 KiB')
})

test('file extensions map only to supported formats', () => {
  assert.equal(formatFromFilename('rules.JSON'), 'json')
  assert.equal(formatFromFilename('rules.yml'), 'yaml')
  assert.equal(formatFromFilename('rules.yaml'), 'yaml')
  assert.equal(formatFromFilename('rules.xml'), 'xml')
  assert.equal(formatFromFilename('rules.txt'), '')
})

test('conflict and transport errors have stable user guidance', () => {
  assert.equal(configChannelErrorMessage({ response: { status: 409 } }), '内容已更新，请刷新后重新编辑')
  assert.equal(configChannelErrorMessage({ response: { status: 413 } }), '配置内容或请求超过大小限制')
  assert.equal(configChannelErrorMessage({ response: { data: { error: 'invalid YAML syntax' } } }),
    'invalid YAML syntax')
  assert.equal(configChannelErrorMessage(new Error('Network Error')), '配置服务暂不可用')
})

test('immutable references and download names derive from metadata, not uploaded filenames', () => {
  assert.equal(configReference('corp-apps', 7), 'config.corp-apps@7')
  assert.equal(configDownloadName('corp-apps', 7, 'yaml'), 'config.corp-apps@7.yaml')
  assert.equal(configDownloadName('corp-apps', 7, 'xml'), 'config.corp-apps@7.xml')
  assert.equal(configDownloadName('corp-apps', 7, 'json'), 'config.corp-apps@7.json')
})

test('restored revision is a new publish based on historical snapshot and current pointer', () => {
  const previous = { name: 'corp-apps', format: 'json', schema_id: 'scope-v1', content: '<script>alert(1)</script>' }
  const payload = buildConfigPublishPayload(previous, 7, 3)
  assert.equal(payload.expected_current_revision, 7)
  assert.equal(payload.base_revision, 3)
  assert.equal(decodeUtf8Base64(payload.content_base64), '<script>alert(1)</script>')
  assert.equal(Object.hasOwn(payload, 'revision'), false)
})

test('preview renders untrusted XML and HTML as escaped plain text, without editable revision', () => {
  const component = readFileSync(new URL('../views/ConfigChannels.vue', import.meta.url), 'utf8')
  assert.match(component, /<pre\s+v-text="previewText"/)
  assert.doesNotMatch(component, /v-html|innerHTML|v-model(?:\.[a-z]+)?="(?:expectedRevision|baseRevision)"/)
})

test('config page publishes with optimistic revision, keeps draft on 409, and restores history', async () => {
  const calls = []
  const messages = []
  const current = {
    name: 'alpha', current_revision: 7, format: 'json', schema_id: 'scope-v1',
    content_bytes: 2, sha256_hex: 'a'.repeat(64), updated_at_unix_ms: 1
  }
  let conflict = false
  const mockApi = {
    listConfigChannels: async (cursor) => {
      calls.push(['list', cursor])
      return { data: { items: [current], next_cursor: '' } }
    },
    listConfigHistory: async (name, cursor) => {
      calls.push(['history', name, cursor])
      return { data: { items: [
        { revision: 3, format: 'json', schema_id: 'scope-v1', content_bytes: 13,
          sha256_hex: 'b'.repeat(64), created_at_unix_ms: 1, base_revision: null, change_note: 'original' }
      ], next_cursor: '' } }
    },
    resolveConfigChannel: async (reference) => {
      calls.push(['resolve', reference])
      return { data: {
        name: 'alpha', revision: Number(reference.split('@')[1]), format: 'json', schema_id: 'scope-v1',
        content_base64: encodeUtf8Base64('{"past":true}')
      } }
    },
    publishConfigChannel: async (payload) => {
      calls.push(['publish', payload])
      if (conflict) throw { response: { status: 409, data: { error: 'current revision conflict' } } }
      return { data: { revision: 8, exact_reference: 'config.alpha@8', created_revision: true } }
    }
  }
  const source = readFileSync(new URL('../views/ConfigChannels.vue', import.meta.url), 'utf8')
  const { descriptor } = parse(source)
  const compiled = compileScript(descriptor, { id: 'config-page-test', inlineTemplate: true })
  const output = await transform(compiled.content, { format: 'cjs', loader: 'js' })
  const module = { exports: {} }
  const mockedRequire = (name) => {
    if (name === 'vue') return vue
    if (name === '../api') return mockApi
    if (name === '../utils/configChannel.js') return configChannelUtils
    if (name === 'element-plus') {
      return { ElMessage: {
        error: (text) => messages.push(['error', text]),
        warning: (text) => messages.push(['warning', text]),
        success: (text) => messages.push(['success', text])
      } }
    }
    throw new Error(`unexpected config-page dependency: ${name}`)
  }
  new Function('require', 'module', 'exports', output.code)(mockedRequire, module, module.exports)
  const ConfigChannels = module.exports.default
  let app
  try {
    const root = makeNode('root')
    const renderer = createTestRenderer()
    app = renderer.createApp(ConfigChannels, { searchText: '' })
    const container = defineComponent({
      setup (_, { slots }) { return () => h('div', {}, slots.default?.()) }
    })
    const table = defineComponent({
      props: ['data'],
      setup (props, { slots }) {
        provide('rows', toRef(props, 'data'))
        return () => h('table', {}, slots.default?.())
      }
    })
    const column = defineComponent({
      setup (_, { slots }) {
        const rows = inject('rows')
        return () => h('column', {}, rows?.value?.flatMap((row) => slots.default?.({ row }) || []) || [])
      }
    })
    const overlay = defineComponent({
      props: ['modelValue'],
      setup (props, { slots }) {
        return () => props.modelValue
          ? h('overlay', {}, [slots.default?.(), slots.footer?.()]) : null
      }
    })
    const input = defineComponent({
      props: ['modelValue'],
      setup (props, { attrs }) { return () => h('input', { ...attrs, value: props.modelValue }) }
    })
    for (const name of ['el-button', 'el-alert', 'el-form', 'el-form-item', 'el-tag', 'el-radio-group',
      'el-radio-button', 'el-option']) app.component(name, container)
    app.component('el-table', table)
    app.component('el-table-column', column)
    app.component('el-dialog', overlay)
    app.component('el-drawer', overlay)
    app.component('el-input', input)
    app.component('el-select', input)
    app.directive('loading', {})
    app.mount(root)
    await settle(3)

    const click = async (text) => {
      const button = findNode(root, (node) => node.tag === 'div' &&
        typeof node.props.onClick === 'function' && nodeText(node) === text)
      assert.ok(button, `missing button ${text}; rendered ${nodeText(root)}; calls ${JSON.stringify(calls)}`)
      await button.props.onClick()
      await settle()
    }
    await click('编辑并发布')
    const textarea = findNode(root, (node) => node.tag === 'input' && node.props.type === 'textarea')
    assert.ok(textarea)
    assert.equal(textarea.props.value, '{"past":true}')
    const revisionInput = findNode(root, (node) => node.tag === 'input' &&
      node.props.value === 'r7（只读）')
    assert.ok(revisionInput)
    assert.equal(Object.hasOwn(revisionInput.props, 'disabled'), true)
    textarea.props['onUpdate:modelValue']('{"changed":true}')
    await settle()

    conflict = true
    await click('发布')
    assert.deepEqual(calls.at(-1), ['publish', {
      name: 'alpha', expected_current_revision: 7, format: 'json', schema_id: 'scope-v1',
      content_base64: encodeUtf8Base64('{"changed":true}')
    }])
    assert.equal(messages.at(-1)[1], '内容已更新，请刷新后重新编辑')
    assert.ok(findNode(root, (node) => node.tag === 'input' && node.props.value === '{"changed":true}'))

    await click('取消')
    await click('版本历史')
    assert.ok(calls.some((call) => call[0] === 'history' && call[1] === 'alpha'))
    await click('基于此版本创建新版本')
    conflict = false
    await click('发布')
    assert.deepEqual(calls.findLast((call) => call[0] === 'publish'), ['publish', {
      name: 'alpha', expected_current_revision: 7, format: 'json', schema_id: 'scope-v1',
      content_base64: encodeUtf8Base64('{"past":true}'), base_revision: 3,
      change_note: '基于 revision 3 创建新版本'
    }])
    assert.ok(calls.some((call) => call[0] === 'list' && call[1] === ''))
  } finally {
    app?.unmount()
  }
})

const makeNode = (tag, text = '') => ({ tag, text, props: {}, parent: null, children: [] })
const nodeText = (node) => node.text + node.children.map(nodeText).join('')
const findNode = (node, predicate) => predicate(node)
  ? node : node.children.map((child) => findNode(child, predicate)).find(Boolean)
const settle = async (times = 1) => {
  for (let count = 0; count < times; count++) {
    await new Promise((resolve) => setImmediate(resolve))
    await nextTick()
  }
}

const createTestRenderer = () => createRenderer({
  createElement: (tag) => makeNode(tag),
  createText: (text) => makeNode('#text', text),
  createComment: (text) => makeNode('#comment', text),
  setText: (node, text) => { node.text = text },
  setElementText: (node, text) => { node.text = text; node.children = [] },
  patchProp: (node, key, _before, after) => { node.props[key] = after },
  insert: (node, parent, anchor = null) => {
    if (node.parent) node.parent.children.splice(node.parent.children.indexOf(node), 1)
    node.parent = parent
    const index = anchor ? parent.children.indexOf(anchor) : -1
    parent.children.splice(index < 0 ? parent.children.length : index, 0, node)
  },
  remove: (node) => {
    if (node.parent) node.parent.children.splice(node.parent.children.indexOf(node), 1)
    node.parent = null
  },
  parentNode: (node) => node.parent,
  nextSibling: (node) => {
    const children = node.parent?.children || []
    return children[children.indexOf(node) + 1] || null
  }
})
