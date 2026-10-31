import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence] = process.argv.slice(2)
const b = await openBrowser(base), { evaluate: ev, wait, send, pause } = b
const body = () => ev('document.body.innerText')
const text = value => wait(async () => (await body()).includes(value), value)
await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__requests=[];const f=window.fetch;window.fetch=function(u,o){window.__requests.push({url:String(u),body:o?.body});return f.apply(this,arguments)};const x=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){this.__url=u;return x.apply(this,arguments)};const s=XMLHttpRequest.prototype.send;XMLHttpRequest.prototype.send=function(body){window.__requests.push({url:this.__url,body});return s.apply(this,arguments)}` })
const find = (label, scope='document') => `[...${scope}.querySelectorAll('button,[role=tab],.el-pager li,.operator-type-item,.channel-type-item,summary')].find(e=>(e.textContent.trim()===${JSON.stringify(label)} || e.textContent.trim()===${JSON.stringify(({确定:'OK',取消:'Cancel'})[label] || label)} || ((e.classList.contains('operator-type-item') || e.classList.contains('channel-type-item')) && e.textContent.trim().startsWith(${JSON.stringify(label)}))) && !e.disabled && e.getBoundingClientRect().width>0)`
async function mouse(expression) {
  const p = await ev(`(()=>{const e=${expression};if(!e||e.disabled)throw Error('unavailable mouse target');e.scrollIntoView({block:'center',inline:'nearest'});return true})()`)
  assert.ok(p); await pause(160)
  const r = await wait(async () => { const point = await ev(`(()=>{const e=${expression},r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2,h=document.elementFromPoint(x,y);return {x,y,w:r.width,h:r.height,hit:e===h||e.contains(h),visible:x>0&&x<innerWidth&&y>0&&y<innerHeight}})()`)
  return point.hit && point.visible ? point : null }, 'mouse target unobstructed')
  assert.ok(r.hit && r.visible, JSON.stringify(r))
  await send('Input.dispatchMouseEvent',{type:'mouseMoved',x:r.x,y:r.y})
  await send('Input.dispatchMouseEvent',{type:'mousePressed',x:r.x,y:r.y,button:'left',clickCount:1})
  await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:r.x,y:r.y,button:'left',clickCount:1})
  await pause(220)
}
const click = async (label,scope='document') => {await wait(()=>ev(`!!(${find(label,scope)})`),label);await mouse(find(label,scope))}
const set = async (label,value) => ev(`(()=>{const e=document.querySelector(${JSON.stringify('[aria-label="'+label+'"]')});if(!e||e.disabled)throw Error('missing input '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
const api = async (path,payload,status=200) => {const r=await fetch(base+'/api/'+path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const d=await r.json();assert.equal(r.status,status,JSON.stringify(d));return d.data||d}
const counts = () => ev(`(()=>{const a=window.__requests;return {describe:a.filter(x=>x.url.endsWith('/semantics/v1/describe')).length,rows:a.filter(x=>x.url.includes('/dataframe/preview')||x.url.includes('/database/preview')).length,consume:a.filter(x=>['consume','batch/execute','stream/execute'].some(p=>x.url.includes(p))).length}})()`)
const close = async () => mouse('[...document.querySelectorAll(".el-dialog__headerbtn")].find(e=>e.getBoundingClientRect().width>0)')
const layout = async () => {
  const result=await ev('(()=>{const t=[...document.querySelectorAll(".el-table")].find(e=>e.getBoundingClientRect().width>0);if(!t)return {page:document.documentElement.scrollWidth<=innerWidth+1};const h=[...t.querySelectorAll(".el-table__header-wrapper th")],d=[...t.querySelectorAll(".el-table__body tr:first-child td")];return {page:document.documentElement.scrollWidth<=innerWidth+1,aligned:!d.length||h.length===d.length&&h.every((e,i)=>Math.abs(e.getBoundingClientRect().width-d[i].getBoundingClientRect().width)<2)}})()')
  assert.ok(result.page && result.aligned!==false,JSON.stringify(result))
}
const row = name => '[...document.querySelectorAll(".el-table__body tr")].find(r=>r.textContent.includes('+JSON.stringify(name)+'))'
try {
  for(const [width,height] of [[1280,800],[1440,900],[1920,1080]]) {
    await send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false})
    await send('Page.navigate',{url:base+'/channels'});await text('DataFrame 通道');await pause(300);await layout()
    assert.equal(await ev('[...document.querySelectorAll(".nav-menu a")].some(e=>e.getAttribute("href")==="/semantics")'),false)
    assert.equal(await ev('document.querySelector(".nav-menu").textContent.includes("标准与字典")'),false)
    assert.equal((await counts()).describe,0)
    await mouse('[...document.querySelectorAll(".object-name")].find(e=>e.textContent.trim()==="input")')
    await wait(()=>ev('!!document.querySelector("[aria-label=对象说明]:enabled")'),'current channel description');await text('latency')
    assert.ok(await ev(`document.querySelector('[data-description-field="latency"]').innerText.includes('ms')`))
    const canonical=await api('semantics/v1/describe',{target:{kind:'channel',locator:{kind:'dataframe',name:'input'}}})
    await click('技术详情（最近读取结果）')
    const page=await ev('JSON.parse([...document.querySelectorAll("[data-testid=channel-description] pre")].at(-1).textContent)')
    assert.deepEqual(page.basis,canonical.basis);assert.deepEqual(page.description,canonical.description)
    assert.equal((await counts()).rows,0);assert.equal((await counts()).consume,0)
    await close();await layout()
    await send('Page.navigate',{url:base+'/operators'});await text('新建算子');await click('内置算子')
    await click('查看',row('passthrough'));await text('输入输出契约')
    await click('技术详情与完整作者声明 JSON')
    const op=await api('semantics/v1/describe',{target:{kind:'operator',name:'builtin.passthrough'}})
    assert.deepEqual(JSON.parse(await ev('document.querySelector("[data-testid=contract-configuration]").textContent')),op.contract.document.configuration)
    await close();await layout()
    // Existing links remain readable without the navigation item.
    await send('Page.navigate',{url:base+'/semantics?ref='+encodeURIComponent(op.basis.contract_ref)})
    await text('算子契约和使用依据为只读精确记录')
    await send('Page.navigate',{url:base+'/tasks'});await text('SQL 编辑器')
    const sql='SELECT * FROM dataframe.input USING builtin.passthrough'
    await ev('(()=>{const e=document.querySelector(".sql-textarea textarea");e.value='+JSON.stringify(sql)+';e.dispatchEvent(new Event("input",{bubbles:true}))})()')
    await click('执行')
    await wait(()=>ev('!!document.querySelector(".result-section .el-table")'),'inline SQL result table')
    assert.equal(await ev('document.querySelector(".result-section .el-table__header-wrapper").textContent.trim()'),'latency')
    assert.equal(await ev('document.querySelectorAll(".result-section .el-table__body tr").length'),3)
    await text('1000');await text('3000');await layout()
    await writeFile(evidence+'/sql-result-'+width+'.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
    await mouse('document.querySelector("[title=切换主题]")');await layout()
    await mouse('document.querySelector("[title=切换主题]")')
    await send('Emulation.setPageScaleFactor',{pageScaleFactor:1.1});await layout();await send('Emulation.setPageScaleFactor',{pageScaleFactor:1})
    await send('Page.navigate',{url:base+'/channels'});await text('DataFrame 通道');await layout()
  }
  await api('channels/config/publish',{name:'description-config',format:'json',schema_id:'example.v1',content_base64:Buffer.from('{"mode":"example"}').toString('base64'),expected_current_revision:0})
  await click('配置通道');await text('description-config')
  await click('预览',row('description-config'));await text('字段数据说明 unsupported')
  assert.ok(await ev('document.querySelector("[data-testid=config-description-status]").textContent.includes("config.description-config@1")'))
  await close();await layout()
  assert.deepEqual(b.exceptions,[])
  await writeFile(evidence+'/workflow.json',JSON.stringify({viewports:[1280,1440,1920],realMouse:true,legacyRoute:true,sqlRows:3},null,2))
  console.log('PASS: object/HTTP parity, no semantics navigation, old exact links, three viewport SQL result tables, themes/zoom and unsupported configuration identity')
} catch(e) {
  await writeFile(evidence+'/failure.txt',e.stack+'\n'+await body())
  await writeFile(evidence+'/failure.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));throw e
} finally {await b.close()}
