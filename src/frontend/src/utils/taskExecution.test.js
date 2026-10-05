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

test('finite sources and legacy analysis preserve the selected mode', () => {
  for (const requires_async of [false, undefined]) {
    const analysis = { task_kind: 'batch', statement_count: 1, requires_async }
    assert.equal(taskExecutionPolicy(analysis, 'sync').mode, 'sync')
    assert.equal(taskExecutionPolicy(analysis, 'async').mode, 'async')
    assert.equal(taskExecutionPolicy(analysis, 'sync').allowSync, true)
    assert.equal(taskExecutionPolicy(analysis, 'sync').notice, '')
  }
})

test('existing stream and multi-statement execution still require async', () => {
  for (const analysis of [{ task_kind: 'stream', statement_count: 1 }, { task_kind: 'batch', statement_count: 2 }]) {
    const policy = taskExecutionPolicy(analysis, 'sync')
    assert.equal(policy.mode, 'async')
    assert.equal(policy.allowSync, false)
    assert.equal(policy.continuousSource, false)
  }
})
