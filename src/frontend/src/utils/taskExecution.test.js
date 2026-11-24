// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import test from 'node:test'
import { taskExecutionPolicy } from './taskExecution.js'

test('continuous capture forces async and explains the mode change', () => {
  const policy = taskExecutionPolicy({ task_kind: 'batch', statement_count: 1, requires_async: true }, 'sync')
  assert.equal(policy.mode, 'async')
  assert.equal(policy.allowSync, false)
  assert.equal(policy.continuousSource, true)
  assert.equal(policy.notice, '当前 SQL 使用连续采集源，已切换为异步执行。')
})

test('finite sources preserve the selected mode', () => {
  const analysis = { task_kind: 'batch', statement_count: 1, requires_async: false }
  assert.equal(taskExecutionPolicy(analysis, 'sync').mode, 'sync')
  assert.equal(taskExecutionPolicy(analysis, 'async').mode, 'async')
  assert.equal(taskExecutionPolicy(analysis, 'sync').allowSync, true)
  assert.equal(taskExecutionPolicy(analysis, 'sync').notice, '')
})

test('initial loading preserves the selected mode until analysis exists', () => {
  assert.equal(taskExecutionPolicy(null, 'sync').mode, 'sync')
  assert.equal(taskExecutionPolicy(null, 'async').mode, 'async')
  assert.equal(taskExecutionPolicy(null).notice, '')
})

test('existing stream and multi-statement execution still require async', () => {
  for (const analysis of [{ task_kind: 'stream', statement_count: 1, requires_async: true }, { task_kind: 'batch', statement_count: 2, requires_async: false }]) {
    const policy = taskExecutionPolicy(analysis, 'sync')
    assert.equal(policy.mode, 'async')
    assert.equal(policy.allowSync, false)
    assert.equal(policy.continuousSource, false)
  }
})


test('DESC uses synchronous reading regardless of selected execution mode', () => {
  const policy = taskExecutionPolicy({ command_kind: 'describe', task_kind: null, statement_count: 1, requires_async: false }, 'async')
  assert.equal(policy.mode, 'sync')
  assert.equal(policy.readOnly, true)
  assert.equal(policy.allowSync, true)
})
