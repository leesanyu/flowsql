export const contract = Object.freeze({ bridge_version: 1, api_version: 1, workflow_version: 'graphic-walker-0.5.3' })

export function connectBridge(update) {
  const embedded = window.parent !== window && new URLSearchParams(location.search).get('embed') === '1'
  const send = (type, payload) => {
    if (embedded) parent.postMessage({ version: 1, type, payload }, location.origin)
  }
  const receive = (event) => {
    if (event.origin !== location.origin || event.source !== parent) return
    const { version, type, payload } = event.data || {}
    if (version !== 1 || !payload || !['context', 'visibility'].includes(type)) return
    if (typeof payload.visible !== 'boolean') return
    update(payload)
  }
  if (embedded) window.addEventListener('message', receive)
  return { embedded, send, close: () => window.removeEventListener('message', receive) }
}
