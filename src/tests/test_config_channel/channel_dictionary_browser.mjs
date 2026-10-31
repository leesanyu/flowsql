import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence] = process.argv.slice(2)
const b = await openBrowser(base), { evaluate: ev, wait, send } = b
const body = () => ev('document.body.innerText')
const text = value => wait(async () => (await body()).includes(value), value)
const idle = () => wait(() => ev('![...document.querySelectorAll("button")].some(e=>e.textContent.trim()==="刷新核对" && e.disabled)'), 'channel idle')
const click = async (value, scope='') => { await idle(); await ev(`(async()=>{const e=[...document.querySelectorAll(${JSON.stringify(scope+'button')})].find(e=>e.textContent.trim()===${JSON.stringify(value)});if(!e || e.disabled)throw Error('button unavailable: '+${JSON.stringify(value)});e.click();await new Promise(r=>setTimeout(r,0))})()`); await idle() }
const set = async (label,value,scope='') => ev(`(()=>{const e=document.querySelector(${JSON.stringify(scope+'[aria-label="'+label+'"]')});if(!e||e.disabled)throw Error('input unavailable: '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
const api = async (op,payload,status=200) => { const r=await fetch(base+'/api/semantics/v1/'+op,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const d=await r.json();assert.equal(r.status,status,JSON.stringify(d));return d.data }
const raw = async (path,payload) => {const r=await fetch(base+path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});assert.equal(r.status,200,await r.text())}
const frame={kind:'dataframe',name:'sample'}, kafka={kind:'kafka_source',name:'events'}, table={kind:'database_table',type:'sqlite',name:'sample',table:'empty_table'}
const navigate=async url=>{await ev('window.__navMarker=true');await send('Page.navigate',{url:base+url});await wait(()=>ev('window.__navMarker!==true'),'new navigation');await text(url.startsWith('/semantics')?'通道数据字典':'通道管理');await idle();await ev('window.confirm=()=>true')}
const reload=async()=>{await ev('window.__reloadMarker=true');await send('Page.reload');await wait(()=>ev('window.__reloadMarker!==true'),'new document');await text('通道数据字典');await idle();await wait(()=>ev('!!document.querySelector(\'[aria-label=通道字典标题]\')'),'dictionary loaded')}
// Historical direct links remain supported after object entry points move into details.
const enter=async kind=>{const locator=kind==='dataframe'?frame:kind==='kafka'?kafka:table;await navigate('/semantics?dataset='+encodeURIComponent(JSON.stringify(locator)));await text('通道数据字典');await idle();await ev('window.confirm=()=>true')}

const confirm=async()=>{await idle();const items=await ev('[...document.querySelectorAll("[data-testid=channel-dictionary] input[type=checkbox]")].map(e=>e.value)');for(const item of items){await ev(`(async()=>{const e=[...document.querySelectorAll('[data-testid=channel-dictionary] input[type=checkbox]')].find(e=>e.value===${JSON.stringify(item)});if(!e.checked)e.click();await new Promise(r=>setTimeout(r,0))})()`)}assert.equal(await ev('[...document.querySelectorAll("[data-testid=channel-dictionary] input[type=checkbox]")].every(e=>e.checked)'),true)}
const supplement=async name=>{await set('通道字典名称','ui.'+name);await set('通道绑定名称','ui.'+name);await set('通道字典标题',name+' 字典');await ev('document.querySelector("[data-channel-field=\\"0\\"]").open=true');await set('定义状态','declared','[data-channel-field="0"] ');await set('定义','人工字段说明','[data-channel-field="0"] ');await set('标准版本状态','declared','[data-channel-field="0"] ');await set('标准版本','standard.ui.channel@1','[data-channel-field="0"] ');await confirm();await click('保存字典并应用');await text('绑定已应用：binding.ui.'+name+'@1')}
try {
  await send('Emulation.setDeviceMetricsOverride',{width:1440,height:1000,deviceScaleFactor:1,mobile:false})
  await api('save',{kind:'standard',name:'ui.channel',expected_current_revision:0,document:{schema_id:'flowsql.data-standard.v1',title:'标准',definition:{state:'declared',origin:'manual',value:'字段标准'}}})
  await enter('dataframe');await text('pending');await click('显式加载 DataFrame 结构');await supplement('frame')
  assert.equal((await api('channels/check',{locator:frame})).state,'matched')
  assert.equal((await api('resolve',{exact_reference:'dictionary.ui.frame@1'})).document.fields[0].standard_refs.value[0],'standard.ui.channel@1')
  await reload();await text('matched');assert.equal(await ev('document.querySelector("[aria-label=通道字典标题]").value'),'frame 字典')
  // A save conflict preserves local input and never retries with a newer expected revision.
  await set('通道字典标题','本地未保存')
  const external=(await api('resolve',{exact_reference:'dictionary.ui.frame@1'})).document
  await api('save',{kind:'dictionary',name:'ui.frame',expected_current_revision:1,document:{...external,title:'另一客户端'}})
  await click('保存字典');await text('REVISION_CONFLICT');assert.equal(await ev('document.querySelector("[aria-label=通道字典标题]").value'),'本地未保存')
  // Existing precise dictionary selection and explicit mapping, then a valid new save.
  await ev('window.confirm=()=>true');await click('从来源生成候选');await set('已有字典精确版本','dictionary.ui.frame@2');await click('选择已有版本');await set('通道字典标题','保存成功但应用冲突')
  const binding=(await api('resolve',{exact_reference:'binding.ui.frame@1'})).document
  await api('save',{kind:'binding',name:'ui.frame',expected_current_revision:1,document:{...binding,object_label:'外部客户端'}})
  await click('保存字典并应用');await text('字典已保存：dictionary.ui.frame@3；绑定未更新：REVISION_CONFLICT')
  assert.equal((await api('resolve',{exact_reference:'binding.ui.frame@2'})).document.dictionary_ref,'dictionary.ui.frame@1')
  assert.equal(await ev('document.querySelector("[aria-label=通道字典标题]").value'),'保存成功但应用冲突')
  await click('从来源生成候选');await set('已有字典精确版本','dictionary.ui.frame@3');await click('选择已有版本');await confirm();await click('应用已保存版本');await text('binding.ui.frame@3')
  // All three historical direct links permit generation, semantics, standards and precise binding.
  await enter('database');await supplement('table');await reload();await text('matched')
  await enter('kafka');await supplement('kafka')
  // Producer/type change leaves the old dictionary intact and requires explicit confirmation.
  const q=await fetch(base+'/api/channels/kafka/channels/query',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"name":"events"}'});const row=(await q.json()).items[0]
  row.payload_options.source.fields[0].type='int64';row.payload_options.source.fields[0].unit='s'
  await raw('/api/channels/kafka/channels/modify',{name:row.name,connection_name:row.connection_name,role:row.role,topic:row.topic,consumer_group:row.consumer_group,payload_kind:row.payload_kind,payload_options:row.payload_options,expected_revision:row.revision})
  await click('刷新核对');await text('mismatch');await click('从来源生成候选');await text('type_changed');await text('producer_changed')
  assert.equal(await ev('document.querySelector("[data-channel-field=\\"0\\"] [aria-label=定义]").value'),'人工字段说明')
  await click('保存字典并应用');await text('绑定未更新：CONFIRMATION_REQUIRED');await confirm();await click('应用已保存版本');await text('binding.ui.kafka@2')
  await click('刷新核对');await text('matched')
  // A source failure clears the old observed match and preserves the draft.
  await raw('/api/channels/kafka/channels/remove',{name:'events',expected_revision:2})
  await click('刷新核对');await text('NOT_FOUND');assert.match(await ev('document.querySelector("[data-testid=channel-state]").textContent'),/unavailable/)
  await raw('/api/channels/kafka/channels/add',{name:row.name,connection_name:row.connection_name,role:row.role,topic:row.topic,consumer_group:row.consumer_group,payload_kind:row.payload_kind,payload_options:row.payload_options})
  await click('刷新核对');await text('unbound');await click('从来源生成候选');await text('同名对象已替换');await text('复用旧字典 dictionary.ui.kafka@2')
  assert.equal(b.exceptions.length,0,JSON.stringify(b.exceptions))
  const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});await writeFile(evidence+'/page.png',Buffer.from(shot.data,'base64'))
  console.log('PASS A6: three-source legacy direct links, pending/load, defaults, manual semantics, standards, precise reuse/mapping, refresh, conflicts, partial success, producer/type confirmation and source failure')
} catch(e) { await writeFile(evidence+'/failure.txt',String(e.stack)+'\n'+await body());throw e } finally {await b.close()}
