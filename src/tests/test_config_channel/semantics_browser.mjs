import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence] = process.argv.slice(2)
const b = await openBrowser(base)
const { evaluate: ev, wait, send } = b
const body = () => ev('document.body.innerText')
const idle = () => wait(() => ev('![...document.querySelectorAll("button")].find(e=>e.textContent==="搜索")?.disabled'), 'UI idle')
const click = async text => { await idle(); await ev(`(()=>{const e=[...document.querySelectorAll('button')].find(e=>e.textContent.trim()===${JSON.stringify(text)});if(!e || e.disabled)throw Error('button unavailable: '+${JSON.stringify(text)});e.click()})()`); await idle() }
const set = async (label, value, scope = '') => { await ev(`(()=>{const e=document.querySelector(${JSON.stringify(scope + '[aria-label="' + label + '"]')});if(!e||e.disabled)throw Error('input unavailable '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)}
const expectText = text => wait(async () => (await body()).includes(text), text)
const api = async (op, payload, status=200) => { const r = await fetch(base + '/api/semantics/v1/' + op, { method:'POST', headers:{'Content-Type':'application/json'},body:JSON.stringify(payload) }); const d = await r.json(); assert.equal(r.status,status,JSON.stringify(d)); return d.data }
const resolve = ref => api('resolve',{exact_reference:ref})
const save = async ref => { await click('保存新版本'); await expectText('定义已保存：'+ref); assert.equal(await ev('document.querySelector("[data-testid=error]")?.textContent || ""'), '') }
try {
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 1000, deviceScaleFactor: 1, mobile: false })
  await send('Page.navigate',{url:base+'/semantics'}); await expectText('新建标准'); await idle()
  await click('新建标准'); await set('名称','ui.duration'); await set('标题','耗时标准')
  await set('定义状态','declared'); await set('定义','请求耗时 <img src=x onerror=alert(1)>')
  await save('standard.ui.duration@1')
  await click('新建字典'); await set('名称','ui.requests'); await set('标题','请求字典'); await click('添加字段')
  await set('字段身份 0','duration'); await set('字段名 0','duration_ms'); await set('字段标题 0','耗时'); await set('类型系统 0','arrow'); await set('类型名称 0','float64')
  assert.match(await ev('document.querySelector("[data-testid=structure-diff]").textContent'), /duration_ms/)
  await ev('document.querySelector("[data-semantics=\\"0\\"]").open=true')
  await set('定义状态','declared','[data-semantics="0"] '); await set('定义','请求经过时间','[data-semantics="0"] ')
  await set('标准版本状态','declared','[data-semantics="0"] '); await set('标准版本','standard.ui.duration@1','[data-semantics="0"] ')
  await save('dictionary.ui.requests@1')
  assert.equal((await resolve('dictionary.ui.requests@1')).document.fields[0].standard_refs.value[0], 'standard.ui.duration@1')
  await send('Page.reload'); await expectText('只读版本'); await idle()
  assert.equal(await ev('document.querySelector("[aria-label=标题]").value'), '请求字典')
  assert.equal(await ev('document.querySelector("[aria-label=\\"字段名 0\\"]").disabled'), true)
  await click('新建绑定'); await set('名称','ui.object'); await set('对象标签','人工对象'); await set('Provider 身份','manual'); await set('数据集身份','ui.requests')
  await set('字典精确版本','dictionary.ui.requests@1'); await set('字段映射','{"duration":"duration_ms"}'); await save('binding.ui.object@1')
  assert.equal((await resolve('binding.ui.object@1')).structure_check, 'unknown')
  await click('请求字典'); await expectText('binding.ui.object@1'); await click('编辑当前版本')
  assert.equal(await ev('document.querySelector("[aria-label=\\"字段名 0\\"]").disabled'),true)
  await set('标题','补充后的字典'); await set('应用绑定','binding.ui.object@1')
  // Another client updates the binding after this page selected its exact version.
  const binding = (await resolve('binding.ui.object@1')).document
  await api('save',{kind:'binding',name:'ui.object',expected_current_revision:1,document:{...binding,object_label:'外部修改'}})
  await click('保存并应用'); await expectText('定义已保存：dictionary.ui.requests@2；绑定未更新：REVISION_CONFLICT')
  assert.equal((await resolve('binding.ui.object@2')).document.dictionary_ref,'dictionary.ui.requests@1')
  await set('应用绑定','binding.ui.object@2'); await click('应用到对象'); await expectText('绑定已更新：binding.ui.object@3')
  assert.equal((await resolve('binding.ui.object@3')).document.dictionary_ref,'dictionary.ui.requests@2')
  // Conflict preserves both title and user-entered semantic content.
  await set('标题','本地尚未保存')
  const current = (await resolve('dictionary.ui.requests@2')).document
  await api('save',{kind:'dictionary',name:'ui.requests',expected_current_revision:2,document:{...current,title:'另一客户端'}})
  await click('保存新版本'); await expectText('REVISION_CONFLICT')
  assert.equal(await ev('document.querySelector("[aria-label=标题]").value'),'本地尚未保存')
  // Real navigation guard prompts; cancel keeps the form.
  let prompted = false
  await ev('window.__confirmCalls=0; window.confirm=()=>{window.__confirmCalls++;return false}')
  await click('新建标准'); prompted = await ev('window.__confirmCalls===1'); assert.ok(prompted)
  assert.equal(await ev('document.querySelector("[aria-label=标题]").value'),'本地尚未保存')
  await ev('window.confirm=()=>true'); await click('dictionary.ui.requests@1'); await expectText('只读版本')
  assert.equal(await ev('document.querySelector("[aria-label=标题]").disabled'),true)
  await click('复制为新修订'); await save('dictionary.ui.requests@4')
  assert.equal((await api('history',{kind:'dictionary',name:'ui.requests'})).items[0].base_revision,1)
  await set('启用结构编辑','on') // select through click below to trigger checkbox semantics
  await ev('document.querySelector("[aria-label=启用结构编辑]").click()')
  await set('字段名 0','renamed_ms'); await expectText('renamed_ms'); await save('dictionary.ui.requests@5')
  await set('交换格式','yaml'); await click('导出当前版本')
  const exported = await ev('document.querySelector("[aria-label=导入导出内容]").value'); assert.match(exported,/source_reference/)
  await click('新建字典'); await set('名称','ui.copy'); await set('交换格式','yaml'); await set('导入导出内容',exported); await click('校验导入预览')
  assert.equal((await api('list',{q:'ui.copy'})).items.length,0)
  await click('采用预览'); await save('dictionary.ui.copy@1')
  assert.deepEqual((await resolve('dictionary.ui.copy@1')).document,(await resolve('dictionary.ui.requests@5')).document)
  await set('搜索关键词','renamed_ms'); await click('搜索'); assert.ok((await body()).includes('ui.copy'))
  await set('搜索关键词',''); await click('搜索'); await click('耗时标准'); await expectText('dictionary.ui.requests@5')
  assert.equal(await ev('document.querySelectorAll("article img").length'),0)
  await click('另存新名称'); await set('名称','ui.standard.copy'); await save('standard.ui.standard.copy@1')
  assert.equal(b.exceptions.length,0,JSON.stringify(b.exceptions))
  const screenshot = await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true})
  await writeFile(evidence+'/page.png',Buffer.from(screenshot.data,'base64'))
  console.log('PASS A6: browser creation, semantics, exact standards, bindings, refresh, structure diff, references/history, save/apply partial success, conflicts, import/export, escaping and leave guard')
} catch (e) {
  await writeFile(evidence + '/failure.txt', String(e.stack) + '\n' + await body())
  throw e
} finally { await b.close() }
