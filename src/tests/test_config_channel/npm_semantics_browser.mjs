import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence] = process.argv.slice(2)
const browser = await openBrowser(base)
const { send, evaluate: ev, wait, pause } = browser
const body = () => ev('document.body.innerText')
const text = value => wait(async () => (await body()).includes(value), value)
const find = label => `[...document.querySelectorAll('button,[role=tab],.channel-type-item,.browser-table-item')].find(e=>(e.textContent.trim()===${JSON.stringify(label)} || (e.classList.contains('channel-type-item') && e.textContent.trim().startsWith(${JSON.stringify(label)}))) && e.getBoundingClientRect().width>0)`
async function mouse(expression) {
  await ev(`(${expression}).scrollIntoView({block:'center',inline:'nearest'})`)
  await pause(150)
  const r = await ev(`(()=>{const e=${expression},r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2,h=document.elementFromPoint(x,y);return {x,y,hit:e===h||e.contains(h)}})()`)
  assert.ok(r.hit, JSON.stringify(r))
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: r.x, y: r.y, button: 'left', clickCount: 1 })
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: r.x, y: r.y, button: 'left', clickCount: 1 })
  await pause(200)
}
const click = async label => { await wait(() => ev(`!!(${find(label)})`), label); await mouse(find(label)) }
const set = (label, value) => ev(`(()=>{const e=document.querySelector(${JSON.stringify('[aria-label="' + label + '"]')});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
const api = async (operation, payload) => {
  const result = await fetch(base + '/api/semantics/v1/' + operation, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
  const value = await result.json(); assert.equal(result.status, 200, JSON.stringify(value)); return value.data
}
async function checkEditors(document) {
  const groups = await ev(`(()=>{const p=document.querySelector('[data-testid=channel-description]');return [...p.querySelectorAll('.description-editor')].map(e=>{const r=e.getBoundingClientRect();return {label:e.querySelector('textarea').getAttribute('aria-label'),notes:[...e.querySelectorAll('.supplement')].map(n=>n.textContent),bordered:parseFloat(getComputedStyle(e).borderTopWidth)>0&&parseFloat(getComputedStyle(e).borderBottomWidth)>0,contained:[...e.querySelectorAll('textarea,.supplement')].every(n=>{const b=n.getBoundingClientRect();return b.x>=r.x&&b.right<=r.right+1&&b.y>=r.y&&b.bottom<=r.bottom+1})}})})()`)
  const expected = [
    ['对象说明', document.entity, ['limitations'], ['使用局限']],
    ['一行表示什么', document.entity, ['row_semantics'], ['行语义']],
    ...document.fields.map(f => {
      const meaning = f.time_semantics?.state === 'declared' ? f.time_semantics.value.description : null
      const extra = meaning?.state === 'declared' && !String(f.definition.value || '').replace(/\s+/g, '').includes(meaning.value.replace(/\s+/g, ''))
      return [f.name + ' 字段说明', { ...f, time_meaning: { state: extra ? 'declared' : 'unknown' } }, ['time_meaning', 'value_domain', 'aggregation'], ['时间含义', '取值域', '聚合规则']]
    })
  ]
  assert.equal(groups.length, expected.length)
  for (const [label, object, keys, labels] of expected) {
    const group = groups.find(g => g.label === label)
    assert.ok(group?.bordered && group.contained, JSON.stringify(group))
    assert.deepEqual(group.notes.map(n => n.split('：')[0]), labels.filter((_, i) => object[keys[i]].state === 'declared'), label)
  }
  assert.deepEqual(await ev(`(()=>{const p=document.querySelector('[data-testid=channel-description]');return [...p.querySelectorAll('.supplement')].filter(n=>!n.closest('.description-editor')&&/^(使用局限|行语义|取值域|聚合规则)：/.test(n.textContent)).map(n=>n.textContent)})()`), [])
}
async function checkKnownTimes(names = ['observed_at', 'query_at_ns', 'response_at_ns']) {
  for (const name of names) {
    assert.equal(await ev(`document.querySelector('[aria-label="${name} 时间类型"]').value`), 'unix_timestamp')
    assert.equal(await ev(`document.querySelector('[aria-label="${name} 时间单位"]').value`), 'ns')
    assert.equal(await ev(`document.querySelector('[data-description-field="${name}"]').nextElementSibling.querySelector('.field-time').innerText`), '基准：Unix epoch · 时区：UTC')
  }
  assert.equal(await ev(`document.querySelector('[aria-label="latency_ns 时间类型"]').value`), 'duration')
  assert.equal(await ev(`document.querySelector('[aria-label="latency_ns 时间单位"]').value`), 'ns')
  assert.equal(await ev(`document.querySelector('[data-description-field="latency_ns"]').nextElementSibling.querySelector('.field-time').innerText`), '')
  assert.equal(await ev(`document.querySelector('[aria-label="entity_instance_id 时间类型"]').value`), '')
  assert.equal(await ev(`!!document.querySelector('option[value="__preserved__"]')`), false)
  assert.equal((await body()).includes('已有时间表示'), false)
}
try {
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false })
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__requests=[];const f=window.fetch;window.fetch=function(u,o){window.__requests.push({url:String(u),body:o?.body});return f.apply(this,arguments)};const x=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){this.__url=u;return x.apply(this,arguments)};const s=XMLHttpRequest.prototype.send;XMLHttpRequest.prototype.send=function(body){window.__requests.push({url:this.__url,body});return s.apply(this,arguments)}` })
  await send('Page.navigate', { url: base + '/channels' })
  await text('数据库通道'); await click('数据库通道'); await text('example'); await click('浏览')
  await text('npm_dns_transaction_ipv4_final_v1'); await click('npm_dns_transaction_ipv4_final_v1')
  await wait(() => ev('!!document.querySelector("[aria-label=对象说明]:enabled")'), 'editable NPM description')
  assert.deepEqual(await ev('[...document.querySelectorAll("[data-testid=channel-description] .fields thead th")].map(e=>(e.querySelector("span")||e).textContent.trim())'), ['字段 / 类型', '单位', '时间', '关联'])
  assert.equal(await ev('document.querySelector("[aria-label=一行表示什么]").value.includes("DNS")'), true)
  const locator = { kind: 'database_table', type: 'sqlite', name: 'example', table: 'npm_dns_transaction_ipv4_final_v1' }
  let before = await api('describe', { target: { kind: 'channel', locator } })
  const fresh = structuredClone(before)
  for (const name of ['observed_at', 'query_at_ns', 'response_at_ns'])
    assert.equal(fresh.description.document.fields.find(f => f.name === name).time_semantics.value.epoch.value, '1970-01-01T00:00:00Z')
  const definition = before.description.document.fields.find(f => f.name === 'latency_ns').definition.value
  assert.equal(await ev(`document.querySelector('[aria-label="latency_ns 字段说明"]').value`), definition)
  await checkEditors(before.description.document)
  await checkKnownTimes()
  assert.equal(await ev(`document.querySelector('[aria-label="observed_at 字段说明"]').closest('.description-editor').querySelector('.supplement')?.textContent?.startsWith('时间含义：') || false`), false, 'time meaning already in field body is not repeated')
  const page = await body()
  for (const value of ['NULL', 'run_id', 'RCODE', '采集截断']) assert.ok(page.includes(value), value)
  assert.equal(await ev('document.documentElement.scrollWidth>innerWidth+1'), false)
  const reads = await ev('window.__requests')
  assert.equal(reads.filter(r => /database\/preview|batch\/execute|stream\/execute/.test(r.url)).length, 0)
  assert.equal(reads.filter(r => /semantics\/v1\/(save|channels\/save-description)$/.test(r.url)).length, 0)
  await writeFile(evidence + '/browser-read.png', Buffer.from((await send('Page.captureScreenshot', { format: 'png' })).data, 'base64'))
  // Persist an existing spelling and an unsupported imported definition through the real API.
  const oldTime = structuredClone(fresh.description.document.fields.find(f => f.name === 'observed_at').time_semantics)
  oldTime.origin = 'imported'; oldTime.value.epoch.value = 'Unix epoch'
  const unsupported = structuredClone(fresh.description.document.fields.find(f => f.name === 'response_at_ns').time_semantics)
  unsupported.origin = 'imported'; unsupported.value.epoch.value = 'custom epoch'
  unsupported.value.unit.origin = 'imported'
  unsupported.value.unit.value = { code: 'minute', label: '分钟' }
  await api('channels/save-description', { locator, basis: before.basis, changes: { fields: { observed_at: { time_semantics: oldTime }, response_at_ns: { time_semantics: unsupported, unit: unsupported.value.unit } } }, confirmations: before.checks.required_confirmations || [] })
  await mouse("document.querySelector('[data-testid=channel-description] .heading-actions button:last-child')")
  await wait(() => ev(`!!document.querySelector('[data-description-field="response_at_ns"] .field-time [role=status]')`), 'imported time reread')
  await wait(() => ev('!!document.querySelector("[aria-label=对象说明]:enabled")'), 'refreshed time definitions')
  before = await api('describe', { target: { kind: 'channel', locator } })
  assert.equal(before.description.document.fields.find(f => f.name === 'observed_at').time_semantics.value.epoch.value, 'Unix epoch')
  await checkKnownTimes(['observed_at', 'query_at_ns'])
  assert.equal(await ev(`!!document.querySelector('[aria-label="response_at_ns 时间类型"],[aria-label="response_at_ns 单位"],[aria-label="response_at_ns 时间单位"]')`), false)
  assert.equal(await ev(`document.querySelector('[data-description-field="response_at_ns"] .field-time').innerText`), '当前时间定义暂不支持编辑')
  assert.equal(await ev(`document.querySelector('[data-description-field="response_at_ns"]').nextElementSibling.querySelector('details').open`), false)
  await mouse(`document.querySelector('[data-description-field="response_at_ns"]').nextElementSibling.querySelector('summary')`)
  assert.equal(await ev(`document.querySelector('[data-description-field="response_at_ns"]').nextElementSibling.querySelector('details p').textContent`), '时间点；单位：minute；基准：custom epoch；时区：UTC')
  if (fresh.basis.dictionary_ref)
    assert.deepEqual((await api('resolve', { exact_reference: fresh.basis.dictionary_ref })).document, fresh.description.document, 'original precise dictionary remains unchanged')
  await writeFile(evidence + '/browser-imported-time.png', Buffer.from((await send('Page.captureScreenshot', { format: 'png' })).data, 'base64'))
  await set('搜索字段名', 'latency_ns')
  assert.deepEqual(await ev('[...document.querySelectorAll("[data-description-field]")].map(e=>e.dataset.descriptionField)'), ['latency_ns'])
  await set('latency_ns 字段说明', '人工补充：捕获点可配对 DNS 时延，结合运行与有效性解释。')
  await set('对象说明', '人工补充：结合使用局限阅读当前 DNS 结果对象。')
  await set('一行表示什么', '人工补充：一行对应当前运行内的 DNS 事务事件。')
  await mouse("document.querySelector('[data-testid=channel-description] .heading-actions button')")
  await text('说明已保存')
  await wait(() => ev('!!document.querySelector("[aria-label=对象说明]:enabled")'), 'save reread')
  const after = await api('describe', { target: { kind: 'channel', locator } })
  assert.deepEqual((await api('resolve', { exact_reference: before.basis.dictionary_ref })).document, before.description.document, 'saving ordinary descriptions preserves the imported historical dictionary')
  const original = structuredClone(before.description.document), edited = structuredClone(after.description.document)
  original.fields.find(f => f.name === 'latency_ns').definition = edited.fields.find(f => f.name === 'latency_ns').definition
  original.description = edited.description
  original.entity.row_meaning = edited.entity.row_meaning
  assert.deepEqual(edited, original, 'editing local wording must preserve all other field/entity semantics')
  assert.equal(edited.description.value, '人工补充：结合使用局限阅读当前 DNS 结果对象。')
  assert.equal(edited.entity.row_meaning.value, '人工补充：一行对应当前运行内的 DNS 事务事件。')
  assert.equal(edited.fields.find(f => f.name === 'latency_ns').definition.value, '人工补充：捕获点可配对 DNS 时延，结合运行与有效性解释。')
  assert.equal(await ev('document.querySelector("[aria-label=对象说明]").value'), edited.description.value)
  assert.equal(await ev('document.querySelector("[aria-label=一行表示什么]").value'), edited.entity.row_meaning.value)
  await set('搜索字段名', '')
  await checkEditors(edited)
  await checkKnownTimes(['observed_at', 'query_at_ns'])
  assert.deepEqual(edited.fields.find(f => f.name === 'observed_at').time_semantics, oldTime)
  assert.deepEqual(edited.fields.find(f => f.name === 'response_at_ns').time_semantics, unsupported)
  assert.equal(await ev(`document.querySelector('[aria-label="latency_ns 字段说明"]').closest('.description-editor').querySelector('.supplement').textContent`), '时间含义：' + before.description.document.fields.find(f => f.name === 'latency_ns').time_semantics.value.description.value)
  assert.notEqual(after.basis.dictionary_ref, before.basis.dictionary_ref)
  await writeFile(evidence + '/browser-saved.png', Buffer.from((await send('Page.captureScreenshot', { format: 'png' })).data, 'base64'))
  const requests = await ev('window.__requests')
  assert.equal(requests.filter(r => /database\/preview|batch\/execute|stream\/execute/.test(r.url)).length, 0)
  assert.equal(requests.filter(r => /semantics\/v1\/channels\/save-description$/.test(r.url)).length, 1)
  assert.deepEqual(browser.exceptions, [])
  await writeFile(evidence + '/browser-requests.json', JSON.stringify(requests, null, 2))
  console.log('PASS: canonical and existing Unix times, compact unsupported state, deduplicated time meaning, three body edits preserve complete semantics/history, unchanged table and no data read/execute')
} finally { await browser.close() }
