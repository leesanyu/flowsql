import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'

const [direct, evidence, mode = 'standalone'] = process.argv.slice(2)
const checks = [], measurements = []
let failure

const chartTypes = ['bar', 'stacked_bar', 'grouped_bar', 'line', 'area', 'point', 'circle', 'pie', 'heatmap', 'table',
  'highlight_table', 'histogram']

try {
  const browser = await openBrowser(direct, mode)
  const { send, evaluate, evaluateRoot, wait, pause } = browser
  const click = (label) => evaluate(`[...document.querySelectorAll('button')].find(e=>e.textContent===${JSON.stringify(label)}).click()`)
  const snapshotId = () => evaluate(`(()=>{const o=document.querySelector('.snapshot-bar');return o?o.getAttribute('data-snapshot-id'):''})()`)
  try {
    await send('Emulation.setDeviceMetricsOverride', { width: 1366, height: 768, deviceScaleFactor: 1, mobile: false })
    if (mode === 'embedded') {
      await send('Page.navigate', { url: direct + '/channels' })
      const row = (name) => `[...document.querySelectorAll('.el-table__row')].find(e=>e.textContent.includes(${JSON.stringify(name)}))`
      await wait(() => evaluateRoot(`${row('lazy_ui')}?.textContent.includes('未加载')`), 'unloaded channel row')
      const initial = await evaluateRoot(`${row('lazy_ui')}.textContent`)
      assert(initial.includes('未知') && initial.includes('首次访问后确定'), initial)
      await evaluateRoot(`[...${row('lazy_ui')}.querySelectorAll('button')].find(e=>e.textContent.includes('预览')).click()`)
      await wait(() => evaluateRoot(`${row('lazy_ui')}?.textContent.includes('已加载')`), 'loaded channel row')
      assert((await evaluateRoot(`${row('lazy_ui')}.textContent`)).includes('2 字段'))
      await send('Page.navigate', { url: direct + '/channels' })
      await wait(() => evaluateRoot(`${row('failed_ui')}?.textContent.includes('未加载')`), 'unloaded broken channel')
      await evaluateRoot(`[...${row('failed_ui')}.querySelectorAll('button')].find(e=>e.textContent.includes('预览')).click()`)
      await wait(() => evaluateRoot(`${row('failed_ui')}?.textContent.includes('加载失败')`), 'failed channel row')
      assert((await evaluateRoot(`${row('failed_ui')}.textContent`)).includes('未知'))
      checks.push('channel_unloaded_loaded_failed_states')
    }
    // Renewal runs on a short interval so the page exercises the renew path during the run.
    await send('Page.addScriptToEvaluateOnNewDocument', { source: 'globalThis.__flowsqlRenewMilliseconds = 1000' })
    await send('Page.navigate', { url: direct + (mode === 'embedded' ? '/explore' : '/exploration/') })
    await wait(() => evaluate('document.querySelector("select[aria-label=数据来源]")?.options.length>=3'), 'sources')
    const groups = await evaluate(`[...document.querySelectorAll('select[aria-label="数据来源"] optgroup')].map(g=>g.label)`)
    assert.deepEqual(groups, ['数据库表', 'DataFrame 快照'])
    checks.push('source_groups')

    // Opening a DataFrame source acquires a snapshot and shows its identity without exposing the version
    // as a data identity.
    await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent==='dataframe.sales').value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
    await wait(() => evaluate('!!document.querySelector(".snapshot-bar")'), 'snapshot bar')
    const bar = await evaluate(`document.querySelector('.snapshot-bar').textContent`)
    assert(bar.includes('DataFrame 快照') && bar.includes('采集时间') && bar.includes('行数 3'), bar)
    await wait(() => snapshotId(), 'snapshot id')
    const first = await snapshotId()
    checks.push('snapshot_bar')
    const renewal = await evaluate(`fetch('/api/visualization/v1/snapshots/renew',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({snapshot_id:${JSON.stringify(first)}})}).then(async r=>({status:r.status,body:await r.json()}))`)
    assert.equal(renewal.status, 200, JSON.stringify(renewal))
    assert(renewal.body.expires_at_unix > 0, JSON.stringify(renewal))
    checks.push('snapshot_renewed')

    // Boxplot stays unavailable on the DataFrame path and states the reason.
    const marks = await evaluate(`[...document.querySelectorAll('select[aria-label=图表] option')].map(o=>o.value)`)
    assert.equal(marks.includes('boxplot'), false, marks.join(','))
    assert(await evaluate(`document.querySelector('.table-editor').textContent.includes('不支持箱线图')`))
    checks.push('boxplot_disabled')

    // Every listed chart type draws against the bound snapshot.
    for (const mark of chartTypes) {
      await evaluate(`(()=>{const e=document.querySelector('select[aria-label="图表"]');e.value=${JSON.stringify(mark)};e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
      await click('绘图')
      await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), mark)
      // Table marks render as text nodes rather than a canvas.
      await wait(() => evaluate('[...document.querySelectorAll("*")].some(e=>e.shadowRoot?.querySelector("canvas,svg.marks,table"))'), mark + ' rendered')
      await pause(120)
      assert.equal(await evaluate('!!document.querySelector("[role=alert]")'), false, mark)
    }
    checks.push('chart_types')
    const body = browser.workflows.at(-1)
    assert(body.snapshot_id && body.source_id && body.schema_revision, JSON.stringify(body))
    checks.push('compute_carries_snapshot')

    // Simple/advanced editing keeps the snapshot binding; the version only changes on request.
    await click('高级编辑')
    await wait(() => evaluate('!!document.querySelector(".native-editor")?.shadowRoot.querySelector(".App")'), 'native editor')
    await click('简洁绘图')
    assert.equal(await snapshotId(), first)
    checks.push('mode_switch_keeps_snapshot')

    await click('获取最新快照')
    await wait(async () => (await snapshotId()) && (await snapshotId()) !== first, 'new snapshot')
    const second = await snapshotId()
    assert.notEqual(second, first)
    assert(await evaluate(`document.querySelector('.snapshot-bar').textContent.includes('行数 3')`))
    checks.push('refresh_switches_version')

    // "重新绘图" reuses the current snapshot instead of acquiring another one.
    await click('绘图')
    await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), 'redraw')
    assert.equal(await snapshotId(), second)
    assert.equal(browser.workflows.at(-1).snapshot_id, second)
    checks.push('redraw_reuses_snapshot')

    // Database and DataFrame pages coexist and switching preserves each version.
    await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent==='memory.rows').value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
    await wait(() => evaluate('document.querySelectorAll(".workspace-tab").length===2'), 'two pages')
    assert.equal(await evaluate('!!document.querySelector(".snapshot-bar")'), false)
    await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent==='dataframe.sales').value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
    await wait(async () => (await snapshotId()) === second, 'first page restored')
    checks.push('database_dataframe_switch')

    for (const [width, height] of [[1366, 768], [1920, 1080]]) {
      await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false })
      await pause(200)
      const size = await evaluate(`(()=>{const p=document.querySelector('.plot-area').getBoundingClientRect();const host=[...document.querySelectorAll('*')].find(e=>e.shadowRoot);const c=host.shadowRoot.querySelector('canvas,svg.marks').getBoundingClientRect();return {width:innerWidth,height:innerHeight,pw:p.width,ph:p.height,cw:c.width,ch:c.height,scroll:document.documentElement.scrollWidth}})()`)
      assert(size.pw / size.width >= .85 && size.ph / size.height >= .70, JSON.stringify(size))
      assert(size.cw >= size.pw * .75 && size.ch >= size.ph * .65, JSON.stringify(size))
      assert(size.scroll <= size.width, JSON.stringify(size))
      measurements.push({ width, height, ...size })
      checks.push(`canvas_${width}x${height}`)
    }

    // Closing the page releases its snapshot: a later compute with that version reports expiry.
    const closing = second
    await evaluate(`document.querySelector('button[aria-label="关闭 dataframe.sales"]').click()`)
    await wait(() => evaluate('document.querySelectorAll(".workspace-tab").length===1'), 'page closed')
    const released = await evaluate(`fetch('/api/visualization/v1/snapshots/renew',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({snapshot_id:${JSON.stringify(closing)}})}).then(r=>r.status)`)
    assert.equal(released, 409, String(released))
    checks.push('close_releases_snapshot')
    assert.equal(browser.exceptions.length, 0, JSON.stringify(browser.exceptions))
  } finally {
    await browser.close()
  }
} catch (error) {
  failure = error
  console.error('FAIL', error.message)
}
await writeFile(evidence + '/dataframe-browser.json', JSON.stringify({ mode, checks, measurements, failure: failure?.message }, null, 2))
if (failure) { console.error(failure.stack || failure.message); process.exit(1) }
console.log(JSON.stringify({ checks }))
