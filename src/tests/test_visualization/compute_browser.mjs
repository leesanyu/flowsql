import { spawn } from 'node:child_process'
import { mkdtemp, readdir, writeFile } from 'node:fs/promises'
import { tmpdir, homedir } from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'

const [base, report, mode, sourceLabel] = process.argv.slice(2)
const checks = [], workflows = [], results = [], resultWorkflows = []
const pause = (ms) => new Promise((r) => setTimeout(r, ms))
const wait = async (fn, label) => {
  const end = Date.now() + 18000
  while (Date.now() < end) { try { const value = await fn(); if (value) return value } catch {} await pause(100) }
  throw new Error('Timeout: ' + label)
}
let browser, socket, failure, evaluatePage
const exceptions = [], completedRequests = []
try {
  const cache = path.join(homedir(), '.cache/ms-playwright')
  const installed = (await readdir(cache)).filter((p) => p.startsWith('chromium_headless_shell-')).sort().at(-1)
  const executable = process.env.FLOWSQL_TEST_CHROMIUM || path.join(cache, installed || 'missing', 'chrome-headless-shell-linux64/chrome-headless-shell')
  browser = spawn(executable, ['--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--remote-debugging-port=0',
    '--user-data-dir=' + await mkdtemp(path.join(tmpdir(), 'flowsql-compute-browser-')), 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] })
  let logs = ''; browser.stderr.on('data', (d) => { logs += d }); browser.on('error', (e) => { logs += e.message })
  await wait(() => logs.includes('DevTools listening on'), 'Chromium')
  const endpoint = logs.match(/DevTools listening on (ws:\/\/\S+)/)[1]
  const pages = await (await fetch(endpoint.replace('ws://', 'http://').split('/devtools/')[0] + '/json/list')).json()
  socket = new WebSocket(pages.find((p) => p.type === 'page').webSocketDebuggerUrl)
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject })
  let next = 0; const pending = new Map(), requests = new Map()
  socket.onmessage = (event) => {
    const message = JSON.parse(event.data)
    if (message.method === 'Runtime.exceptionThrown') exceptions.push(message.params.exceptionDetails)
    if (message.method === 'Network.requestWillBeSent' && message.params.request.url.endsWith('/compute')) {
      const data = JSON.parse(message.params.request.postData)
      workflows.push(data.workflow); requests.set(message.params.requestId, data)
    }
    const call = pending.get(message.id)
    if (call) { pending.delete(message.id); message.error ? call.reject(Error(JSON.stringify(message.error))) : call.resolve(message.result) }
  }
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++next; pending.set(id, { resolve, reject }); socket.send(JSON.stringify({ id, method, params }))
  })
  const evaluateRoot = async (expression) => {
    const result = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails))
    return result.result.value
  }
  const evaluate = (expression) => evaluateRoot(mode === 'embedded'
    ? `document.querySelector('iframe').contentWindow.eval(${JSON.stringify(expression)})` : expression)
  evaluatePage = evaluate
  const select = (label, value) => evaluate(`(()=>{const e=document.querySelector('select[aria-label="${label}"]');e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('change',{bubbles:true}));return true})()`)
  const draw = async (name) => {
    await evaluate('[...document.querySelectorAll("button")].find(e=>e.textContent==="绘图").click()')
    await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), name)
    await wait(() => evaluate('[...document.querySelectorAll("*")].some(e=>e.shadowRoot?.querySelector("canvas,svg.marks"))'), name + ' rendered')
    assert.equal(await evaluate('!!document.querySelector("[role=alert]")'), false)
    completedRequests.push([...requests.values()].at(-1))
    checks.push(name); console.log('PASS', name)
  }
  await send('Network.enable'); await send('Page.enable'); await send('Runtime.enable')
  await send('Page.navigate', { url: base + (mode === 'embedded' ? '/explore' : '/exploration/') })
  if (mode === 'embedded') await wait(() => evaluateRoot('!!document.querySelector("iframe")?.contentWindow.document.querySelector("select")'), 'embedded workspace')
  await wait(() => evaluate(`document.querySelector('select[aria-label="数据来源"]')?.options.length >= 2`), 'database source discovery')
  await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>${sourceLabel ? `o.textContent === ${JSON.stringify(sourceLabel)}` : "o.value"}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
  await wait(() => evaluate('!!document.querySelector(".quick-controls")'), 'source metadata')
  await select('横轴', 'f0'); await select('纵轴', 'f2'); await select('图表', 'bar'); await select('聚合', 'mean')
  await draw('native_bar_mean')
  await evaluate('[...document.querySelectorAll("button")].find(e=>e.textContent==="更多设置").click()')
  await select('横轴', 'f1'); await select('图表', 'line')
  await evaluate(`document.querySelector('input[aria-label="UTC 时间横轴"]').click()`)
  await draw('native_line_utc_text')
  await evaluate(`document.querySelector('input[aria-label="UTC 时间横轴"]').click()`)
  await select('横轴', 'f3'); await select('图表', 'point')
  await draw('native_scatter_projection')
  await select('横轴', 'f0'); await select('图表', 'bar'); await select('过滤字段', 'f0')
  await evaluate('(()=>{const e=document.querySelector("input[aria-label=过滤值]");Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,"value").set.call(e,"alpha");e.dispatchEvent(new Event("input",{bubbles:true}))})()')
  await draw('native_filter_then_aggregate')
  assert(workflows.length >= 4)
  assert(workflows.some((w) => w.workflow.some((s) => s.type === 'filter')))
  assert(workflows.some((w) => w.workflow.some((s) => s.type === 'view' && s.query.some((q) => q.op === 'raw'))))
  checks.push('real_graphic_walker_workflows')
  for (const data of completedRequests) {
    const response = await fetch(base + '/api/visualization/v1/compute', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...data, request_id: crypto.randomUUID() }) })
    const result = await response.json()
    assert.equal(response.status, 200, JSON.stringify(result)); assert.equal(result.execution, 'database'); results.push(result); resultWorkflows.push(data.workflow)
  }
  checks.push('real_workflows_native_results')
  if (mode) {
    await select('纵轴', 'f5')
    await evaluate('[...document.querySelectorAll("button")].find(e=>e.textContent==="绘图").click()')
    await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.match(/混合存储类型|安全数值范围|Decimal/)'), 'unsupported type visible')
    checks.push('unsupported_type_error_visible')
  }
} catch (error) { failure = String(error.stack || error); console.error(error); if (evaluatePage) console.error(await evaluatePage('document.body.innerText')); console.error(JSON.stringify(exceptions)); process.exitCode = 1 }
finally {
  if (socket) socket.close()
  if (browser && browser.exitCode === null && browser.signalCode === null) {
    const ended = new Promise((r) => browser.once('exit', r)); browser.kill('SIGTERM'); await ended
  }
  const selected = mode ? 7 : 6
  await writeFile(report, JSON.stringify({ selected, executed: checks.length + (failure ? 1 : 0), passed: checks.length, failed: failure ? 1 : 0,
    skipped: 0, not_executed: selected - checks.length - (failure ? 1 : 0), checks, workflows: completedRequests.map((r) => r.workflow), resultWorkflows, results, error: failure }, null, 2))
}
