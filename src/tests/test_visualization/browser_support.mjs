import { spawn } from 'node:child_process'
import { mkdtemp, readdir, writeFile } from 'node:fs/promises'
import { tmpdir, homedir } from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'


const pause = (ms) => new Promise((r) => setTimeout(r, ms))
const wait = async (fn, label) => {
  const end = Date.now() + 18000
  while (Date.now() < end) { try { const value = await fn(); if (value) return value } catch {} await pause(100) }
  throw new Error('Timeout: ' + label)
}
export async function openBrowser(base, mode = 'standalone') {
  let browser, socket
  const exceptions = [], workflows = []
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
      workflows.push(data)
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
  const select = (label, value) => evaluate(`(()=>{const e=document.querySelector('select[aria-label="${label}"]');e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('change',{bubbles:true}));return true})()`)
  const draw = async (name) => {
    await evaluate('[...document.querySelectorAll("button")].find(e=>e.textContent==="绘图").click()')
    await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), name)
    await wait(() => evaluate('[...document.querySelectorAll("*")].some(e=>e.shadowRoot?.querySelector("canvas,svg.marks"))'), name + ' rendered')
    assert.equal(await evaluate('!!document.querySelector("[role=alert]")'), false)
    console.log('PASS', name)
  }
  await send('Network.enable'); await send('Page.enable'); await send('Runtime.enable')
  return { send, evaluate, evaluateRoot, select, draw, wait, pause, workflows, exceptions,
    async close() {
      socket.close()
      if (browser.exitCode === null && browser.signalCode === null) {
        const ended = new Promise((r) => browser.once('exit', r)); browser.kill('SIGTERM'); await ended
      }
    }
  }
}

