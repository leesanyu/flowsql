import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence, replacement, broken] = process.argv.slice(2)
const b = await openBrowser(base), { evaluate: ev, wait, send, pause } = b
const body = () => ev('document.body.innerText')
const text = value => wait(async () => (await body()).includes(value), value)
await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__requests=[];const f=window.fetch;window.fetch=function(u,o){window.__requests.push({url:String(u),body:o?.body});return f.apply(this,arguments)};const x=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){this.__url=u;return x.apply(this,arguments)};const s=XMLHttpRequest.prototype.send;XMLHttpRequest.prototype.send=function(body){window.__requests.push({url:this.__url,body});return s.apply(this,arguments)}` })
const find = (label, scope='document') => `[...${scope}.querySelectorAll('button,[role=tab],.el-pager li,.operator-type-item,summary')].find(e=>(e.textContent.trim()===${JSON.stringify(label)} || e.textContent.trim()===${JSON.stringify(({确定:'OK',取消:'Cancel'})[label] || label)} || (e.classList.contains('operator-type-item') && e.textContent.trim().startsWith(${JSON.stringify(label)}))) && !e.disabled && e.getBoundingClientRect().width>0)`
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
const panel = 'document.querySelector("[data-testid=operator-contract]")'
const value = label => ev('document.querySelector('+JSON.stringify('[aria-label="'+label+'"]')+').value')
const row = name => '[...document.querySelectorAll(".el-table__body tr")].find(r=>r.textContent.includes('+JSON.stringify(name)+'))'
const action = (name,label) => click(label,row(name))
const close = () => click('关闭','[...document.querySelectorAll(".el-dialog__footer")].find(e=>e.getBoundingClientRect().width>0)')
const describe = name => api('semantics/v1/describe',{target:{kind:'operator',name}})
const save = async purpose => {
  await set('算子用途',purpose);await set('算子使用注意','Review actual inputs and limits')
  await set('算子调用示例','SELECT * FROM dataframe.input USING builtin.passthrough INTO dataframe.out')
  await click('保存补充说明',panel);await text('补充说明已保存')
}
const upload = async path => {
  const dom=await send('DOM.getDocument')
  const f=await send('DOM.querySelector',{nodeId:dom.root.nodeId,selector:'input[type=file]'})
  await send('DOM.setFileInputFiles',{nodeId:f.nodeId,files:[path]})
  await text('上传成功')
}
const open = async (name,type='Python 算子') => {
  await click(type);await action(name,type==='Python 算子'?'编辑':'查看')
  if(type==='C++ 插件')await click(name.includes('baseliner')?'explore.baseliner 契约':'contractdemo.output 契约')
  await text('输入输出契约')
  await wait(()=>ev('!document.querySelector("[data-testid=operator-description] button").disabled'),'description ready')
}
async function layout() {
  const r=await ev('(()=>{const t=document.querySelector(".el-table"),h=[...t.querySelectorAll(".el-table__header-wrapper th")],d=[...t.querySelector(".el-table__body tr").children];return {overflow:document.documentElement.scrollWidth>innerWidth+1,aligned:h.length===d.length&&h.every((e,i)=>Math.abs(e.getBoundingClientRect().width-d[i].getBoundingClientRect().width)<2),buttons:[...t.querySelector(".el-table__body tr").querySelectorAll("button")].map(e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height}})}})()')
  assert.ok(!r.overflow && r.aligned,JSON.stringify(r))
  for(let i=0;i<r.buttons.length;i++)for(let j=i+1;j<r.buttons.length;j++){const a=r.buttons[i],c=r.buttons[j];assert.ok(a.x+a.w<=c.x+1||c.x+c.w<=a.x+1||a.y+a.h<=c.y+1||c.y+c.h<=a.y+1)}
}
try {
  for(const [width,height] of [[1280,800],[1440,900],[1920,1080]]) {
    await send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false})
    await send('Page.navigate',{url:base+'/operators'});await text('新建算子');await pause(400)
    assert.equal((await counts()).describe,0)
    for(const type of ['Python 算子','内置算子','C++ 插件']) {
      await click(type);await layout()
      await writeFile(evidence+'/'+type+'-'+width+'.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
    }
    await mouse('document.querySelector("[title=切换主题]")');await layout()
    await mouse('document.querySelector("[title=切换主题]")')
    await send('Emulation.setPageScaleFactor',{pageScaleFactor:1.1});await layout();await send('Emulation.setPageScaleFactor',{pageScaleFactor:1})
  }
  await click('Python 算子');await click('新建算子')
  await set('新算子名称','browser.disposable')
  await set('新算子源码','from flowsql import OperatorBase, register_operator\n@register_operator(category="browser",name="disposable")\nclass Disposable(OperatorBase):\n def work(self,df): return df\n')
  await click('保存并激活');await text('算子已激活')
  await action('disposable','查看');await text('该历史算子未声明契约');await close()
  await action('disposable','去激活');await action('disposable','删除');await text('算子 browser.disposable 已删除')
  await open('chisquare');await text('col1');await text('分类字段');await text('汇总与明细混合')
  assert.equal((await counts()).rows,0);assert.equal((await counts()).consume,0)
  const ordinary=await describe('explore.chisquare')
  assert.equal(ordinary.contract.document.operator.version,'1')
  await click('技术详情与完整作者声明 JSON')
  assert.deepEqual(JSON.parse(await ev('document.querySelector("[data-testid=contract-configuration]").textContent')),ordinary.contract.document.configuration)
  await save('Chi-square usage note')
  await click('编辑源码与静态声明');await text('编辑依据')
  await set('已有算法参数','{"col1":"category_a"}');await click('刷新声明与来源列表')
  assert.equal(await value('已有算法参数'),'{"col1":"category_a"}')
  await close()
  await open('passthrough','内置算子');await save('Pass through existing input')
  await set('选择实际数据集','input');await set('已有算法参数','{}');await click('检查实际对象')
  await wait(()=>ev('document.querySelector("[data-testid=contract-check-result]")?.textContent.includes("compatible")'),'preflight')
  await close()
  await open('baseliner.so','C++ 插件')
  for(const output of ['results','model_parameters','relation_fusion','maintenance']) {
    assert.ok(await ev('!!document.querySelector('+JSON.stringify('[data-contract-output="'+output+'"]')+')'))
  }
  await text('不同行类别与 NULL');await text('必要解释上下文')
  await click('结构资源与嵌套配置');await click('task_config');await click('inline_config')
  await text('只满足一个条件分支')
  await writeFile(evidence+'/baseliner.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await close()
  await open('chisquare');await set('算子用途','Retained conflict draft')
  const d=await describe('explore.chisquare')
  await api('semantics/v1/operators/save-description',{name:'explore.chisquare',expected_component_revision:d.basis.component_revision,expected_description_ref:d.basis.description_ref,notes:{purpose:'External notes',usage_notes:'',examples:[]}})
  await click('保存补充说明');await text('REVISION_CONFLICT')
  assert.equal(await value('算子用途'),'Retained conflict draft')
  await close()
  const before=await api('operators/detail',{name:'explore.chisquare'})
  const changed=before.content.replace("'version': '1'","'version': '2'")
  assert.notEqual(changed,before.content)
  await api('operators/update',{name:'explore.chisquare',content:changed,expected_component_revision:before.component_revision})
  const next=await describe('explore.chisquare')
  assert.notEqual(next.basis.component_revision,d.basis.component_revision);assert.equal(next.description.notes,null)
  await open('chisquare');await text('旧说明和本地原稿不会自动套用');assert.equal(await value('算子用途'),'Retained conflict draft')
  await close()
  const current=await api('operators/detail',{name:'explore.chisquare'})
  await api('operators/update',{name:'explore.chisquare',content:'this is invalid Python @@@',expected_component_revision:current.component_revision},400)
  assert.equal((await describe('explore.chisquare')).basis.component_revision,next.basis.component_revision)
  // A fresh browser session must see only current notes; explicit saving uses current component basis.
  await ev('sessionStorage.removeItem("flowsql.operator-description.explore.chisquare")')
  await open('chisquare');await save('Current chi-square v2 notes');await close()
  await open('legacy');await text('该历史算子未声明契约')
  assert.equal((await describe('browser.legacy')).contract.state,'missing');await close()
  await open('output.so','C++ 插件');await save('Original C++ notes')
  const old=await describe('contractdemo.output')
  await click('更新所属插件');await upload(replacement);await close()
  await action('output.so','去激活');await action('output.so','删除');await action('description_cpp.so','激活');await text('算子 description_cpp.so 已激活')
  const updated=await describe('contractdemo.output')
  assert.notEqual(updated.basis.component_revision,old.basis.component_revision)
  assert.equal(updated.contract.document.operator.version,'2');assert.equal(updated.description.notes,null)
  const exact=await api('channels/config/resolve',{exact_reference:old.basis.description_ref})
  assert.equal(JSON.parse(Buffer.from(exact.content_base64,'base64').toString()).notes.purpose,'Original C++ notes')
  await action('description_cpp.so','查看');await click('contractdemo.output 契约');await text('Numerical fixture release two');await close()
  await upload(broken);await action('broken_description.so','激活');await text('激活失败')
  assert.equal((await describe('contractdemo.output')).basis.component_revision,updated.basis.component_revision)
  await action('description_cpp.so','去激活');await action('description_cpp.so','删除')
  assert.deepEqual(b.exceptions,[])
  await writeFile(evidence+'/result.json',JSON.stringify({realMouse:true,viewports:[1280,1440,1920],requests:await counts(),old:old.basis,updated:updated.basis},null,2))
  console.log('PASS: grouped ordinary/complex contracts, exact HTTP parity, real mouse layouts, notes/conflicts, Python/C++ author updates and component isolation')
} catch(e) {
  await writeFile(evidence+'/failure.txt',e.stack+'\n'+await body())
  await writeFile(evidence+'/failure.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));throw e
} finally {await b.close()}
