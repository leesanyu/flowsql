import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'
const [direct, host, evidence] = process.argv.slice(2)
const checks = []
let failure
const selected = 36
try {
  for (const mode of ['standalone', 'embedded']) {
    const browser = await openBrowser(mode === 'embedded' ? host : direct, mode)
    const { evaluate, evaluateRoot, send, select, wait, pause } = browser
    const click = (text) => evaluate(`[...document.querySelectorAll('button')].find(e=>e.textContent===${JSON.stringify(text)}).click()`)
    const open = async (label) => {
      await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent===${JSON.stringify(label)}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
      await wait(() => evaluate('!!document.querySelector(".quick-controls")'), 'workspace')
    }
    const input = async (label, value) => {
      await evaluate(`(()=>{const e=document.querySelector('input[aria-label="${label}"]');Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(e,${JSON.stringify(String(value))});e.dispatchEvent(new Event('input',{bubbles:true}))})()`)
    }
    try {
      await send('Emulation.setDeviceMetricsOverride', { width: 1366, height: 768, deviceScaleFactor: 1, mobile: false })
      await send('Page.navigate', { url: mode === 'embedded' ? host + '/explore' : direct + '/exploration/' })
      await wait(() => evaluate('document.querySelector("select[aria-label=数据来源]")?.options.length>=3'), 'sources')
      await open('experience.samples')
      assert.equal(await evaluate('document.querySelector("select[aria-label=图表]").options.length'), 13)
      assert.equal(await evaluate('document.querySelector(".table-editor").dataset.mode'), 'simple')
      checks.push(mode + '_all_types_in_default_ui')
      const kinds = ['bar','stacked_bar','grouped_bar','line','area','point','circle','pie','heatmap','table','highlight_table','boxplot','histogram']
      for (const kind of kinds) {
        await select('图表', kind)
        await wait(() => evaluate(`document.querySelector('select[aria-label=图表]').value===${JSON.stringify(kind)}`), kind)
        assert.equal(await evaluate('document.querySelector(".primary").disabled'), false, kind)
        if (kind === 'histogram') {
          assert.equal(await evaluate('!!document.querySelector("select[aria-label=纵轴]")'), false)
          assert.equal(await evaluate('!!document.querySelector("select[aria-label=数值字段]")'), true)
          assert.equal(await evaluate('!!document.querySelector("select[aria-label=聚合]")'), false)
          await select('数值字段','f2'); await input('箱数', 2)
        }
        if (kind === 'pie') {
          assert.equal(await evaluate('!!document.querySelector("select[aria-label=横轴],select[aria-label=纵轴]")'), false)
          assert.equal(await evaluate('!!document.querySelector("select[aria-label=分类]")'), true)
        }
        await click('绘图')
        await wait(() => evaluate('document.querySelector("output")?.textContent.includes("行")'), kind + ' result')
        await pause(180)
        assert.equal(await evaluate('document.querySelector("[role=alert]")?.textContent || ""'), '', kind)
        const rendered = await evaluate(`(()=>{const host=[...document.querySelectorAll('*')].find(e=>e.shadowRoot);return !!host?.shadowRoot.querySelector('canvas,svg.marks,table')})()`)
        assert(rendered, kind + ' missing actual renderer')
        if (kind === 'histogram') assert.equal(browser.workflows.at(-1).workflow.workflow.find(s=>s.type==='view').query[0].op,'histogram')
        checks.push(mode + '_draw_' + kind)
      }
      await input('箱数', 0)
      assert.equal(await evaluate('document.querySelector(".primary").disabled'), true)
      await input('箱数', 3)
      await click('高级编辑')
      await wait(() => evaluate('!!document.querySelector(".native-editor")?.shadowRoot'), 'histogram advanced')
      await click('简洁绘图')
      assert.equal(await evaluate('document.querySelector("select[aria-label=图表]").value'), 'histogram')
      assert.equal(await evaluate('document.querySelector("input[aria-label=箱数]").value'), '3')
      checks.push(mode + '_single_field_histogram_mode_restore')
      await open('experience.other'); await select('图表','pie')
      await open('experience.samples')
      assert.equal(await evaluate('document.querySelector("select[aria-label=图表]").value'), 'histogram')
      assert.equal(await evaluate('document.querySelector("input[aria-label=箱数]").value'), '3')
      checks.push(mode + '_table_state_restore')
      await click('图表推荐')
      await evaluate('[...document.querySelectorAll(".recommendations button")].find(e=>e.textContent==="饼图").click()')
      assert.equal(await evaluate('document.querySelector("select[aria-label=图表]").value'), 'pie')
      checks.push(mode + '_recommendation_same_chart')
      assert.deepEqual(browser.exceptions, [])
      checks.push(mode + '_no_uncaught_exceptions')
      const screenshot=await send('Page.captureScreenshot', {format:'png'})
      await writeFile(evidence + '/' + mode + '-chart-types.png', Buffer.from(screenshot.data,'base64'))
    } catch (error) { console.error(await evaluate('document.body.innerText').catch(()=>'')); console.error(browser.exceptions); throw error }
    finally { await browser.close() }
  }
} catch (error) { failure = String(error.stack || error); console.error(failure); process.exitCode = 1 }
finally {
  await writeFile(evidence + '/chart-browser.json', JSON.stringify({selected,executed:checks.length+(failure?1:0),passed:checks.length,failed:failure?1:0,skipped:0,not_executed:selected-checks.length-(failure?1:0),checks,failure},null,2))
}
