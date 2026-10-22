import test from 'node:test'
import assert from 'node:assert/strict'
import { computationClient } from '../../exploration/src/compute.js'

test('parallel subqueries retain their own identity and stale completions cannot change status', async () => {
  const waiting = [], visible = [], errors = []
  globalThis.fetch = (url, options) => new Promise((resolve) => waiting.push({ url, options, resolve }))
  const client = computationClient({ source_id: 'source', schema_revision: 'revision' }, (r) => visible.push(r), (e) => errors.push(e))
  const first = client.compute({ workflow: [1] }), second = client.compute({ workflow: [2] })
  assert.notEqual(JSON.parse(waiting[0].options.body).request_id, JSON.parse(waiting[1].options.body).request_id)
  waiting[1].resolve({ ok: true, json: async () => ({ rows: [2] }) })
  assert.deepEqual(await second, [2])
  waiting[0].resolve({ ok: true, json: async () => ({ rows: [1] }) })
  assert.deepEqual(await first, [1]); assert.deepEqual(visible, [{ rows: [2] }]); assert.deepEqual(errors, [])
})

test('hide sends explicit server cancellation and blocks stale responses and new requests', async () => {
  const waiting = [], cancelled = [], visible = []
  globalThis.fetch = (url, options) => {
    if (url.endsWith('/cancel')) { cancelled.push(JSON.parse(options.body).request_id); return Promise.resolve({ ok: true }) }
    return new Promise((resolve) => waiting.push({ options, resolve }))
  }
  const client = computationClient({ source_id: 'source', schema_revision: 'revision' }, (r) => visible.push(r), () => {})
  const pending = client.compute({ workflow: [] })
  client.setVisible(false)
  assert.equal(cancelled.length, 1); assert(waiting[0].options.signal.aborted)
  waiting[0].resolve({ ok: true, json: async () => ({ rows: [1] }) })
  await assert.rejects(pending, { name: 'AbortError' })
  await assert.rejects(client.compute({ workflow: [] }), /已隐藏/)
  assert.deepEqual(visible, [])
})

test('closed source rejects late data; another source and revision keep their own request identity', async () => {
  const waiting = [], updates = []
  globalThis.fetch = (url, options) => url.endsWith('/cancel') ? Promise.resolve({ ok: true }) :
    new Promise((resolve) => waiting.push({ options, resolve }))
  const a = computationClient({ source_id: 'a', schema_revision: 'one' }, (r) => updates.push(['a', r]), () => {})
  const b = computationClient({ source_id: 'b', schema_revision: 'two' }, (r) => updates.push(['b', r]), () => {})
  const first = a.compute({ workflow: [] })
  a.setVisible(false)
  const second = b.compute({ workflow: [] })
  assert.equal(JSON.parse(waiting[0].options.body).source_id, 'a')
  assert.equal(JSON.parse(waiting[1].options.body).schema_revision, 'two')
  waiting[1].resolve({ ok: true, json: async () => ({ rows: [2] }) })
  await second
  waiting[0].resolve({ ok: true, json: async () => ({ rows: [1] }) })
  await assert.rejects(first, { name: 'AbortError' })
  assert.deepEqual(updates, [['b', { rows: [2] }]])
})
