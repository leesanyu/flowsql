import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'

const [direct, host, label, evidence] = process.argv.slice(2)
const checks = []
let failure
try {
  for (const mode of host === '-' ? ['standalone'] : ['standalone', 'embedded']) {
    const browser = await openBrowser(mode === 'embedded' ? host : direct, mode)
    const { send, evaluate, select, wait, pause } = browser
    const click = (name) => evaluate(`[...document.querySelectorAll('button')].find(e=>e.textContent===${JSON.stringify(name)}).click()`)
    const open = async (name) => {
      await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent===${JSON.stringify(name)}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
      await wait(() => evaluate('!!document.querySelector(".quick-controls")'), 'workspace')
    }
    const run = async (unit, expected) => {
      await select('时间分桶', unit)
      await click('绘图')
      await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), unit + ' result')
      await pause(250)
      assert.equal(await evaluate('document.querySelector("[role=alert]")?.textContent || ""'), '')
      const query = browser.workflows.at(-1).workflow.workflow.find((s) => s.type === 'view').query[0]
      assert.deepEqual(query.groupBy, [{field:'f0',unit,asFieldKey:'f0'}])
      assert.equal(query.measures[0].agg, 'distinctCount')
      const response = await fetch(direct + '/api/visualization/v1/compute', {method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({...browser.workflows.at(-1),request_id:crypto.randomUUID()})})
      assert.equal(response.status,200)
      const data = await response.json()
      assert.deepEqual(data.rows.map((r)=>[r.f0,r[query.measures[0].asFieldKey]]).sort(), expected.sort())
      assert(await evaluate(`(()=>{const h=[...document.querySelectorAll('*')].find(e=>e.shadowRoot);return !!h?.shadowRoot.querySelector('canvas,svg.marks,table')})()`))
      checks.push(mode + '_' + unit + '_native_result_and_renderer')
    }
    try {
      await send('Page.navigate', {url:mode === 'embedded' ? host + '/explore' : direct + '/exploration/'})
      await wait(() => evaluate('document.querySelector("select[aria-label=数据来源]")?.options.length>1'), 'sources')
      await open(label)
      await select('图表','line'); await select('横轴','f0'); await select('聚合','distinctCount'); await select('纵轴','f1')
      await run('day', [[null,1],['2024-02-29',1],['2024-03-01',1],['2025-01-01',0]])
      await run('month', [[null,1],['2024-02-01',1],['2024-03-01',1],['2025-01-01',0]])
      await click('高级编辑')
      await wait(() => evaluate('!!document.querySelector(".native-editor")?.shadowRoot'), 'advanced editor')
      await pause(250)
      await click('简洁绘图')
      assert.equal(await evaluate('document.querySelector("select[aria-label=时间分桶]").value'), 'month')
      assert.equal(await evaluate('document.querySelector("select[aria-label=纵轴]").value'), 'f1')
      checks.push(mode + '_advanced_state_restore')
      if (host !== '-') {
        await open(label.replace('.samples','.other'))
        await open(label)
        assert.equal(await evaluate('document.querySelector("select[aria-label=时间分桶]").value'), 'month')
        assert.equal(await evaluate('document.querySelector("select[aria-label=聚合]").value'), 'distinctCount')
        checks.push(mode + '_table_state_restore')
      }
      await select('聚合','sum')
      assert.equal(await evaluate('document.querySelector("select[aria-label=纵轴]").value'), 'f2')
      await click('绘图')
      await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.includes("安全")'), 'unsafe integer error')
      await select('聚合','distinctCount'); await select('纵轴','f1'); await run('day', [[null,1],['2024-02-29',1],['2024-03-01',1],['2025-01-01',0]])
      checks.push(mode + '_error_recovery')
      await select('聚合','none')
      assert.equal(await evaluate('!!document.querySelector("select[aria-label=时间分桶]")'), false)
      checks.push(mode + '_raw_clears_bucket')
      assert.deepEqual(browser.exceptions,[])
    } finally { await browser.close() }
  }
} catch (error) { failure=String(error.stack || error); console.error(failure); process.exitCode=1 }
finally {
  const selected=host === '-' ? 6 : 14
  await writeFile(evidence, JSON.stringify({selected,executed:checks.length+(failure?1:0),passed:checks.length,failed:failure?1:0,
    skipped:0,not_executed:selected-checks.length-(failure?1:0),checks,failure},null,2))
}
