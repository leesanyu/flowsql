const declared = semantic => semantic?.state === 'declared' ? semantic.value : undefined
const units = ['s', 'ms', 'us', 'ns']
// Recognize only the two explicit Unix baselines already used by our contracts.
const unixEpoch = value => ['1970-01-01T00:00:00Z', 'Unix epoch'].includes(value)

export function timeEditorConfig(value, modes = []) {
  if (!value) return {}
  const unit = declared(value.unit)?.code, timezone = declared(value.timezone)
  if (value.kind === 'duration' && units.includes(unit)) return { mode: 'duration', unit }
  const instant = { ...(unit ? { unit } : {}), ...(timezone ? { timezone } : {}) }
  if (value.kind === 'instant' && modes.includes('datetime')) return { mode: 'datetime', ...instant }
  if (value.kind === 'instant' && unixEpoch(declared(value.epoch)) && units.includes(unit)) return { mode: 'unix_timestamp', ...instant }
  return { unsupported: true }
}

export function timeDescription(value, definition) {
  const text = declared(value?.description) || ''
  const normalize = text => text.replace(/\s+/g, '')
  return normalize(definition || '').includes(normalize(text)) ? '' : text
}

export function timeSummary(value) {
  const parts = [value.kind === 'instant' ? '时间点' : value.kind === 'duration' ? '时长' : '未支持的时间种类']
  const unit = declared(value.unit)?.code, epoch = declared(value.epoch), timezone = declared(value.timezone)
  if (unit) parts.push(`单位：${unit}`)
  if (epoch) parts.push(`基准：${epoch}`)
  if (timezone) parts.push(`时区：${timezone}`)
  return parts.join('；')
}
