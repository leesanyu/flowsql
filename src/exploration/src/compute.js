import { contract } from './bridge.js'

export async function api(path, options = {}) {
  const response = await fetch('/api/visualization/v1/' + path, options)
  const body = await response.json()
  if (!response.ok) throw Object.assign(new Error(body.message || '数据探索请求失败'), { code: body.code })
  return body
}

export function computationClient(source, notify, fail) {
  const pending = new Map()
  let enabled = true
  let epoch = 0
  let latest = 0
  const cancel = () => {
    ++epoch
    for (const [id, controller] of pending) {
      fetch('/api/visualization/v1/cancel', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ request_id: id }), keepalive: true }).catch(() => {})
      controller.abort()
    }
    pending.clear()
  }
  return {
    setVisible(visible) { enabled = visible; if (!visible) cancel() },
    cancel,
    async compute(workflow) {
      if (!enabled) throw new Error('工作区已隐藏，查询已暂停')
      const generation = epoch
      const sequence = ++latest
      const id = crypto.randomUUID()
      const controller = new AbortController()
      pending.set(id, controller)
      try {
        const result = await api('compute', { method: 'POST', headers: { 'Content-Type': 'application/json' }, signal: controller.signal,
          body: JSON.stringify({ request_id: id, source_id: source.source_id, schema_revision: source.schema_revision,
            ...(source.snapshot_id ? { snapshot_id: source.snapshot_id } : {}),
            workflow_version: contract.workflow_version, workflow, detail_mode: source.detail_mode || 'complete' }) })
        if (generation !== epoch || !enabled) throw new DOMException('查询已被替代', 'AbortError')
        if (sequence === latest) notify(result)
        return result.rows
      } catch (error) {
        if (error.name !== 'AbortError' && generation === epoch && sequence === latest) {
          if (error.code === 'SNAPSHOT_EXPIRED' && source.on_expired) source.on_expired()
          fail(error.message)
        }
        throw error
      } finally { pending.delete(id) }
    }
  }
}
