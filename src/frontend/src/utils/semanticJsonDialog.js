import { prettyJson, downloadJson } from './dataSemantics.js'
import { jsonTokens } from './semanticReading.js'

let active = null
const styles = `
dialog.semantic-json-window { box-sizing:border-box; width:min(1100px,92vw); height:84vh; max-width:100vw; max-height:100vh; margin:auto; padding:0; border:1px solid #364152; border-radius:12px; background:#111827; color:#e5e7eb; color-scheme:dark; box-shadow:0 24px 80px #0008; font:14px/1.6 system-ui,sans-serif; overflow:hidden; }
dialog.semantic-json-window::backdrop { background:#080f1db3; backdrop-filter:blur(3px); }
dialog.semantic-json-window[data-maximized] { width:100vw; height:100vh; border-radius:0; }
.semantic-json-window .json-frame { height:100%; display:flex; flex-direction:column; }
.semantic-json-window .json-heading { display:flex; align-items:center; gap:12px; padding:16px 20px; border-bottom:1px solid #364152; }
.semantic-json-window h2 { flex:1; margin:0; min-width:0; font-size:17px; font-weight:600; color:#f3f4f6; }
.semantic-json-window .json-badge { color:#6ee7b7; font:600 12px ui-monospace,monospace; }
.semantic-json-window .json-toolbar { display:flex; align-items:center; flex-wrap:wrap; gap:8px; padding:10px 20px; background:#182132; border-bottom:1px solid #364152; }
.semantic-json-window button,.semantic-json-window a { box-sizing:border-box; font:inherit; font-size:13px; line-height:1.5; color:#dbeafe; background:#233047; border:1px solid #48566e; border-radius:6px; padding:5px 11px; cursor:pointer; text-decoration:none; }
.semantic-json-window button:hover,.semantic-json-window a:hover { background:#31405b; color:white; }
.semantic-json-window button:focus-visible,.semantic-json-window a:focus-visible,.semantic-json-window .json-code:focus-visible { outline:2px solid #60a5fa; outline-offset:2px; }
.semantic-json-window button:disabled { opacity:.5; cursor:wait; }
.semantic-json-window .json-feedback { flex:1; color:#9ca3af; font-size:12px; text-align:right; }
.semantic-json-window .json-content { min-height:0; flex:1; overflow:auto; display:grid; grid-template-columns:max-content 1fr; align-items:start; padding:16px 0; overscroll-behavior:contain; tab-size:2; }
.semantic-json-window pre { margin:0; font:13px/1.8 ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace; white-space:pre; overflow:visible; max-height:none; }
.semantic-json-window .json-lines { padding:0 14px; color:#68768e; text-align:right; user-select:none; position:sticky; left:0; background:#111827; }
.semantic-json-window .json-code { padding:0 20px 0 8px; color:#cbd5e1; }
.semantic-json-window .json-key { color:#93c5fd; }
.semantic-json-window .json-string { color:#a7f3d0; }
.semantic-json-window .json-number { color:#fdba74; }
.semantic-json-window .json-boolean { color:#c4b5fd; }
.semantic-json-window .json-null { color:#f9a8d4; }
@media(max-width:600px) { dialog.semantic-json-window { width:96vw; height:92vh; } .semantic-json-window .json-heading,.semantic-json-window .json-toolbar { padding:10px; } .semantic-json-window .json-lines { padding:0 8px; } }
`

export function showJsonReading({ title, value, links = [], resolve = null, onClose = () => {} }) {
  active?.close()
  if (!document.getElementById('semantic-json-styles')) {
    const style = document.createElement('style'); style.id = 'semantic-json-styles'; style.textContent = styles; document.head.append(style)
  }
  const trigger = document.activeElement
  const dialog = document.createElement('dialog'); dialog.className = 'semantic-json-window'; dialog.setAttribute('aria-label', title)
  dialog.dataset.testid = 'semantic-json-dialog'
  const element = (tag, className, text) => { const node = document.createElement(tag); node.className = className; if (text !== undefined) node.textContent = text; return node }
  const button = (label, action) => { const node = element('button', '', label); node.type = 'button'; node.addEventListener('click', action); return node }
  const frame = element('div', 'json-frame'), heading = element('header', 'json-heading'), toolbar = element('nav', 'json-toolbar')
  const content = element('div', 'json-content'), numbers = element('pre', 'json-lines'), code = element('pre', 'json-code')
  numbers.setAttribute('aria-hidden', 'true'); code.tabIndex = 0; code.setAttribute('aria-label', '说明 JSON'); code.dataset.testid = 'semantic-json-content'
  const feedback = element('span', 'json-feedback', 'JSON · 只读'); feedback.setAttribute('role', 'status')
  let current = value, closed = false
  function render() {
    const fragment = document.createDocumentFragment()
    for (const token of jsonTokens(current)) fragment.append(token.kind === 'space' || token.kind === 'punctuation' ? document.createTextNode(token.text) : element('span', 'json-' + token.kind, token.text))
    code.replaceChildren(fragment)
    numbers.textContent = Array.from({ length: prettyJson(current).split('\n').length }, (_, i) => i + 1).join('\n')
  }
  const maximize = button('最大化', () => { const full = dialog.toggleAttribute('data-maximized'); maximize.textContent = full ? '还原' : '最大化' })
  const close = () => { if (!closed) dialog.close() }
  heading.append(element('span', 'json-badge', '{ } JSON'), element('h2', '', title), maximize, button('关闭', close))
  toolbar.append(button('复制 JSON', async () => {
    try { await navigator.clipboard.writeText(prettyJson(current)); if (!closed) feedback.textContent = '已复制完整 JSON' }
    catch (error) { if (!closed) feedback.textContent = '复制失败，请选择正文手动复制：' + error.message }
  }), button('下载 JSON', () => downloadJson(title, prettyJson(current))))
  for (const link of links) { const anchor = element('a', '', link.label); anchor.href = link.href; anchor.target = '_top'; anchor.dataset.currentObject = ''; toolbar.append(anchor) }
  if (resolve) {
    const load = button(resolve.label, async () => {
      load.disabled = true
      try { const exact = await resolve.load(); if (!closed) { current = { ...value, exact_reading: exact }; render() } }
      catch (error) { if (!closed) { current = { ...value, exact_reading_error: { message: error.message } }; render() } }
      finally { if (!closed) load.disabled = false }
    })
    toolbar.append(load)
  }
  toolbar.append(feedback); content.append(numbers, code); frame.append(heading, toolbar, content); dialog.append(frame)
  const handle = { close }
  dialog.addEventListener('keydown', event => { if (event.key === 'Escape') event.stopPropagation() })
  dialog.addEventListener('close', () => { closed = true; dialog.remove(); if (active === handle) active = null; if (trigger?.isConnected) trigger.focus(); onClose() }, { once: true })
  dialog.addEventListener('click', event => { if (event.target === dialog) { const rect = dialog.getBoundingClientRect(); if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) close() } })
  render(); document.body.append(dialog); active = handle; dialog.showModal(); heading.querySelector('button').focus()
  return handle
}
