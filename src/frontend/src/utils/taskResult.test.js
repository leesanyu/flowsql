// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import test from 'node:test'

import { formatTaskResult } from './taskResult.js'

test('named DataFrame completion reports rows without inventing a column count', () => {
  const expected = {
    columns: [],
    rows: [],
    message: '执行完成（695 行已写入）'
  }

  assert.deepEqual(formatTaskResult({ status: 'completed', rows: 695, data: [] }), expected)
  assert.deepEqual(formatTaskResult({ status: 'completed', rows: 695, cols: 30, data: [] }), expected)
  assert.deepEqual(formatTaskResult({ status: 'completed', rows: 695 }), expected)
})

test('completed inline object rows still produce table columns', () => {
  const data = [
    { source_ip: '172.16.210.84', source_port: 31600 },
    { source_ip: '172.16.30.190', source_port: 8871 }
  ]

  assert.deepEqual(formatTaskResult({ status: 'completed', data }), {
    columns: ['source_ip', 'source_port'],
    rows: data
  })
})

test('completed structured rows still produce table data', () => {
  assert.deepEqual(
    formatTaskResult({
      status: 'completed',
      data: {
        columns: ['source_ip', 'source_port'],
        data: [['172.16.210.84', 31600]]
      }
    }),
    {
      columns: ['source_ip', 'source_port'],
      rows: [{ source_ip: '172.16.210.84', source_port: 31600 }]
    }
  )
})

test('failed, running, and unknown task states keep their existing messages', () => {
  assert.deepEqual(formatTaskResult({ status: 'failed', error: 'boom' }), { error: 'boom' })
  assert.deepEqual(formatTaskResult({ status: 'failed' }), { error: '执行失败' })
  assert.deepEqual(formatTaskResult({ status: 'running' }), {
    columns: [],
    rows: [],
    message: '任务执行中（running）'
  })
  assert.deepEqual(formatTaskResult({ status: 'pending' }, '正在等待'), {
    columns: [],
    rows: [],
    message: '正在等待'
  })
  assert.deepEqual(formatTaskResult({}), {
    columns: [],
    rows: [],
    message: '任务状态：unknown'
  })
})

test('captured result columns retain positions, duplicate labels and zero-row schema', () => {
  const semantic_context = { state: 'available', fields: [{ output_index: 0 }, { output_index: 1 }] }
  const result = formatTaskResult({ status: 'completed', result_columns: ['same', 'same'], result_values: [[1, 2]], semantic_context })
  assert.deepEqual(result.columns, ['same', 'same'])
  assert.deepEqual(result.rows, [{ column_0: 1, column_1: 2 }])
  assert.equal(result.semanticContext, semantic_context)
  const empty = formatTaskResult({ status: 'completed', rows: 0, result_columns: ['zero'], result_values: [], semantic_context })
  assert.deepEqual(empty.columns, ['zero'])
  assert.deepEqual(empty.rows, [])
})
