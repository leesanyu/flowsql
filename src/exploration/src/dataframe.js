import { api } from './compute.js'

// Snapshot lifecycle lives outside the chart: acquire binds one immutable version to the page.
export function snapshotClient(onExpired) {
  const snapshots = new Map()
  const call = (path, body) => api(path, { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body) })
  return {
    async acquire(source_id) {
      const snapshot = await call('snapshots/acquire', { request_id: crypto.randomUUID(), source_id })
      snapshots.set(snapshot.snapshot_id, snapshot)
      return snapshot
    },
    async release(snapshot_id) {
      if (!snapshot_id) return
      snapshots.delete(snapshot_id)
      // Release is idempotent; a failure must not block closing or refreshing the page.
      await call('snapshots/release', { snapshot_id }).catch(() => {})
    },
    async renew(snapshot_id) {
      try {
        const renewed = await call('snapshots/renew', { snapshot_id })
        const snapshot = snapshots.get(snapshot_id)
        if (snapshot) { snapshot.expires_at_unix = renewed.expires_at_unix; snapshots.set(snapshot_id, snapshot) }
        return renewed.expires_at_unix
      } catch (error) {
        if (error.code === 'SNAPSHOT_EXPIRED') onExpired(snapshot_id)
        throw error
      }
    },
    expiresAt(snapshot_id) { return snapshots.get(snapshot_id)?.expires_at_unix },
    close() { snapshots.clear() }
  }
}

export function snapshotLabel(snapshot) {
  const captured = new Date(snapshot.captured_at)
  const time = Number.isNaN(captured.getTime()) ? snapshot.captured_at : captured.toLocaleString()
  return { captured: time, rows: snapshot.row_count, revision: snapshot.schema_revision }
}
