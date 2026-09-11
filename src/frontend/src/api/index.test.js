// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

import assert from 'node:assert/strict'
import test from 'node:test'

import {
  PCAP_UPLOAD_DEFAULTS,
  PCAP_UPLOAD_PATH,
  PCAP_UPLOAD_REQUEST_CONFIG,
  PCAP_REPLAY_SPEEDS,
  buildPcapUploadFormData,
  formatPreviewCell,
  isPacketRawPreviewValue,
  isSupportedPcapFile,
  pcapUploadErrorMessage,
  replaySpeedToMilli
} from './index.js'

class RecordingFormData {
  constructor () {
    this.parts = []
  }

  append (name, value) {
    this.parts.push([name, value])
  }
}

test('PCAP upload FormData keeps scalar fields before the file part', () => {
  assert.equal(PCAP_UPLOAD_PATH, '/api/channels/pcapfile/upload')
  assert.deepEqual(PCAP_UPLOAD_REQUEST_CONFIG, { timeout: 0 })
  const file = { name: 'capture.pcap' }
  const form = buildPcapUploadFormData(file, { name: ' capture_01 ' }, () => new RecordingFormData())

  assert.deepEqual(form.parts, [
    ['name', 'capture_01'],
    ['format', PCAP_UPLOAD_DEFAULTS.format],
    ['batch_packets', String(PCAP_UPLOAD_DEFAULTS.batch_packets)],
    ['replay_mode', PCAP_UPLOAD_DEFAULTS.replay_mode],
    ['replay_speed_milli', '1000'],
    ['file', file]
  ])
})

test('PCAP upload FormData serializes explicit values as multipart text', () => {
  const file = { name: 'capture.pcapng' }
  const form = buildPcapUploadFormData(
    file,
    {
      name: 'capture_02',
      format: 'pcapng',
      batch_packets: 512,
      replay_mode: 'timestamp',
      replay_speed: 10
    },
    () => new RecordingFormData()
  )

  assert.deepEqual(form.parts.slice(0, -1), [
    ['name', 'capture_02'],
    ['format', 'pcapng'],
    ['batch_packets', '512'],
    ['replay_mode', 'timestamp'],
    ['replay_speed_milli', '10000']
  ])
  assert.deepEqual(form.parts.at(-1), ['file', file])
})

test('PCAP replay speed uses literal multipliers and maps exactly to the backend unit', () => {
  assert.equal(PCAP_UPLOAD_DEFAULTS.replay_speed, 1)
  assert.deepEqual(PCAP_REPLAY_SPEEDS, [0.001, 0.01, 0.1, 1, 10, 100, 1000])
  assert.deepEqual(
    PCAP_REPLAY_SPEEDS.map(replaySpeedToMilli),
    [1, 10, 100, 1000, 10000, 100000, 1000000]
  )
})

test('PCAP file selection accepts only pcap and pcapng extensions', () => {
  assert.equal(isSupportedPcapFile({ name: 'capture.pcap' }), true)
  assert.equal(isSupportedPcapFile({ name: 'CAPTURE.PCAPNG' }), true)
  assert.equal(isSupportedPcapFile({ name: 'capture.pcap.txt' }), false)
  assert.equal(isSupportedPcapFile({ name: '.pcap' }), false)
  assert.equal(isSupportedPcapFile(null), false)
})

test('PCAP upload errors are mapped to stable Chinese messages', () => {
  const expected = {
    invalid_request: '上传参数或文件无效',
    channel_conflict: '通道名称已存在',
    file_too_large: '文件超过上传大小限制',
    provider_unavailable: 'PCAP 服务暂不可用',
    storage_failure: '服务器保存文件失败',
    internal_error: '服务器内部错误'
  }
  for (const [code, message] of Object.entries(expected)) {
    assert.equal(pcapUploadErrorMessage({ response: { data: { error: code } } }), message)
  }
  assert.equal(
    pcapUploadErrorMessage({ response: { data: { error: 'future_error' } } }),
    'PCAP 上传失败（future_error）'
  )
  assert.equal(pcapUploadErrorMessage({ response: { status: 502 } }), 'PCAP 上传失败（HTTP 502）')
  assert.equal(pcapUploadErrorMessage(new Error('Network Error')), '无法连接 PCAP 上传服务')
})

test('packet raw preview cells show hex, full length, and truncation', () => {
  const empty = { hex: '', byte_length: 0, truncated: false }
  const complete = { hex: '0001aaff', byte_length: 4, truncated: false }
  const truncated = { hex: '01234567', byte_length: 65, truncated: true }

  assert.equal(isPacketRawPreviewValue(empty), true)
  assert.equal(isPacketRawPreviewValue(complete), true)
  assert.equal(isPacketRawPreviewValue(truncated), true)
  assert.equal(isPacketRawPreviewValue({ hex: '00', byte_length: -1, truncated: false }), false)
  assert.equal(isPacketRawPreviewValue({ hex: '00', byte_length: 1.5, truncated: false }), false)
  assert.equal(formatPreviewCell(empty), '（0 B）')
  assert.equal(formatPreviewCell(complete), '0001aaff（4 B）')
  assert.equal(formatPreviewCell(truncated), '01234567…（65 B，已截断）')
})

test('other preview cell values have stable readable text', () => {
  assert.equal(formatPreviewCell(null), 'NULL')
  assert.equal(formatPreviewCell(undefined), 'NULL')
  assert.equal(formatPreviewCell([1, 300, 0]), '[1,300,0]')
  assert.equal(formatPreviewCell({ status: 'ok' }), '{"status":"ok"}')
  assert.equal(formatPreviewCell(true), 'true')
  assert.equal(formatPreviewCell(3389), '3389')
  assert.equal(formatPreviewCell('rdp'), 'rdp')
})
