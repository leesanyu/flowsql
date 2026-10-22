import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'
const [direct, host, evidence, control] = process.argv.slice(2)
const checks = [], measurements = []
let failure
try {
  for (const mode of ['standalone', 'embedded']) {
    const browser = await openBrowser(mode === 'embedded' ? host : direct, mode)
    const { send, evaluate: evaluatePage, evaluateRoot, wait, pause, select, draw } = browser
    const click = (label) => evaluatePage(`[...document.querySelectorAll('button')].find(e=>e.textContent===${JSON.stringify(label)}).click()`)
    try {
      await send('Emulation.setDeviceMetricsOverride', { width: 1366, height: 768, deviceScaleFactor: 1, mobile: false })
      await send('Page.navigate', { url: (mode === 'embedded' ? host + '/explore' : direct + '/exploration/') })
      await wait(() => evaluatePage('document.querySelector("select[aria-label=数据来源]")?.options.length>=3'), 'sources')
      await evaluatePage(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent==='experience.samples').value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
      await wait(() => evaluatePage('!!document.querySelector(".quick-controls")'), 'editor')
      await draw('initial_chart')
      for (const [width, height] of [[1366,768],[1920,1080]]) {
        await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false })
        for (const theme of ['light', 'dark']) {
          if (await evaluatePage('document.documentElement.dataset.theme') !== theme) {
            if (mode === 'embedded') await evaluateRoot('document.querySelector("button[title=切换主题]").click()')
            else await click('切换主题')
          }
          await wait(() => evaluatePage(`document.documentElement.dataset.theme===${JSON.stringify(theme)}`), 'theme')
          await pause(250)
          const size = await evaluatePage(`(()=>{const p=document.querySelector('.plot-area').getBoundingClientRect();const host=[...document.querySelectorAll('*')].find(e=>e.shadowRoot);const c=host.shadowRoot.querySelector('canvas,svg.marks').getBoundingClientRect();return {width:innerWidth,height:innerHeight,pw:p.width,ph:p.height,cw:c.width,ch:c.height,scroll:document.documentElement.scrollWidth}})()`)
          assert(size.pw/size.width>=.85 && size.ph/size.height>=.70, JSON.stringify(size))
          assert(size.cw>=size.pw*.75 && size.ch>=size.ph*.65, JSON.stringify(size))
          assert(size.scroll<=size.width, JSON.stringify(size))
          measurements.push({mode,theme,...size}); checks.push(`${mode}_${width}_${theme}_filled_canvas`)
        }
      }
      const infoRect = await evaluatePage('(()=>{const r=document.querySelector("button[aria-label=来源信息]").getBoundingClientRect();return {x:r.x+5,y:r.y+5}})()')
      if (mode==='embedded') infoRect.y+=56
      await send('Input.dispatchMouseEvent', {type:'mouseMoved', ...infoRect})
      assert.equal(await evaluatePage('getComputedStyle(document.querySelector("[role=tooltip]")).display'), 'block')
      await evaluatePage('document.querySelector("button[aria-label=来源信息]").focus()')
      assert.equal(await evaluatePage('getComputedStyle(document.querySelector("[role=tooltip]")).display'), 'block')
      await evaluatePage('document.querySelector("button[aria-label=来源信息]").click()')
      assert.equal(await evaluatePage('document.querySelector("button[aria-label=来源信息]").getAttribute("aria-expanded")'), 'true')
      await evaluatePage('document.querySelector("button[aria-label=来源信息]").click()')
      checks.push(mode+'_information_focus_and_click')
      await click('高级编辑')
      await wait(() => evaluatePage('!!document.querySelector(".native-editor")?.shadowRoot.querySelector(".App")'), 'native editor')
      assert.equal(await evaluatePage('!!document.querySelector(".recommendations")'), false)
      await click('图表推荐'); await click('折线图')
      await wait(() => evaluatePage('document.querySelector("select[aria-label=图表]").value==="line"'), 'recommendation sync')
      assert.equal(await evaluatePage('[...document.querySelectorAll(".recommendations button")].find(e=>e.textContent==="直方图").disabled'), false)
      // Change the original editor's aggregation toggle, not our quick selector.
      await evaluatePage(`(()=>{const r=document.querySelector('.native-editor').shadowRoot;const b=r.querySelector('button[aria-pressed]');if(!b||b.getAttribute('aria-pressed')!=='true')throw Error('missing enabled native aggregation control');b.click()})()`)
      await click('简洁绘图')
      assert.equal(await evaluatePage('document.querySelector("select[aria-label=图表]").value'), 'line')
      assert.equal(await evaluatePage('document.querySelector("select[aria-label=聚合]").value'), 'none')
      await click('绘图'); await wait(() => evaluatePage('!!document.querySelector("output")?.textContent.includes("行")'), 'restored drawing')
      assert.equal(browser.workflows.at(-1).workflow.workflow.find(s=>s.type==='view').query[0].op, 'raw')
      checks.push(mode+'_native_recommendation_and_mode_sync')
      const open = async (label) => {
        await evaluatePage(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent===${JSON.stringify(label)}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
        await wait(() => evaluatePage(`document.querySelector('[role=tab][aria-selected=true]')?.textContent===${JSON.stringify(label)} && !!document.querySelector('.quick-controls')`), label)
      }
      const state = () => evaluatePage(`Object.fromEntries([...document.querySelectorAll('.quick-controls select,.settings-panel select,.settings-panel input')].map(e=>[e.getAttribute('aria-label')||e.type,e.type==='checkbox'?e.checked:e.value]))`)
      // A's configuration includes fields, aggregate, filter, limited detail, UTC and native mode.
      await open('experience.samples')
      await select('图表','bar')
      await click('更多设置'); await select('聚合', 'sum'); await select('过滤字段','f0')
      await evaluatePage(`(()=>{const e=document.querySelector('input[aria-label="过滤值"]');Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(e,'alpha');e.dispatchEvent(new Event('input',{bubbles:true}))})()`)
      await evaluatePage('document.querySelector(".settings-panel input[type=checkbox]").click()')
      await click('高级编辑')
      const original = await state()
      await open('experience.other'); await select('图表','line'); await select('横轴','f1'); await click('更多设置')
      await evaluatePage(`[...document.querySelectorAll('input')].find(e=>e.getAttribute('aria-label')==='UTC 时间横轴').click()`)
      await draw('table_b_line')
      await open('experience.samples')
      assert.equal(await evaluatePage('document.querySelector(".table-editor").dataset.mode'), 'advanced')
      await click('更多设置')
      assert.deepEqual(await state(), original)
      checks.push(mode+'_independent_workspace_restore')
      const count = await evaluatePage('document.querySelectorAll("[role=tab]").length')
      await open('experience.samples')
      assert.equal(await evaluatePage('document.querySelectorAll("[role=tab]").length'), count)
      await open('second.samples'); await select('图表','point'); await select('横轴','f3'); await draw('same_name_other_source')
      await open('experience.samples')
      assert.equal(await evaluatePage('document.querySelector("select[aria-label=图表]").value'), 'bar')
      checks.push(mode+'_same_name_identity_and_reopen')
      await click('简洁绘图')
      // Delay completed HTTP responses and deliver them after leaving A. Ignore abort in this mock
      // so the client must actually reject stale generations, not merely rely on fetch abort.
      await evaluatePage(`(()=>{window.__originalFetch=window.fetch;window.__held=[];window.__cancels=[];window.__signals=[];window.__delay=true;
        window.fetch=async (url,options)=>{if(String(url).endsWith('/cancel'))window.__cancels.push(JSON.parse(options.body));
          const response=await window.__originalFetch(url,options);
          if(window.__delay&&String(url).endsWith('/compute')){window.__signals.push(options.signal);await new Promise(resolve=>window.__held.push(resolve))}return response}})()`)
      await click('绘图'); await wait(() => evaluatePage('window.__held.length>0'), 'held response')
      await evaluatePage('window.__aSignals=window.__signals.slice()')
      await open('experience.other')
      await evaluatePage('window.__delay=false;window.__held.splice(0).forEach(resolve=>resolve())')
      await draw('b_after_stale_a')
      assert(await evaluatePage('window.__cancels.length>0&&window.__aSignals.every(s=>s.aborted)'))
      assert.equal(await evaluatePage('document.querySelector("select[aria-label=图表]").value'), 'line')
      assert.equal(browser.workflows.at(-1).source_id, await evaluatePage('document.querySelector("select[aria-label=数据来源]").value'))
      checks.push(mode+'_switch_cancel_and_stale_response')
      // Only the visible editor exists. A closed page releases both its state and pending query.
      await evaluatePage('window.__delay=true')
      await click('绘图'); await wait(() => evaluatePage('window.__held.length>0'), 'close held response')
      await evaluatePage(`[...document.querySelectorAll('button')].find(e=>e.getAttribute('aria-label')==='关闭 experience.other').click()`)
      await evaluatePage('window.__delay=false;window.__held.splice(0).forEach(resolve=>resolve())')
      await wait(() => evaluatePage('!!document.querySelector(".quick-controls")'), 'remaining page')
      assert.equal(await evaluatePage('[...document.querySelectorAll("[role=tab]")].some(e=>e.textContent==="experience.other")'), false)
      await open('experience.other')
      assert.equal(await evaluatePage('document.querySelector("select[aria-label=图表]").value'), 'bar')
      checks.push(mode+'_close_releases_state_and_query')
      if (mode==='embedded') {
        await evaluatePage('window.__delay=true')
        await click('绘图'); await wait(() => evaluatePage('window.__held.length>0'), 'hide held response')
        await evaluateRoot('[...document.querySelectorAll(".el-menu-item")].find(e=>e.textContent==="通道管理").click()')
        await wait(() => evaluatePage('document.body.dataset.visible==="false"'), 'hidden bridge')
        await evaluatePage('window.__delay=false;window.__held.splice(0).forEach(resolve=>resolve())')
        const requests=browser.workflows.length
        await pause(350)
        assert.equal(browser.workflows.length, requests)
        assert.equal(await evaluatePage('document.querySelectorAll(".table-editor").length'), 0)
        await evaluateRoot('[...document.querySelectorAll(".el-menu-item")].find(e=>e.textContent==="数据探索").click()')
        await wait(() => evaluatePage('!!document.querySelector(".quick-controls")'), 'visible again')
        checks.push(mode+'_hide_cancels_and_stops_computation')
      }
      await evaluatePage('window.fetch=window.__originalFetch')
      await click('刷新来源')
      await wait(() => evaluatePage('!!document.querySelector(".quick-controls")'), 'unchanged source refresh')
      await evaluatePage(`fetch(${JSON.stringify(control+'/schema')})`)
      await click('刷新来源')
      await wait(() => evaluatePage('document.querySelector("[role=alert]")?.textContent.includes("字段结构已变化")'), 'schema invalid')
      assert.equal(await evaluatePage('document.querySelectorAll(".table-editor").length'), 0)
      const beforeInvalid=browser.workflows.length
      await pause(200); assert.equal(browser.workflows.length,beforeInvalid)
      await click('重新选择字段'); await draw('schema_reset')
      await evaluatePage(`fetch(${JSON.stringify(control+'/schema')})`)
      await click('刷新来源')
      await wait(() => evaluatePage('document.querySelector("[role=alert]")?.textContent.includes("字段结构已变化")'), 'schema invalid before removal')
      await evaluatePage(`fetch(${JSON.stringify(control+'/remove')})`)
      await click('刷新来源')
      await wait(() => evaluatePage('document.querySelector("[role=alert]")?.textContent.includes("来源已消失")'), 'source disappeared')
      assert.equal(await evaluatePage('document.querySelectorAll(".table-editor").length'), 0)
      assert.equal(await evaluatePage('[...document.querySelectorAll("button")].some(e=>e.textContent==="重新选择字段")'), false)
      await evaluatePage(`fetch(${JSON.stringify(control+'/restore')})`)
      checks.push(mode+'_schema_and_source_invalidation')
      assert.deepEqual(browser.exceptions, [])
      checks.push(mode+'_no_uncaught_exceptions')
      const screenshot = await send('Page.captureScreenshot', { format: 'png' })
      await writeFile(evidence+'/'+mode+'.png', Buffer.from(screenshot.data,'base64'))
    } catch (error) {
      console.error(await evaluatePage('document.body.innerText').catch(()=>''))
      console.error(JSON.stringify(browser.exceptions))
      throw error
    } finally { await browser.close() }
  }
} catch (error) { failure=String(error.stack||error);console.error(failure);process.exitCode=1 }
finally {
  const selected=25
  await writeFile(evidence+'/browser.json',JSON.stringify({selected,executed:checks.length+(failure?1:0),passed:checks.length,failed:failure?1:0,skipped:0,not_executed:selected-checks.length-(failure?1:0),checks,measurements,failure},null,2))
}
