import assert from 'node:assert/strict'
import { test } from 'node:test'
import { describeBuildInfo } from '../src/productVersion.js'

test('build details distinguish clean, dirty and unknown identities', () => {
  assert.equal(describeBuildInfo({ revision: '1234567890abcdef', dirty: false }),
    '提交：1234567890ab；源码状态：无本地修改')
  assert.equal(describeBuildInfo({ revision: '1234567890abcdef', dirty: true }),
    '提交：1234567890ab；源码状态：有本地修改')
  assert.equal(describeBuildInfo({ revision: null, dirty: null }),
    '提交：未知；源码状态：未知')
})
