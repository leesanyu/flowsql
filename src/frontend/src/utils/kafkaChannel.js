// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
const named = /^[a-z][a-z0-9_-]{0,63}$/
export const emptySecret = () => ({ operation: 'keep', mode: 'value', value: '', reference: '', configured: false })
export const connectionDraft = (row = {}) => ({
  name: row.name || '', servers: (row.bootstrap_servers || []).join('\n'),
  security_protocol: row.security_protocol || 'PLAINTEXT', sasl_mechanism: row.sasl_mechanism || 'PLAIN',
  tls: { ca_ref: '', certificate_ref: '', private_key_ref: '', ...row.tls },
  sasl: { username: '', principal: '', service_name: 'kafka', keytab_ref: '', token_endpoint: '', client_id: '', scope: '', ca_ref: '', ...row.sasl },
  secrets: Object.fromEntries(['password', 'client_secret', 'private_key_password'].map((field) => [field, {
    ...emptySecret(), configured: !!row.credential_bindings?.[field]?.configured,
    reference: row.credential_bindings?.[field]?.reference || ''
  }]))
})
export const channelDraft = (row = {}) => ({
  name: row.name || '', connection_name: row.connection_name || '', role: row.role || 'source',
  topic: row.topic || '', consumer_group: row.consumer_group || '', payload_kind: row.payload_kind || 'structured',
  source_enabled: !!row.payload_options?.source?.fields?.length,
  source: {
    entity: row.payload_options?.source?.entity || 'events',
    fields: (row.payload_options?.source?.fields || [{ name: 'message', path: '/message', type: 'utf8', nullable: false }]).map((field) => ({ ...field, unit: field.unit || '' })),
    offset_reset: row.payload_options?.source?.offset_reset || 'earliest',
    commit_policy: row.payload_options?.source?.commit_policy || 'after_success',
    max_batch_rows: row.payload_options?.source?.max_batch_rows ?? 256,
    max_message_bytes: row.payload_options?.source?.max_message_bytes ?? 1048576
  }
})
export const usesTls = (protocol) => ['SSL', 'SASL_SSL'].includes(protocol)
export const usesSasl = (protocol) => ['SASL_PLAINTEXT', 'SASL_SSL'].includes(protocol)
export const secretFields = (draft) => [
  ...(usesSasl(draft.security_protocol) && ['PLAIN', 'SCRAM-SHA-256', 'SCRAM-SHA-512'].includes(draft.sasl_mechanism) ? ['password'] : []),
  ...(usesSasl(draft.security_protocol) && draft.sasl_mechanism === 'OAUTHBEARER' ? ['client_secret'] : []),
  ...(usesTls(draft.security_protocol) && draft.tls.certificate_ref ? ['private_key_password'] : [])
]
const requireValue = (condition, message) => { if (!condition) throw new Error(message) }
export const buildConnection = (draft, revision = 0) => {
  requireValue(named.test(draft.name), '名称须以小写字母开头，最多 64 个小写字母、数字、下划线或短横线')
  const bootstrap_servers = draft.servers.split(/[\s,]+/).filter(Boolean)
  requireValue(bootstrap_servers.length > 0 && bootstrap_servers.length <= 32, '请填写 1～32 个 broker 地址（host:port）')
  const payload = { name: draft.name, bootstrap_servers, security_protocol: draft.security_protocol }
  if (revision) payload.expected_revision = revision
  if (usesTls(draft.security_protocol)) {
    requireValue(!!draft.tls.certificate_ref === !!draft.tls.private_key_ref, '客户端证书和私钥必须成对选择')
    payload.tls = Object.fromEntries(Object.entries(draft.tls).filter(([, value]) => value))
  }
  if (usesSasl(draft.security_protocol)) {
    payload.sasl_mechanism = draft.sasl_mechanism
    const fields = ['PLAIN', 'SCRAM-SHA-256', 'SCRAM-SHA-512'].includes(draft.sasl_mechanism)
      ? ['username'] : draft.sasl_mechanism === 'GSSAPI' ? ['principal', 'service_name', 'keytab_ref']
        : ['token_endpoint', 'client_id', 'scope', 'ca_ref']
    payload.sasl = Object.fromEntries(fields.filter((key) => draft.sasl[key]).map((key) => [key, draft.sasl[key]]))
  }
  const bindings = {}
  for (const field of secretFields(draft)) {
    const secret = draft.secrets[field]
    const required = field !== 'private_key_password'
    requireValue(!(secret.operation === 'clear' && required), '当前认证机制的必需凭证不能清除')
    requireValue(!(secret.operation === 'keep' && required && !secret.configured), '请填写或引用当前机制的必需凭证')
    if (secret.operation === 'keep') continue
    if (secret.operation === 'clear') { bindings[field] = { operation: 'clear' }; continue }
    const key = secret.mode === 'reference' ? 'reference' : 'value'
    requireValue(secret[key] && secret[key] !== '********', '请填写新凭证，不能提交遮罩文本')
    bindings[field] = { operation: 'replace', [key]: secret[key] }
  }
  if (Object.keys(bindings).length) payload.credential_bindings = bindings
  return payload
}
export const buildChannel = (draft, revision = 0) => {
  requireValue(named.test(draft.name) && named.test(draft.connection_name), '请填写合法通道名称并选择连接')
  requireValue(draft.topic && /^[A-Za-z0-9._-]{1,249}$/.test(draft.topic) && !['.', '..'].includes(draft.topic), '请填写合法的单个 Topic')
  requireValue(['source', 'sink', 'both'].includes(draft.role), '请选择通道角色')
  const payload = { name: draft.name, connection_name: draft.connection_name, role: draft.role, topic: draft.topic, payload_kind: draft.payload_kind }
  if (draft.role !== 'sink') { requireValue(draft.consumer_group.trim(), 'source/both 需要消费组'); payload.consumer_group = draft.consumer_group }
  if (draft.role !== 'sink' && draft.payload_kind === 'structured' && draft.source_enabled) {
    const source = draft.source, names = new Set(), fieldName = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/
    requireValue(fieldName.test(source.entity), '请填写合法实体名称')
    requireValue(Array.isArray(source.fields) && source.fields.length > 0 && source.fields.length <= 128, '请配置 1～128 个字段')
    const fields = source.fields.map((field) => {
      requireValue(fieldName.test(field.name) && !field.name.startsWith('__kafka_') && !names.has(field.name), '字段名称重复、非法或使用了保留前缀')
      names.add(field.name)
      requireValue(typeof field.path === 'string' && field.path.startsWith('/') && field.path.length <= 1024 && !/~(?:[^01]|$)/.test(field.path), '字段路径需要有效 JSON Pointer')
      requireValue(['bool', 'int64', 'uint64', 'float64', 'utf8'].includes(field.type) && typeof field.nullable === 'boolean', '字段类型或空值配置错误')
      requireValue(typeof field.unit === 'string' && field.unit.length <= 64, '字段单位长度不能超过 64')
      requireValue(!field.unit.startsWith('unix_') || (field.type === 'int64' && ['unix_s', 'unix_ms', 'unix_us', 'unix_ns'].includes(field.unit)), 'Unix 时间字段需要 int64 和明确单位')
      return { name: field.name, path: field.path, type: field.type, nullable: field.nullable, ...(field.unit ? { unit: field.unit } : {}) }
    })
    requireValue(['earliest', 'latest', 'error'].includes(source.offset_reset) && ['after_success', 'none'].includes(source.commit_policy), '请选择合法位点策略')
    requireValue(Number.isInteger(source.max_batch_rows) && source.max_batch_rows >= 1 && source.max_batch_rows <= 4096, '批次行数范围为 1～4096')
    requireValue(Number.isInteger(source.max_message_bytes) && source.max_message_bytes >= 1 && source.max_message_bytes <= 16777216, '单消息上限范围为 1～16777216 字节')
    payload.payload_options = { source: { encoding: 'json', entity: source.entity, fields, offset_reset: source.offset_reset, commit_policy: source.commit_policy, max_batch_rows: source.max_batch_rows, max_message_bytes: source.max_message_bytes } }
  }
  if (revision) payload.expected_revision = revision
  return payload
}
export const blockedReason = (row, kind, removing = false) => row.in_use ? '活动任务或验证正在使用，结束后再操作'
  : removing && kind === 'connections' && row.references?.length ? `被通道引用：${row.references.join('、')}` : ''
export const kafkaError = (error) => {
  if (error?.response?.status === 409) return '配置已变化、被引用或正在使用，请刷新列表后重新编辑；当前草稿已保留'
  if (error?.response?.status === 429) return '同时最多运行 8 个连接验证，请稍后重试'
  if (!error?.response && error instanceof Error) return error.message
  const code = error?.response?.data?.code
  return typeof code === 'string' && /^[A-Z0-9_]+$/.test(code) ? `操作失败：${code}` : 'Kafka 管理服务暂不可用'
}
export const verificationLabel = (row) => {
  const v = row.verification
  if (!v || v.status === 'unverified') return '尚未验证'
  if (v.status === 'running') return '验证进行中'
  return `${v.status === 'passed' ? '连接验证通过' : '验证失败'} · ${v.stage || ''} · ${v.verified_at ? new Date(v.verified_at).toLocaleString() : ''}`
}
