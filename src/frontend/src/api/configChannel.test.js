// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import test from 'node:test'

import { createConfigChannelApi } from './configChannel.js'

test('config channel control API uses bounded cursors and exact revision references', async () => {
  const calls = []
  const reply = { data: { items: [], next_cursor: '' } }
  const client = {
    post: async (path, body) => {
      calls.push([path, body])
      return reply
    }
  }
  const api = createConfigChannelApi(client)
  assert.equal(await api.listConfigChannels(), reply)
  assert.equal(await api.listConfigChannels('alpha', 500), reply)
  assert.equal(await api.listConfigHistory('alpha', '3', 2), reply)
  assert.equal(await api.resolveConfigChannel('config.alpha@3'), reply)
  const publish = {
    name: 'alpha', expected_current_revision: 3, format: 'yaml',
    schema_id: 'rules-v1', content_base64: 'YQ==', base_revision: 1
  }
  assert.equal(await api.publishConfigChannel(publish), reply)
  assert.deepEqual(calls, [
    ['/api/channels/config/list', { cursor: '', limit: 100 }],
    ['/api/channels/config/list', { cursor: 'alpha', limit: 100 }],
    ['/api/channels/config/history', { name: 'alpha', cursor: '3', limit: 2 }],
    ['/api/channels/config/resolve', { exact_reference: 'config.alpha@3' }],
    ['/api/channels/config/publish', publish]
  ])
})

test('config control error status and body are not swallowed', async () => {
  const conflict = { response: { status: 409, data: { error: 'current revision conflict' } } }
  const api = createConfigChannelApi({ post: async () => { throw conflict } })
  await assert.rejects(api.publishConfigChannel({}), (error) => error === conflict)
})
