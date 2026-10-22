import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'
const [base, label, report] = process.argv.slice(2)
const checks = []
let failure
const browser = await openBrowser(base)
const { evaluate, send, wait, pause, select, draw } = browser
const click = (label) => evaluate(`[...document.querySelectorAll('button')].find(e=>e.textContent===${JSON.stringify(label)}).click()`)
const noDialog = async () => {
  await pause(100)
  assert.equal(await evaluate(`[...document.querySelectorAll('*')].some(e=>e.shadowRoot?.textContent.includes('Oops!'))`), false)
  assert.equal(await evaluate(`[...document.querySelectorAll('*')].some(e=>e.shadowRoot?.querySelector('[role=dialog]'))`), false)
}
try {
  await send('Page.navigate', { url: base + '/exploration/' })
  await wait(() => evaluate('document.querySelector("select[aria-label=数据来源]")?.options.length>1'), 'sources')
  await evaluate(`(()=>{const e=document.querySelector('select[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent===${JSON.stringify(label)}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
  await wait(() => evaluate('!!document.querySelector(".quick-controls")'), 'numeric editor')
  await draw('decimal_mean')
  await wait(() => evaluate('document.querySelector(".precision-badge")?.textContent.includes("近似")'), 'approximate badge')
  await evaluate('document.querySelector(".precision-badge").click()')
  if (label.startsWith('clickhouse.')) assert(await evaluate('document.querySelector(".precision-info").textContent.includes("原生返回 Float64")'))
  checks.push('decimal_mean_approximation_and_origin')
  await select('聚合','none'); await draw('decimal_projection')
  await evaluate('[...document.querySelectorAll(".precision-info details")].forEach(e=>e.open=true)')
  assert(await evaluate('document.querySelector(".precision-info").textContent.includes("0.10000000000000000001")'))
  assert(await evaluate('document.querySelector(".precision-info").textContent.includes("NULL")'))
  checks.push('projection_exact_text_and_null')
  await select('聚合','mean'); await select('纵轴','f2'); await click('绘图')
  await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.includes("安全范围")'), 'simple integer error')
  assert.equal(await evaluate('document.querySelector("select[aria-label=纵轴]").value'), 'f2')
  assert.equal(await evaluate('document.querySelector("output").textContent'), '查询失败')
  await noDialog(); checks.push('simple_chinese_error_keeps_configuration')
  await click('高级编辑'); await click('绘图')
  await wait(() => evaluate('document.querySelector("[role=alert]")?.textContent.includes("安全范围")'), 'native integer error')
  await noDialog(); checks.push('native_error_has_no_english_popup')
  await select('纵轴','f1'); await draw('decimal_recovery')
  await noDialog(); assert.deepEqual(browser.exceptions, [])
  checks.push('adjust_field_recovers_without_losing_chart')
} catch (error) { failure=String(error.stack||error);console.error(failure);console.error(await evaluate('document.body.innerText').catch(()=>''));process.exitCode=1 }
finally {
  await browser.close()
  await writeFile(report, JSON.stringify({ selected:5,executed:checks.length+(failure?1:0),passed:checks.length,failed:failure?1:0,skipped:0,not_executed:5-checks.length-(failure?1:0),checks,failure },null,2))
}
