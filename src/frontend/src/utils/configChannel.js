// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

export const CONFIG_CONTENT_MAX_BYTES = 8 * 1024 * 1024

const byteLength = (value) => new TextEncoder().encode(String(value ?? '')).length

const hasUnpairedSurrogate = (value) => {
  for (let index = 0; index < value.length; index++) {
    const unit = value.charCodeAt(index)
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const next = value.charCodeAt(++index)
      if (!(next >= 0xdc00 && next <= 0xdfff)) return true
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return true
    }
  }
  return false
}

export const encodeUtf8Base64 = (content) => {
  const bytes = new TextEncoder().encode(content)
  let binary = ''
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000))
  }
  return btoa(binary)
}

export const decodeConfigBytes = (encoded) => {
  const binary = atob(encoded)
  return Uint8Array.from(binary, (value) => value.charCodeAt(0))
}

export const decodeUtf8Bytes = (bytes) =>
  new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes)

export const decodeUtf8Base64 = (encoded) =>
  decodeUtf8Bytes(decodeConfigBytes(encoded))

export const formatFromFilename = (filename) => {
  const match = String(filename || '').toLowerCase().match(/\.([^.]+)$/)
  if (!match) return ''
  if (match[1] === 'json' || match[1] === 'xml') return match[1]
  return match[1] === 'yaml' || match[1] === 'yml' ? 'yaml' : ''
}

export const configReference = (name, revision) => `config.${name}@${revision}`

export const configDownloadName = (name, revision, format) =>
  `${configReference(name, revision)}.${format === 'yaml' ? 'yaml' : format}`

export const validateConfigDraft = (draft) => {
  if (!/^[a-z][a-z0-9_-]{0,63}$/.test(draft.name || '')) return '通道名称格式无效'
  if (!['json', 'yaml', 'xml'].includes(draft.format)) return '请选择配置格式'
  if (!draft.schema_id) return 'Schema ID 为必填项'
  if (byteLength(draft.schema_id) > 255) return 'Schema ID 不能超过 255 字节'
  if (byteLength(draft.original_filename) > 255) return '文件名不能超过 255 字节'
  if (byteLength(draft.change_note) > 1024) return '变更说明不能超过 1024 字节'
  if (!draft.content) return '配置内容不能为空'
  if (hasUnpairedSurrogate(draft.content)) return '配置内容必须是有效 UTF-8 文本'
  if (byteLength(draft.content) > CONFIG_CONTENT_MAX_BYTES) return '配置内容不能超过 8 MiB'
  return ''
}

export const buildConfigPublishPayload = (draft, expectedRevision, baseRevision = 0) => {
  const payload = {
    name: draft.name,
    expected_current_revision: expectedRevision,
    format: draft.format,
    schema_id: draft.schema_id,
    content_base64: encodeUtf8Base64(draft.content)
  }
  if (baseRevision) payload.base_revision = baseRevision
  if (draft.original_filename) payload.original_filename = draft.original_filename
  if (draft.change_note) payload.change_note = draft.change_note
  return payload
}

export const configChannelErrorMessage = (error) => {
  const status = error?.response?.status
  if (status === 409) return '内容已更新，请刷新后重新编辑'
  if (status === 413) return '配置内容或请求超过大小限制'
  if (status === 503) return '配置服务暂不可用'
  const detail = error?.response?.data?.error
  if (typeof detail === 'string' && detail) return detail
  if (status) return `配置操作失败（HTTP ${status}）`
  return '配置服务暂不可用'
}
