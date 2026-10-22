import { spawn } from 'node:child_process'
import { mkdtemp, readdir, writeFile } from 'node:fs/promises'
import { tmpdir, homedir } from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'

const [direct, host, unconfigured, report, unreachable] = process.argv.slice(2)
const checks = []
const pause = (ms) => new Promise((r) => setTimeout(r, ms))
const wait = async (fn, label) => {
  const end = Date.now() + 18000
  while (Date.now() < end) { try { const value = await fn(); if (value) return value } catch {} await pause(100) }
  throw new Error('Timeout: ' + label)
}
let browser, socket
try {
  const cache = path.join(homedir(), '.cache/ms-playwright')
  const installed = (await readdir(cache)).filter((p) => p.startsWith('chromium_headless_shell-')).sort().at(-1)
  const executable = process.env.FLOWSQL_TEST_CHROMIUM || path.join(cache, installed || 'missing', 'chrome-headless-shell-linux64/chrome-headless-shell')
  browser = spawn(executable, ['--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--remote-debugging-port=0',
    '--user-data-dir=' + await mkdtemp(path.join(tmpdir(), 'flowsql-exploration-browser-')), 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] })
  let logs = ''; browser.stderr.on('data', (d) => { logs += d }); browser.on('error', (e) => { logs += e.message })
  await wait(() => logs.includes('DevTools listening on'), 'Chromium startup: ' + executable)
  const endpoint = logs.match(/DevTools listening on (ws:\/\/\S+)/)[1]
  const pages = await (await fetch(endpoint.replace('ws://', 'http://').split('/devtools/')[0] + '/json/list')).json()
  socket = new WebSocket(pages.find((p) => p.type === 'page').webSocketDebuggerUrl)
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject })
  let next = 0; const pending = new Map(); const errors = []; const requests = []
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data)
    if (message.method === 'Runtime.exceptionThrown') errors.push(message.params.exceptionDetails)
    if (message.method === 'Network.requestWillBeSent') requests.push({ url: message.params.request.url, document: message.params.documentURL })
    const call = pending.get(message.id)
    if (call) { pending.delete(message.id); message.error ? call.reject(Error(JSON.stringify(message.error))) : call.resolve(message.result) }
  }
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++next; pending.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params }))
  })
  const evaluate = async (expression) => {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails))
    return result.result.value
  }
  const navigate = async (url) => { await send('Page.navigate', { url }); await pause(150) }
  const child = (expression) => `(()=>{const w=document.querySelector('iframe')?.contentWindow;return w && (${expression})})()`
  const check = (name) => { checks.push(name); console.log('PASS', name) }
  await send('Runtime.enable'); await send('Page.enable'); await send('Network.enable')
  await navigate(direct + '/exploration/')
  await wait(() => evaluate('document.querySelector(".source-status") && document.body.textContent.includes("数据来源")'), 'real Graphic Walker direct render')
  await wait(() => evaluate('document.querySelector(".empty-state")?.textContent.includes("选择数据表")'), 'exploration empty state')
  assert.equal(await evaluate('!!document.querySelector("header")'), true)
  check('standalone_graphic_walker')
  await navigate(host + '/explore')
  await wait(() => evaluate(child('w.document.querySelector(".source-status") && w.document.documentElement.dataset.theme === "light"')), 'Vue iframe handshake')
  assert.equal(await evaluate(child('!!w.document.querySelector("header")')), false)
  assert.equal(await evaluate('document.querySelectorAll("iframe").length'), 1)
  assert.equal(await evaluate('document.querySelector("iframe").clientHeight === document.querySelector(".app-main").clientHeight'), true)
  check('vue_same_origin_iframe_resources')
  await evaluate('document.querySelector("button[title=切换主题]").click()')
  await wait(() => evaluate(child('w.document.documentElement.dataset.theme === "dark"')), 'dark theme')
  check('theme_context')
  await evaluate(child('(w.sessionMarker="preserved")'))
  await evaluate('[...document.querySelectorAll(".el-menu-item")].find(e=>e.textContent==="通道管理").click()')
  await wait(() => evaluate(child('w.document.body.dataset.visible === "false"')), 'hidden notification')
  await evaluate('[...document.querySelectorAll(".el-menu-item")].find(e=>e.textContent==="数据探索").click()')
  await wait(() => evaluate(child('w.document.body.dataset.visible === "true"')), 'visible notification')
  assert.equal(await evaluate(child('w.sessionMarker')), 'preserved')
  check('single_iframe_preserves_state_and_visibility')
  const missing = await fetch(host + '/exploration/missing')
  assert.equal(missing.status, 404); assert.equal((await missing.text()).includes('<html'), false)
  assert.equal((await fetch(host + '/api/visualization/v1/capabilities?probe=1')).status, 200)
  const html = await fetch(host + '/exploration/')
  const head = await fetch(host + '/exploration/', { method: 'HEAD' })
  assert.equal(head.headers.get('content-length'), html.headers.get('content-length'))
  assert.match(head.headers.get('content-type'), /text\/html/)
  assert.equal(head.headers.get('cache-control'), 'no-store')
  assert.equal((await fetch(host + '/api/visualization/v1/compute', { method: 'DELETE' })).status, 405)
  check('proxy_paths_methods_query_and_404')
  await evaluate('window.postMessage({version:2,type:"ready",payload:{}},location.origin)')
  await pause(100)
  assert.equal(await evaluate('!!document.querySelector(".exploration-error")'), false)
  const tree = await send('Page.getFrameTree')
  const childContext = await send('Page.createIsolatedWorld', { frameId: tree.frameTree.childFrames[0].frame.id, worldName: 'bridge-contract-check' })
  await send('Runtime.evaluate', { contextId: childContext.executionContextId,
    expression: 'parent.postMessage({version:2,type:"ready",payload:{}},location.origin)' })
  await wait(() => evaluate('document.querySelector(".exploration-error")?.textContent.includes("版本不兼容")'), 'bridge version rejection')
  check('bridge_origin_source_and_version')
  await navigate(unconfigured + '/explore')
  await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.includes("未配置数据探索服务")'), 'unconfigured message')
  check('unconfigured_service_message')
  await navigate(unreachable + '/explore')
  await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.includes("暂不可用")'), 'unreachable service message')
  assert.equal((await fetch(unreachable + '/api/version')).status, 200)
  check('unreachable_service_isolation')
  assert.deepEqual(errors, [])
  assert.deepEqual(requests.filter(({ url, document }) => document.includes('/exploration/') && /^https?:/.test(url)
    && ![direct, host, unconfigured, unreachable].some((origin) => url.startsWith(origin + '/'))), [])
  check('no_uncaught_browser_errors')
} catch (error) {
  console.error(error)
  process.exitCode = 1
} finally {
  if (socket) socket.close()
  if (browser && browser.exitCode === null && browser.signalCode === null) {
    const ended = new Promise((r) => browser.once('exit', r)); browser.kill('SIGTERM'); await ended
  }
  await writeFile(report, JSON.stringify({ selected: 9, executed: checks.length + (process.exitCode ? 1 : 0), passed: checks.length,
    failed: process.exitCode ? 1 : 0, skipped: 0, not_executed: 9 - checks.length - (process.exitCode ? 1 : 0), checks }, null, 2))
}
