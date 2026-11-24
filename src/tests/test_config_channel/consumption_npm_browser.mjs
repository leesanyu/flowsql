import assert from 'node:assert/strict'
import { readFile, writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, visual, factsPath, evidence] = process.argv.slice(2)
const facts = JSON.parse(await readFile(factsPath,'utf8'))
for (const mode of ['workbench','exploration']) {
  const b=await openBrowser(mode==='workbench'?base:visual),{evaluate:ev,wait,send,pause,select,draw}=b
  const body=()=>ev('document.body.innerText')
  const text=value=>wait(async()=>(await body()).includes(value),value)
  const raw=()=>ev('document.querySelector("[data-testid=semantic-json-content]").textContent')
  const data=async()=>JSON.parse(await raw())
  async function close(){
    if(!await ev('!!document.querySelector("dialog.semantic-json-window")'))return
    await ev(`[...document.querySelector('dialog').querySelectorAll('button')].find(e=>e.textContent==='关闭').click()`)
    await wait(()=>ev('!document.querySelector("dialog.semantic-json-window")'),'closed')
  }
  async function open(title){await close();await ev(`document.querySelector('[data-semantic-panel="${title}"]').click()`);await wait(()=>ev('!!document.querySelector("dialog.semantic-json-window")'),'reader');await data()}

  try {
    await send('Page.addScriptToEvaluateOnNewDocument',{source:`window.__calls=[];window.__pending=0;window.__lastCall=0;const f=window.fetch;window.fetch=function(u,o){window.__calls.push(String(u));window.__pending++;window.__lastCall=Date.now();return f.apply(this,arguments).finally(()=>window.__pending--)}`})
    await send('Page.navigate',{url:mode==='workbench'?base+'/tasks':visual+'/exploration/?web_url='+encodeURIComponent(base)})
    if(mode==='workbench'){
      async function sql(value){
        await close();await wait(()=>ev('!!document.querySelector(".sql-textarea textarea")'),'SQL editor')
        await ev(`(()=>{const e=document.querySelector('.sql-textarea textarea');e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}))})()`)
        await pause(350);await ev(`document.querySelector('[aria-label="执行 SQL"]').click()`);await wait(()=>ev('!!document.querySelector("[data-testid=result-semantics]")'),'result');await ev('document.querySelector("[data-testid=result-semantics]").click()');await text('本次结果依据')
      }
      await sql('SELECT latency_ns FROM sqlite.example.npm_dns_transaction_ipv4_data')
      for(const key of ['validity','null_meaning','aggregation'])await text(facts[key])
      await text('producer_default');await text('静态定义不证明历史行')
      await sql('SELECT SUM(packets_ab) AS total FROM sqlite.example.npm_session_ipv4_data')
      await text(facts.cumulative);assert.ok((await data()).reading.fields.some(field=>field.result_unit==='未知'))
      await text(facts.session_warning)
    }else{
      await wait(()=>ev('document.querySelector("[aria-label=数据来源]")?.options.length>1'),'NPM sources')
      await ev(`(()=>{const e=document.querySelector('[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent.endsWith('.npm_dns_transaction_ipv4_data')).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
      await wait(()=>ev('!!document.querySelector(".quick-controls")'),'controls')
      await select('图表','histogram')
      const fid=await ev(`[...document.querySelector('[aria-label="数值字段"]').options].find(o=>o.textContent==='latency_ns').value`)
      await select('数值字段',fid)
      await wait(()=>ev('window.__pending===0 && Date.now()-window.__lastCall>=750'),'configuration requests settled')
      await draw('NPM DNS latency histogram')
      const before=await ev('window.__calls.length')
      for(const title of ['来源字段说明','所选字段说明','图表口径说明'])await open(title)
      for(const key of ['validity','null_meaning','aggregation'])await text(facts[key])
      const layout=await ev(`({reading:document.querySelector('.semantic-reading').getBoundingClientRect().height,plot:document.querySelector('.plot-area').getBoundingClientRect().height,viewport:innerHeight})`)
      assert.ok(layout.reading<=layout.viewport*.5 && layout.plot>0,JSON.stringify(layout))
      assert.ok((await data()).reading.fields.some(field=>field.result_unit==='个'))
      await text('producer_default')
      await pause(250);assert.equal(await ev('window.__calls.length'),before,'reading NPM definitions performs no requests')
      await text('静态定义不证明历史行')
      const currentPanel='document.querySelector(\'dialog.semantic-json-window\')'
      const managed=await ev(`${currentPanel}.querySelector('[data-current-object]').href`)
      assert.ok(managed.startsWith(base+'/channels?dataset='))
      async function changeWebUrl(value){
        await ev(`history.replaceState(null,'','?web_url='+encodeURIComponent(${JSON.stringify(value)}))`)
        await open('图表口径说明')
      }
      await changeWebUrl('javascript:window.__navigationUnsafe=true')
      assert.equal(await ev(`!!${currentPanel}.querySelector('[data-current-object]')`),false)
      assert.equal(await ev('!!window.__navigationUnsafe'),false)
      await changeWebUrl(base)
      await ev(`${currentPanel}.querySelector('[data-current-object]').click()`)
      await wait(()=>ev('!!document.querySelector("[data-testid=channel-description]")'),'actual Web object management')
      await wait(()=>ev('!!document.querySelector(\'[aria-label="latency_ns 字段说明"]\')'),'current NPM object')
    }
    assert.deepEqual(b.exceptions,[])
    await writeFile(`${evidence}/${mode}.png`,Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
    console.log('PASS',mode,'NPM full validity/NULL/aggregation and precise static evidence')
  }catch(error){await writeFile(`${evidence}/${mode}-failure.txt`,error.stack+'\n'+await body());await writeFile(`${evidence}/${mode}-failure.png`,Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));throw error}
  finally{await b.close()}
}
