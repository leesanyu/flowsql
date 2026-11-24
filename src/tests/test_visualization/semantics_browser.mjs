import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from './browser_support.mjs'
const [base, mode, control, evidence] = process.argv.slice(2)
const b=await openBrowser(base,mode==='embedded'?'embedded':'standalone'),{evaluate:ev,wait,send,pause}=b
const body=()=>ev('document.body.innerText')
const text=value=>wait(async()=>(await body()).includes(value),value)
const raw=()=>ev('document.querySelector("[data-testid=semantic-json-content]").textContent')
const data=async()=>JSON.parse(await raw())
async function close(){
  if(!await ev('!!document.querySelector("dialog.semantic-json-window")'))return
  await ev(`[...document.querySelector('dialog').querySelectorAll('button')].find(e=>e.textContent==='关闭').click()`)
  await wait(()=>ev('!document.querySelector("dialog.semantic-json-window")'),'closed')
}
const select=async(label,value)=>{await close();await b.select(label,value)}
const draw=async name=>{await close();await b.draw(name)}
async function open(title,keyboard=false) {
  await close()
  await wait(()=>ev(`!!document.querySelector('[data-semantic-panel="${title}"]')`),title)
  await ev(`document.querySelector('[data-semantic-panel="${title}"]').focus()`)
  if(keyboard){await send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',windowsVirtualKeyCode:13,nativeVirtualKeyCode:13,text:'\r',unmodifiedText:'\r'});await send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',windowsVirtualKeyCode:13,nativeVirtualKeyCode:13})}
  else await ev(`document.querySelector('[data-semantic-panel="${title}"]').click()`)
  await wait(()=>ev('!!document.querySelector("dialog.semantic-json-window[open]")'),'JSON '+title)
  assert.equal(await ev('document.querySelectorAll("dialog.semantic-json-window").length'),1)
  assert.ok(await ev('document.querySelector("dialog .json-key") && document.querySelector("dialog .json-string")'))
  await data()
}
async function source(label) {
  await close()
  await ev(`(()=>{const e=document.querySelector('[aria-label="数据来源"]');e.value=[...e.options].find(o=>o.textContent===${JSON.stringify(label)}).value;e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
  await wait(()=>ev('!!document.querySelector(".quick-controls")'),'source controls')
}
const counts=()=>ev(`({compute:window.__calls.filter(u=>u.endsWith('/compute')).length,acquire:window.__calls.filter(u=>u.endsWith('/acquire')).length,preview:window.__calls.filter(u=>u.includes('/preview')).length,write:window.__calls.filter(u=>['/save','/apply','/save-description'].some(p=>u.endsWith(p))).length})`)
async function setMeaning() {
  const post=async(path,payload)=>{const r=await fetch(control+'/semantics/'+path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const body=await r.json();assert.equal(r.status,200,JSON.stringify(body));return body.data}
  const locator={kind:'dataframe',name:'sales'},before=await post('describe',{target:{kind:'channel',locator}})
  await post('channels/save-description',{locator,basis:before.basis,editor_changes:{fields:{value:{definition:'New browser frame meaning '+mode}}},confirmations:before.checks.required_confirmations})
}
try {
  await send('Page.addScriptToEvaluateOnNewDocument',{source:`window.__calls=[];const f=window.fetch;window.fetch=function(u,o){window.__calls.push(String(u));return f.apply(this,arguments)}`})
  await send('Page.navigate',{url:base+(mode==='embedded'?'/explore':'/exploration/')})
  if(mode==='embedded')await wait(()=>b.evaluateRoot('!!document.querySelector("iframe")?.contentWindow.document.querySelector("select")'),'embedded')
  await wait(()=>ev('document.querySelector("[aria-label=数据来源]")?.options.length>=2'),'sources')
  for(const label of mode==='error'?['memory.rows']:['memory.rows','dataframe.sales']){
    await source(label);await select('横轴','f2');await select('纵轴','f1');await select('图表','bar');await select('聚合','sum');await draw(label+' bar')
    await pause(350);const initialReads=await counts(),height=await ev('document.querySelector(".plot-area").getBoundingClientRect().height')
    await open('来源字段说明',true);assert.equal(await ev('document.querySelector(".plot-area").getBoundingClientRect().height'),height,'modal leaves canvas size intact')
    await open('所选字段说明');await open('图表口径说明');await pause(250)
    assert.deepEqual(await counts(),initialReads,'opening dialogs does not read business data')
    if(mode==='error'){
      await text('说明读取失败');await text('SOURCE_UNAVAILABLE');await text('数据与图表设置已保留')
      assert.equal((await data()).semantic_context.error.code,'SOURCE_UNAVAILABLE')
      assert.equal(await ev('document.querySelector("[aria-label=聚合]").value'),'sum')
      await select('聚合','count');await draw('metadata error retains usable chart')
      assert.ok((await body()).includes('行'));continue
    }
    for(const value of ['not measured','only valid samples','only one revision per identity','18446744073709551615','browser captured meaning'])await text(value)
    assert.equal(await ev('!!window.__unsafe || !!document.querySelector("dialog img")'),false)
    assert.ok((await data()).reading.fields.some(field=>field.result_unit==='未知'))
    await open('所选字段说明');assert.ok((await data()).reading.fields.some(field=>field.result_unit==='ms'))
    assert.ok((await data()).semantic_context.fields.every(field=>['f1','f2'].includes(field.output_name)))
    await open('来源字段说明');await text('cumulative revisions must not be summed')
    assert.ok((await raw()).includes('dictionary.'))
    const current=await ev('document.querySelector("dialog [data-current-object]")?.href || null')
    if(mode==='embedded')assert.ok(current.startsWith(base+'/channels?dataset='))
    else assert.equal(current,null,'standalone without Web origin has no missing management link')
    const before=await counts();await close();await open('来源字段说明',true);await pause(350);assert.deepEqual(await counts(),before,'reading performs no compute/acquire/preview/write')
    await close();await ev(`[...document.querySelectorAll('button')].find(e=>e.textContent==='高级编辑').click()`);await pause(400)
    await open('所选字段说明');await text('only valid samples');await close()
    await ev(`[...document.querySelectorAll('button')].find(e=>e.textContent==='简洁绘图').click()`)
    if(label==='memory.rows'){
      await select('横轴','f0');await select('图表','line');await select('时间分桶','day');await draw('day line')
      await open('图表口径说明');assert.ok((await raw()).includes('时间桶'))
      await select('时间分桶','month');await draw('month line')
    }
    await select('图表','point');await select('横轴','f3');await select('纵轴','f1');await draw(label+' scatter')
    await select('图表','histogram');await select('数值字段','f1');await draw(label+' histogram')
    await open('所选字段说明');assert.ok((await data()).semantic_context.fields.some(field=>field.output_name==='f1'),'histogram selection includes its input rather than its generated bin/count columns')
    await open('图表口径说明');assert.ok((await raw()).includes('直方图'))
    assert.ok((await data()).reading.fields.some(field=>field.result_unit==='个'))
    if(label==='dataframe.sales'){
      const old=await ev('document.querySelector("[data-snapshot-id]").dataset.snapshotId')
      await setMeaning();await close()
      await ev(`[...document.querySelectorAll('button')].find(e=>e.textContent==='刷新来源').click()`);await pause(500)
      await wait(()=>ev('!!document.querySelector(".quick-controls")'),'source refresh')
      await open('来源字段说明');assert.ok((await raw()).includes('browser captured meaning'));await close()
      await ev(`[...document.querySelectorAll('button')].find(e=>e.textContent==='获取最新快照').click()`)
      await wait(async()=>(await ev('document.querySelector("[data-snapshot-id]").dataset.snapshotId'))!==old,'new snapshot')
      await open('来源字段说明');await text('New browser frame meaning')
    }
  }
  assert.deepEqual(b.exceptions,[])
  await writeFile(`${evidence}/${mode}.png`,Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  console.log('PASS',mode,'JSON modal source/selected/chart, full declarations and exact captured versions, keyboard/security, no reads, unchanged canvas, chart modes and immutable snapshots')
} catch(error){
  await writeFile(`${evidence}/${mode}-failure.txt`,error.stack+'\n'+await body())
  await writeFile(`${evidence}/${mode}-failure.png`,Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));throw error
} finally {await b.close()}
