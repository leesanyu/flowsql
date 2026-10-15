// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
import assert from 'node:assert/strict'
import test from 'node:test'
import { createKafkaChannelApi } from './kafkaChannel.js'
test('Kafka API preserves revision, asynchronous IDs and source/sink management paths', async () => {
  const calls = []
  const api = createKafkaChannelApi({ post: async (...args) => { calls.push(args); return { data: {} } } })
  await api.kafkaDefinitions(); await api.kafkaQuery('channels', 'alpha')
  await api.kafkaSave('connections', { name: 'auth', expected_revision: 7 }, true)
  await api.kafkaRemove('channels', { name: 'events', revision: 3, secret: 'must-not-send' })
  await api.kafkaTestStart({ target: 'channel', name: 'events' }); await api.kafkaTestQuery('8'); await api.kafkaTestCancel('8')
  assert.deepEqual(calls.map(([path]) => path), ['definitions/query', 'channels/query', 'connections/modify', 'channels/remove', 'tests/start', 'tests/query', 'tests/cancel'].map((p) => `/api/channels/kafka/${p}`))
  assert.deepEqual(calls[3][1], { name: 'events', expected_revision: 3 })
})
