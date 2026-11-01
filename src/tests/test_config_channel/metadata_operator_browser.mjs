import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence, replacement, broken, multi] = process.argv.slice(2)
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
const hold = async path => ev(`(()=>{const f=window.fetch;window.__held=false;window.fetch=async function(u,o){const response=await f.apply(this,arguments);if(String(u).endsWith(${JSON.stringify(path)})){window.fetch=f;window.__held=true;await new Promise(resolve=>window.__release=resolve)}return response}})()`)
const release = () => ev('window.__release()')
const requestLog = () => ev('window.__requests')
const counts = () => ev(`(()=>{const a=window.__requests;return {describe:a.filter(x=>x.url.endsWith('/semantics/v1/describe')).length,rows:a.filter(x=>x.url.includes('/dataframe/preview')||x.url.includes('/database/preview')).length,consume:a.filter(x=>['consume','batch/execute','stream/execute'].some(p=>x.url.includes(p))).length}})()`)
const panel = 'document.querySelector("[data-testid=operator-contract]")'
const value = label => ev('document.querySelector('+JSON.stringify('[aria-label="'+label+'"]')+').value')
const row = name => '[...document.querySelectorAll(".el-table__body tr")].find(r=>r.textContent.includes('+JSON.stringify(name)+'))'
const action = (name,label) => click(label,row(name))
const close = () => click('关闭','[...document.querySelectorAll(".el-dialog__footer")].find(e=>e.getBoundingClientRect().width>0)')
const describe = name => api('semantics/v1/describe',{target:{kind:'operator',name},view:'full'})
const technicalJson = async expected => {
  await wait(()=>ev('!!document.querySelector("[data-testid=contract-full]")'),'full technical JSON')
  assert.equal(await ev('document.querySelectorAll("[data-testid=contract-technical] pre").length'),1,'one complete JSON block')
  assert.equal(await ev('[...document.querySelectorAll("[data-testid=contract-technical] button")].filter(e=>/^复制.*JSON$/.test(e.textContent.trim())).length'),1,'one JSON copy button')
  assert.equal(await ev('document.querySelectorAll("[data-testid=contract-configuration]").length'),0,'no duplicated configuration fragment')
  assert.deepEqual(JSON.parse(await ev('document.querySelector("[data-testid=contract-full]").textContent')),expected)
}
const editNotes = async () => { await wait(()=>ev('!!document.querySelector("[aria-label=算子用途]")'),'inline notes') }
const history = async () => { await click('技术详情（已保存内容）',panel);await click('历史版本',panel) }
const ensureRows = async () => {
  for(const [section,title] of [['inputs','输入字段'],['parameters','参数'],['outputs','输出字段']]) {
    if(!await ev('document.querySelector("[data-testid=description-'+section+'] tbody tr") !== null'))await click('添加'+title,panel)
  }
}
const fillRows = async () => {
  await ensureRows()
  for(const [label,content] of [
    ['输入 1 字段名','latency'],['输入 1 类型','double'],['输入 1 填写要求','required_non_null'],
    ['输入 1 说明','Input latency is measured in milliseconds'],
    ['参数 1 参数名','column'],['参数 1 类型','string'],['参数 1 必填',''],['参数 1 说明','Pass column names as strings'],
    ['输出 1 字段名','result'],['输出 1 说明','Interpret rows according to the output conditions']])await set(label,content)
}
const save = async purpose => {
  await editNotes();await set('算子用途',purpose)
  await fillRows()
  if(!await ev('document.querySelector("[data-testid=usage-conditions]").open'))await click('使用限制',panel)
  await set('算子使用注意','Review actual inputs and limits')
  if(!await ev('document.querySelector("[data-testid=usage-examples]").open'))await click('调用示例',panel)
  await set('算子调用示例','SELECT * FROM dataframe.input USING builtin.passthrough INTO dataframe.out')
  await click('保存说明',panel);await text('说明已保存')
}
const upload = async path => {
  const dom=await send('DOM.getDocument')
  const f=await send('DOM.querySelector',{nodeId:dom.root.nodeId,selector:'input[type=file]'})
  await send('DOM.setFileInputFiles',{nodeId:f.nodeId,files:[path]})
  await text('上传成功')
}
const open = async (name,type='Python 算子') => {
  await click(type);await action(name,type==='Python 算子'?'编辑':'查看')
  if(type==='C++ 插件')await click(name.includes('baseliner')?'explore.baseliner 契约':name==='npm.so'?'npm.basic 契约':'contractdemo.output 契约')
  else {
    await text('说明与契约')
    assert.equal(await ev('!!document.querySelector("[data-testid=operator-contract]")'),false)
    await click('说明与契约')
  }
  await wait(()=>ev('!!document.querySelector("[data-testid=operator-usage]")'),'usage ready')
  assert.equal(await ev('document.querySelectorAll("[data-testid=operator-contract] nav,[data-testid=operator-check],[data-testid=invocation-form],[data-testid=contract-check-result]").length'),0,'contract has no invocation UI')
  for(const section of ['inputs','parameters','outputs'])assert.equal(await ev('document.querySelectorAll("[data-testid=description-'+section+'] table").length'),1,'each section has exactly one field table')
  assert.equal(await ev('document.querySelectorAll(".group-heading,.group-description,.add-group").length'),0,'no group editors or actions')
  assert.equal(await ev('document.querySelectorAll("[aria-label=输入说明与条件],[aria-label=参数说明与条件],[aria-label=输出说明与条件]").length'),0,'no monolithic text editors')
  assert.equal(await ev('document.querySelector("[data-testid=usage-conditions]").open'),false)
  assert.deepEqual(await ev('[...document.querySelectorAll("[data-usage-section]")].map(e=>e.dataset.usageSection)'),['inputs','parameters','outputs'])
  assert.doesNotMatch(await ev('document.querySelector("[data-testid=operator-contract]").innerText'),/尚未声明|编辑补充说明|静态声明随源码发布|更多|调用检查|检查输入与参数|选择实际数据集|通用条件|添加输入组|添加结果组|删除组/)

  if (type==='C++ 插件') assert.ok(await ev('(()=>{const labels=[...document.querySelectorAll(".el-form-item__label")];const info=labels.find(e=>e.textContent.trim()==="错误信息"),list=document.querySelector(".cpp-op-list"),panel=document.querySelector("[data-testid=operator-contract]");return (info.compareDocumentPosition(list)&Node.DOCUMENT_POSITION_FOLLOWING)&&(list.compareDocumentPosition(panel)&Node.DOCUMENT_POSITION_FOLLOWING)})()'))
}
async function emptyContract() {
  await wait(()=>ev('document.querySelectorAll("[data-testid^=description-] table").length===3'),'empty structured tables')
  for(const section of ['inputs','parameters','outputs'])assert.equal(await ev('document.querySelectorAll("[data-testid=description-'+section+'] tbody tr").length'),0)
  assert.doesNotMatch(await ev('document.querySelector("[data-testid=operator-contract]").innerText'),/尚未声明|作者版本|通用条件/)
  await ensureRows()
  assert.equal(await value('输入 1 填写要求'),'');assert.equal(await value('输出 1 允许空值'),'');assert.equal(await value('参数 1 默认值'),'')
  assert.equal(await ev(`document.querySelector('[aria-label="参数 1 默认值"]').disabled`),false)
  assert.equal(await ev('document.querySelectorAll("[data-testid^=description-] input[type=checkbox]").length'),0,'no default declaration toggle')
  assert.equal(await ev('document.querySelectorAll(".group-heading,.group-description,.add-group").length'),0,'no group concept in an empty editor')
  assert.deepEqual(await ev(`[...document.querySelector('[aria-label="输入 1 填写要求"]').options].map(o=>o.textContent)`),['','必填非空','必填可空','可选'])
  await mouse(`document.querySelector('[aria-label="输入 1 字段名"]')`);await send('Input.insertText',{text:'Direct input field'})
  await keyboard('Tab',9);assert.equal(await ev('document.activeElement.getAttribute("aria-label")'),'输入 1 类型')
  await send('Input.insertText',{text:'double'})
  await click('添加输入字段',panel);await set('输入 2 字段名','discarded')
  await mouse(`document.querySelector('[aria-label="删除输入 2"]')`)
  assert.equal(await ev('document.querySelectorAll("[data-testid=description-inputs] tbody tr").length'),1)
  assert.equal(await value('输入 1 字段名'),'Direct input field')
  await set('参数 1 类型','json');assert.equal(await value('参数 1 类型'),'json')
  await set('参数 1 默认值','""');assert.equal(await value('参数 1 默认值'),'""')
  await fillRows();await contractLayout()
  await writeFile(evidence+'/structured-empty-edited.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await click('恢复已保存内容',panel)
  for(const section of ['inputs','parameters','outputs'])assert.equal(await ev('document.querySelectorAll("[data-testid=description-'+section+'] tbody tr").length'),0)
}

async function layout() {
  const r=await ev('(()=>{const t=document.querySelector(".el-table"),h=[...t.querySelectorAll(".el-table__header-wrapper th")],d=[...t.querySelector(".el-table__body tr").children];return {overflow:document.documentElement.scrollWidth>innerWidth+1,aligned:h.length===d.length&&h.every((e,i)=>Math.abs(e.getBoundingClientRect().width-d[i].getBoundingClientRect().width)<2),buttons:[...t.querySelector(".el-table__body tr").querySelectorAll("button")].map(e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height}})}})()')
  assert.ok(!r.overflow && r.aligned,JSON.stringify(r))
  for(let i=0;i<r.buttons.length;i++)for(let j=i+1;j<r.buttons.length;j++){const a=r.buttons[i],c=r.buttons[j];assert.ok(a.x+a.w<=c.x+1||c.x+c.w<=a.x+1||a.y+a.h<=c.y+1||c.y+c.h<=a.y+1)}
}
const keyboard = async (key, code, modifiers=0) => {
  await send('Input.dispatchKeyEvent',{type:'keyDown',key,windowsVirtualKeyCode:code,nativeVirtualKeyCode:code,modifiers,...(key==='Enter'?{code:'Enter',text:'\r',unmodifiedText:'\r'}:{})})
  await send('Input.dispatchKeyEvent',{type:'keyUp',key,windowsVirtualKeyCode:code,nativeVirtualKeyCode:code,modifiers})
}
const accessibilityMatrix = []
const dropdownThemes = []
const contrast = (foreground,background) => {
  const luminance = color => {
    const channels=color.match(/[\d.]+/g).map(Number)
    assert.ok(channels.length===3 || channels[3]===1,'control colors must be opaque: '+color)
    return channels.slice(0,3).map(value=>value/255).map(value=>value<=0.04045?value/12.92:((value+0.055)/1.055)**2.4).reduce((sum,value,index)=>sum+value*[0.2126,0.7152,0.0722][index],0)
  }
  const a=luminance(foreground),c=luminance(background)
  return (Math.max(a,c)+0.05)/(Math.min(a,c)+0.05)
}
async function dropdownTheme() {
  const theme=await ev('document.documentElement.classList.contains("dark") ? "dark" : "light"')
  const controls=await ev(`(()=>{return [...${panel}.querySelectorAll('.description-rows input,.description-rows textarea,.description-rows select')].map(e=>{const s=getComputedStyle(e);return {tag:e.tagName,label:e.getAttribute('aria-label'),color:s.color,background:s.backgroundColor,scheme:s.colorScheme,options:[...e.querySelectorAll('option')].map(o=>{const s=getComputedStyle(o);return {text:o.textContent,color:s.color,background:s.backgroundColor}})}})})()`)
  assert.equal(controls.filter(control=>control.tag==='SELECT').length,4,'all four dropdowns are covered')
  for(const control of controls) {
    assert.ok(contrast(control.color,control.background)>=4.5,JSON.stringify({theme,control}))
    for(const option of control.options)assert.ok(contrast(option.color,option.background)>=4.5,JSON.stringify({theme,option}))
    if(control.tag==='SELECT')assert.equal(control.scheme,theme,'native dropdown follows the current theme')
  }
  const opened=[]
  if(await ev('innerWidth===1440')) {
    for(const control of controls.filter(control=>control.tag==='SELECT')) {
      const selector=JSON.stringify('[aria-label="'+control.label+'"]')
      const original=await value(control.label)
      const next=await ev(`(()=>{const e=document.querySelector(${selector});return e.options[Math.min(e.selectedIndex+1,e.options.length-1)].value})()`)
      await mouse('document.querySelector('+selector+')')
      await wait(()=>ev('document.querySelector('+selector+').matches(":open")'),'native dropdown opens: '+control.label)
      assert.equal(await ev('document.activeElement.getAttribute("aria-label")'),control.label)
      await writeFile(evidence+'/dropdown-'+theme+'-'+control.label+'.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
      await keyboard('ArrowDown',40);await keyboard('Enter',13)
      await wait(async()=>await value(control.label)===next,'keyboard selection: '+control.label)
      assert.equal(await ev('document.querySelector('+selector+').matches(":open")'),false)
      await set(control.label,original)
      opened.push(control.label)
    }
  }
  dropdownThemes.push({theme,width:await ev('innerWidth'),controls,opened})
}
async function contractLayout() {
  const dimensions = await ev(`(()=>{const dialog=[...document.querySelectorAll('.el-dialog')].find(e=>e.getBoundingClientRect().width>0),panel=${panel},r=panel.getBoundingClientRect(),d=dialog.getBoundingClientRect();return {pageOverflow:document.documentElement.scrollWidth>innerWidth+1,panelLeft:r.left,panelRight:r.right,dialogLeft:d.left,dialogRight:d.right,width:innerWidth}})()`)
  assert.ok(!dimensions.pageOverflow && dimensions.panelLeft>=0 && dimensions.panelRight<=dimensions.width+1 && dimensions.dialogLeft>=0 && dimensions.dialogRight<=dimensions.width+1,JSON.stringify(dimensions))
  const actions = await ev(`(()=>{return [...${panel}.querySelectorAll('.description-rows .actions button')].map(e=>{const range=document.createRange();range.selectNodeContents(e);return {lines:range.getClientRects().length,right:e.getBoundingClientRect().right,cellRight:e.closest('td').getBoundingClientRect().right}})})()`)
  assert.ok(actions.every(action=>action.lines===1 && action.right<=action.cellRight+1),'row delete buttons stay on one line within their cells')
  const tables=await ev(`(()=>{return [...${panel}.querySelectorAll('.description-rows table')].map(table=>({section:table.closest('[data-testid]').dataset.testid,headers:[...table.querySelectorAll('th')].map(e=>e.textContent.trim()),rows:[...table.querySelectorAll('tbody tr')].map(row=>({cells:row.children.length,controls:[...row.children].map(cell=>cell.querySelectorAll('input,select,textarea,button').length),aligned:[...row.children].every((cell,i)=>Math.abs(cell.getBoundingClientRect().width-table.querySelectorAll('th')[i].getBoundingClientRect().width)<2)}))}))})()`)
  assert.equal(tables.length,3,'one editable table for each of inputs, parameters and outputs')
  for(const table of tables){
    const headers=table.section==='description-parameters'?['参数名','类型','必填','默认值','说明','操作']:table.section==='description-inputs'?['字段名','类型','填写要求','说明','操作']:['字段名','类型','允许空值','说明','操作']
    assert.deepEqual(table.headers,headers,'one configuration field per column')
    for(const row of table.rows){assert.equal(row.cells,headers.length);assert.ok(row.controls.every(count=>count===1));assert.ok(row.aligned,'headers and controls align')}
  }

}
async function keyboardNotes() {
  await ensureRows()
  for (const [label, value] of [['输入 1 说明','Keyboard input conditions'],['参数 1 说明','Keyboard parameter usage'],['输出 1 说明','Keyboard output meaning']]) {
    await mouse('document.querySelector('+JSON.stringify('[aria-label="'+label+'"]')+')')
    await keyboard('a',65,2)
    await send('Input.insertText',{text:value})
    assert.equal(await ev('document.activeElement.getAttribute("aria-label")'),label)
    assert.equal(await ev('document.activeElement.value'),value)
  }
  await contractLayout()
  await dropdownTheme()
  await click('恢复已保存内容',panel)
}

try {
  const matrixRegistry = await api('semantics/v1/list',{})
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
    for(const dark of [false,true]) {
      if(await ev('document.documentElement.classList.contains("dark")') !== dark) await mouse('document.querySelector("[title=切换主题]")')
      const zoom = dark ? 1.1 : 1
      await ev('document.documentElement.style.zoom='+JSON.stringify(String(zoom)))
      for(const [name,type] of [['chisquare','Python 算子'],['passthrough','内置算子'],['baseliner.so','C++ 插件']]) {
        await open(name,type);await contractLayout()
        if(type==='Python 算子') await keyboardNotes()
        if(type==='C++ 插件') {
          assert.ok((await value('输出说明')).includes('仅配置 model_output 时产生'),'conditional model output is directly editable')
          await mouse('document.querySelector("[data-testid=usage-conditions] > summary")')
          assert.equal(await ev('document.querySelector("[data-testid=usage-conditions]").open'),true,'usage restrictions expand with a real mouse click')
          await contractLayout()
        }
        await writeFile(evidence+'/panel-'+width+'-'+(dark?'dark':'light')+'-'+type+'.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
        accessibilityMatrix.push({width,height,theme:dark?'dark':'light',zoom,type,realMouse:true,keyboardNotes:type==='Python 算子'})
        await close()
      }
      await ev('document.documentElement.style.zoom="1"')
    }
  }
  assert.equal(accessibilityMatrix.length,18)
  assert.deepEqual(await api('semantics/v1/list',{}),matrixRegistry,'reading and keyboard edits do not publish registry revisions')
  assert.equal((await counts()).rows,0);assert.equal((await counts()).consume,0)
  await click('Python 算子');await click('新建算子')
  await set('新算子名称','browser.disposable')
  await set('新算子源码','from flowsql import OperatorBase, register_operator\n@register_operator(category="browser",name="disposable")\nclass Disposable(OperatorBase):\n def work(self,df): return df\n')
  await click('保存并激活');await text('算子已激活')
  await action('disposable','查看');await text('说明与契约')
  assert.equal(await ev('!!document.querySelector("[data-testid=operator-contract]")'),false,'details start collapsed')
  await click('说明与契约');await emptyContract();await close()
  await action('disposable','去激活');await action('disposable','删除');await text('算子 browser.disposable 已删除')
  const beforeRead = (await requestLog()).length
  const registryBefore = await api('semantics/v1/list',{})
  await open('chisquare');await text('col1');await text('分类字段');await text('汇总与明细混合')
  assert.equal((await counts()).rows,0);assert.equal((await counts()).consume,0)
  const readingRequests = (await requestLog()).slice(beforeRead)
  assert.equal(readingRequests.filter(r=>r.url.includes('/history')||r.url.includes('/resolve')||r.url.includes('/channels/dataframe')||JSON.parse(r.body||'{}').view==='full').length,0,'reading requests usage only')
  assert.ok(await ev('document.querySelector(".code-editor-container").compareDocumentPosition(document.querySelector(".contract-entry")) & Node.DOCUMENT_POSITION_FOLLOWING'))
  const originalCode = await value('编辑算子源码')
  const sourceDraft = originalCode + '\n# source remains independent\n'
  await set('编辑算子源码',sourceDraft)
  await editNotes();await set('算子用途','Local notes draft before close')
  await click('说明与契约');await click('说明与契约')
  assert.equal(await value('编辑算子源码'),sourceDraft)
  assert.equal(await value('算子用途'),'Local notes draft before close')
  await click('最大化查看')
  assert.equal(await ev('document.querySelector(".contract-entry").getBoundingClientRect().height'),0)
  assert.equal(await value('编辑算子源码'),sourceDraft)
  await click('退出最大化')
  assert.ok(await ev('document.querySelector(".contract-entry").getBoundingClientRect().height>0'))
  await close();await open('legacy');await close();await open('chisquare')
  assert.equal(await value('编辑算子源码'),sourceDraft)
  await editNotes();assert.equal(await value('算子用途'),'Local notes draft before close')
  await click('恢复已保存内容');await editNotes();assert.equal(await value('算子用途'),'')
  await click('恢复已保存内容')
  assert.deepEqual(await api('semantics/v1/list',{}),registryBefore,'reading and drafts do not publish versions')
  const ordinary=await describe('explore.chisquare')
  assert.equal(ordinary.contract.document.operator.version,'1')
  await hold('/semantics/v1/describe');await click('技术详情（已保存内容）')
  await wait(()=>ev('window.__held'),'full response held')
  assert.equal(await ev('document.querySelectorAll("[data-testid=contract-technical] pre").length'),0,'usage is not shown while full JSON loads')
  assert.equal(await ev('[...document.querySelectorAll("[data-testid=contract-technical] button")].some(e=>/复制.*JSON/.test(e.textContent))'),false)
  await release();await technicalJson(ordinary)
  await save('Chi-square usage note');assert.equal((await api('operators/detail',{name:'explore.chisquare'})).content,originalCode)
  await send('Browser.grantPermissions',{origin:base,permissions:['clipboardReadWrite','clipboardSanitizedWrite']})
  if(!await ev('document.querySelector("[data-testid=usage-examples]").open'))await click('调用示例',panel);await click('复制示例',panel);await text('已复制')
  assert.equal(await ev('navigator.clipboard.readText()'),'SELECT * FROM dataframe.input USING builtin.passthrough INTO dataframe.out')
  await set('算子用途','Unsaved purpose excluded from JSON');await set('输入 1 说明','Unsaved input conditions excluded from JSON')
  await click('技术详情（已保存内容）',panel);await technicalJson(await describe('explore.chisquare'));await click('复制 JSON',panel)
  const copied = JSON.parse(await wait(async()=>{const value=await ev('navigator.clipboard.readText()');return value.startsWith('{') ? value : null},'copied saved JSON'))
  assert.deepEqual(copied,await describe('explore.chisquare'),'copied complete JSON excludes all unsaved drafts')
  await ev('document.querySelector("[data-testid=contract-full]").scrollIntoView({block:"center"})')
  await writeFile(evidence+'/single-current-json.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  // A saving response must preserve edits made while it was in flight.
  await editNotes();await set('算子用途','Saved before slow response');await hold('/operators/save-description')
  await ev('[...document.querySelectorAll("button")].find(e=>e.textContent.trim()==="保存说明").click()')
  await wait(()=>ev('window.__held'),'save response held');await set('算子用途','Typed during save response');await set('输入 1 说明','Input edited during save response');await release()
  await wait(()=>ev('![...document.querySelectorAll("[data-testid=operator-contract] button")].find(e=>e.textContent.trim()==="保存说明").disabled'),'save completed')
  assert.equal(await value('算子用途'),'Typed during save response')
  assert.equal(await value('输入 1 说明'),'Input edited during save response')
  assert.equal((await describe('explore.chisquare')).description.notes.purpose,'Saved before slow response')
  assert.equal((await describe('explore.chisquare')).description.notes.inputs[0].description,'Unsaved input conditions excluded from JSON','saving captures flat rows before later typing')
  await click('恢复已保存内容')
  await set('算子用途','Unsaved notes survive refresh');await click('刷新',panel)
  assert.equal(await value('算子用途'),'Unsaved notes survive refresh')
  await click('恢复已保存内容',panel)
  await close()
  await open('passthrough','内置算子');await save('Pass through existing input')
  await close()
  // Twelve real author releases, prepared by the harness, require two pages.
  await open('history');await history()
  await wait(()=>ev('document.querySelectorAll("[data-testid=contract-history] button").length===12'),'one history page')
  let historyRequests = (await requestLog()).filter(r=>r.url.endsWith('/history')).map(r=>JSON.parse(r.body))
  assert.ok(historyRequests.every(r=>r.limit===10 && !r.cursor))
  await click('更多历史版本');await wait(()=>ev('!document.body.innerText.includes("更多历史版本")'),'final history page')
  historyRequests = (await requestLog()).filter(r=>r.url.endsWith('/history')).map(r=>JSON.parse(r.body))
  assert.ok(historyRequests.at(-1).cursor)
  await click('operator_contract.browser.history@1');await text('正在查看历史声明');await text('History fixture release 1')
  assert.ok(await ev('[...document.querySelectorAll("button")].find(e=>e.textContent.trim()==="保存说明").disabled'))
  assert.equal(await ev('document.querySelectorAll("[data-testid^=description-]").length'),0,'historical author record excludes current local table drafts')
  const historical=await api('semantics/v1/resolve',{exact_reference:'operator_contract.browser.history@1'})
  await click('技术详情（已保存内容）',panel);await technicalJson(historical)
  await ev('navigator.clipboard.writeText("")');await click('复制 JSON',panel)
  assert.deepEqual(JSON.parse(await wait(async()=>{const value=await ev('navigator.clipboard.readText()');return value.startsWith('{') ? value : null},'copied historical JSON')),historical,'copy preserves exact historical record')
  await ev('document.querySelector("[data-testid=contract-full]").scrollIntoView({block:"center"})')
  await writeFile(evidence+'/single-historical-json.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await click('返回当前版本');await text('History fixture release 12')
  await click('技术详情（已保存内容）',panel);await technicalJson(await describe('browser.history'));await close()
  for(const [file,name] of [['baseliner.so','explore.baseliner'],['npm.so','npm.basic']]) {
    await open(file,'C++ 插件')
    const full=await describe(name),doc=full.contract.document
    assert.equal(await value('算子用途'),doc.description)
    assert.equal(await value('输入说明'),doc.inputs[0].description)
    assert.equal(await value('参数说明'),doc.configuration.description)
    const outputText=doc.outputs.map(o=>o.port_id+'：'+o.description).join('\n\n')
    assert.equal(await value('输出说明'),outputText)
    assert.equal(await ev('document.querySelectorAll("[data-testid=author-purpose],[data-testid=author-input],[data-testid=author-parameters],.author-output").length'),0,'author text appears once inside the editor')
    assert.equal(await ev('document.querySelectorAll(".declaration .role,.declaration .contract-table,.declaration [data-resource-pointer]").length'),0,'complex author declarations stay out of the default view')
    assert.equal(await ev('document.querySelectorAll("[data-testid=contract-full]").length'),0,'full declaration is loaded on demand')
    await ev('document.querySelector("[data-testid=operator-contract]").scrollIntoView({block:"start"})')
    await writeFile(evidence+'/'+name+'-compact.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
    await click('使用限制',panel);assert.equal(await value('算子使用注意'),doc.limitations.join('\n\n'))
    await click('调用示例',panel);assert.equal(await value('算子调用示例'),doc.examples.join('\n\n'))
    await click('技术详情（已保存内容）',panel);await technicalJson(full)
    const raw=await ev('document.querySelector("[data-testid=contract-full]").textContent')
    if(name==='explore.baseliner')for(const fact of ['row_cases','must_be_null','context_semantics','normalized_config_schema'])assert.ok(raw.includes(fact))
    else for(const fact of ['basic_ipv6','dns_transaction_ipv4','managed_results','capture_v2','module_parameters'])assert.ok(raw.includes(fact))
    const updatedPurpose=doc.description+' 已维护',updatedInput=doc.inputs[0].description+' 已维护'
    await set('算子用途',updatedPurpose);await set('输入说明',updatedInput);await set('参数说明','');await set('输出说明',outputText+'\n补充结果说明')
    await set('算子使用注意','');await set('算子调用示例','')
    await click('保存说明',panel);await text('说明已保存')
    const saved=await describe(name)
    assert.deepEqual(saved.contract,full.contract,'editing descriptions never rewrites the author machine contract')
    assert.equal(saved.description.notes.purpose,updatedPurpose)
    assert.equal(saved.description.notes.input_description,updatedInput)
    assert.equal(saved.description.notes.parameter_description,'')
    assert.equal(saved.description.notes.output_description,outputText+'\n补充结果说明')
    assert.equal(saved.description.notes.usage_notes,'');assert.deepEqual(saved.description.notes.examples,[])
    await close();await open(file,'C++ 插件')
    assert.equal(await value('算子用途'),updatedPurpose);assert.equal(await value('输入说明'),updatedInput)
    assert.equal(await value('参数说明'),'');assert.equal(await value('输出说明'),outputText+'\n补充结果说明')
    await click('使用限制',panel);assert.equal(await value('算子使用注意'),'')
    await click('调用示例',panel);assert.equal(await value('算子调用示例'),'')
    await set('算子用途','');await click('保存说明',panel);await text('说明已保存')
    await close();await open(file,'C++ 插件');assert.equal(await value('算子用途'),'','explicitly cleared purpose stays empty')
    await close()
  }
  await open('chisquare');await editNotes();await set('算子用途','Retained conflict draft')
  const d=await describe('explore.chisquare')
  await api('semantics/v1/operators/save-description',{name:'explore.chisquare',expected_component_revision:d.basis.component_revision,expected_description_ref:d.basis.description_ref,notes:{purpose:'External notes',usage_notes:'',examples:[],inputs:[],parameters:[],outputs:[]}})
  await click('保存说明');await text('REVISION_CONFLICT')
  assert.equal(await value('算子用途'),'Retained conflict draft')
  await click('核对当前组件后继续编辑');await click('保存说明');await text('说明已保存')
  assert.equal((await describe('explore.chisquare')).description.notes.purpose,'Retained conflict draft')
  await editNotes();await set('算子用途','Retained conflict draft')
  await close()
  const before=await api('operators/detail',{name:'explore.chisquare'})
  const changed=before.content.replace('"version": "1"','"version": "2"')
  assert.notEqual(changed,before.content)
  await api('operators/update',{name:'explore.chisquare',content:changed,expected_component_revision:before.component_revision})
  const next=await describe('explore.chisquare')
  assert.notEqual(next.basis.component_revision,d.basis.component_revision);assert.equal(next.description.notes,null)
  await open('chisquare');await text('组件或说明版本已变化');await editNotes();assert.equal(await value('算子用途'),'Retained conflict draft')
  await close()
  const current=await api('operators/detail',{name:'explore.chisquare'})
  await api('operators/update',{name:'explore.chisquare',content:'this is invalid Python @@@',expected_component_revision:current.component_revision},400)
  assert.equal((await describe('explore.chisquare')).basis.component_revision,next.basis.component_revision)
  // A fresh browser session must see only current notes; explicit saving uses current component basis.
  await ev('sessionStorage.removeItem("flowsql.operator-description.inline-editors.explore.chisquare")')
  await open('chisquare');await save('Current chi-square v2 notes');await close()
  await open('legacy');await emptyContract();await writeFile(evidence+'/empty-contract.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));await save('Legacy inline contract notes')
  await set('输入 1 说明','Primary input: latency in milliseconds; nonempty rows')
  await click('添加输入字段',panel)
  await set('输入 2 字段名','latency');await set('输入 2 填写要求','optional');await set('输入 2 说明','Reference input: latency')
  await set('输出 1 说明','Results output: result; only when produced')
  await click('添加输出字段',panel)
  await set('输出 2 字段名','result');await set('输出 2 允许空值','true');await set('输出 2 说明','Model output: only when enabled; fields depend on configuration')
  await set('参数 1 类型','json');await set('参数 1 默认值','""')
  await contractLayout();await click('保存说明',panel)
  await wait(async()=>{const notes=(await describe('browser.legacy')).description.notes;return notes.inputs.length===2 && notes.outputs.length===2 && notes.parameters[0].type==='json' && notes.parameters[0].default==='""'},'flat fields and JSON type/default saved')
  const savedFlat=(await describe('browser.legacy')).description.notes
  assert.deepEqual(savedFlat.inputs,[{name:'latency',type:'double',requirement:'required_non_null',description:'Primary input: latency in milliseconds; nonempty rows'},{name:'latency',type:'',requirement:'optional',description:'Reference input: latency'}])
  assert.deepEqual(savedFlat.outputs,[{name:'result',type:'',nullable:null,description:'Results output: result; only when produced'},{name:'result',type:'',nullable:true,description:'Model output: only when enabled; fields depend on configuration'}])
  await close();await open('legacy')
  assert.equal(await value('输入 1 说明'),'Primary input: latency in milliseconds; nonempty rows')
  assert.equal(await value('输入 2 字段名'),'latency');assert.equal(await value('输入 2 说明'),'Reference input: latency')
  assert.equal(await value('输入 2 填写要求'),'optional');assert.equal(await value('输出 2 字段名'),'result')
  assert.equal(await value('输出 2 允许空值'),'true');assert.equal(await value('输出 2 说明'),'Model output: only when enabled; fields depend on configuration')
  assert.equal(await value('参数 1 默认值'),'""');assert.equal(await value('参数 1 类型'),'json')
  await ev('document.querySelector("[data-testid=description-inputs]").scrollIntoView({block:"start"})')
  await writeFile(evidence+'/flat-saved-inputs.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await ev('document.querySelector("[data-testid=description-outputs]").scrollIntoView({block:"start"})')
  await writeFile(evidence+'/flat-saved-outputs.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await ev('document.querySelector("[data-testid=description-parameters]").scrollIntoView({block:"start"})')
  await writeFile(evidence+'/compact-saved-parameters.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'))
  await mouse(`document.querySelector('[aria-label="删除输入 2"]')`)
  await mouse(`document.querySelector('[aria-label="删除输出 2"]')`)
  await set('参数 1 默认值','');await click('保存说明',panel)
  await wait(async()=>{const notes=(await describe('browser.legacy')).description.notes;return notes.inputs.length===1 && notes.outputs.length===1 && notes.parameters[0].default===''},'field deletion and undeclared default saved')
  assert.equal((await describe('browser.legacy')).contract.state,'missing');await close()
  await click('C++ 插件');await upload(multi);await action('libfixture_cpp_operator_v2.so','激活');await text('算子 libfixture_cpp_operator_v2.so 已激活')
  await action('libfixture_cpp_operator_v2.so','查看');await click('fixture.echo 契约');await emptyContract();await save('C++ missing contract notes');assert.equal((await describe('fixture.echo')).contract.state,'missing')
  assert.equal(await ev('document.querySelector("[data-testid=operator-contract] h3").textContent'),'fixture.echo')
  await hold('/semantics/v1/describe');await click('刷新',panel);await wait(()=>ev('window.__held'),'old object read held')
  await click('fixture.transform 契约');await wait(()=>ev('document.querySelector("[data-testid=operator-contract] h3").textContent==="fixture.transform"'),'selected C++ identity');await release();await pause(100);assert.equal(await ev('document.querySelector("[data-testid=operator-contract] h3").textContent'),'fixture.transform')
  await close()
  await open('output.so','C++ 插件');await save('Original C++ notes')
  const old=await describe('contractdemo.output')
  await close();await upload(replacement)
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
  const unwanted=(await requestLog()).filter(r=>r.url.includes('/operators/check')||r.url.includes('/semantics/v1/channels/')||['/channels/dataframe','/channels/database','/channels/kafka'].some(path=>r.url.includes(path)))
  assert.deepEqual(unwanted,[],'contract reading/editing never requests input sources, structures or preflight')
  assert.deepEqual(b.exceptions,[])
  assert.equal(dropdownThemes.length,6)
  assert.equal(dropdownThemes.flatMap(result=>result.opened).length,8,'four opened dropdowns in both themes')
  await writeFile(evidence+'/result.json',JSON.stringify({realMouse:true,accessibilityMatrix,dropdownThemes,viewports:[1280,1440,1920],requests:await counts(),defaultReading:readingRequests.map(r=>({url:r.url,body:JSON.parse(r.body||'{}')})),old:old.basis,updated:updated.basis},null,2))
  console.log('PASS: flat local fields and textual ownership, ordinary/complex author contracts, exact HTTP parity, real mouse layouts, notes/conflicts, Python/C++ author updates and component isolation')
} catch(e) {
  await writeFile(evidence+'/failure-requests.json',JSON.stringify(await requestLog(),null,2))
  await writeFile(evidence+'/failure.txt',e.stack+'\n'+await body())
  await writeFile(evidence+'/failure.png',Buffer.from((await send('Page.captureScreenshot',{format:'png'})).data,'base64'));throw e
} finally {await b.close()}
