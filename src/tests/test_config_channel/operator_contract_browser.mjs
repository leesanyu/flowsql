import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence, uploadPath] = process.argv.slice(2)
const browser = await openBrowser(base)
const { evaluate: ev, wait, send } = browser
await send('Page.addScriptToEvaluateOnNewDocument',{source:`window.__calls=JSON.parse(sessionStorage.getItem('flowsql.test.calls') || '[]');const f=window.fetch;window.fetch=function(u,o){window.__calls.push(String(u));sessionStorage.setItem('flowsql.test.calls',JSON.stringify(window.__calls));return f.apply(this,arguments)}`})
const body = () => ev('document.body.innerText')
const expectText = text => wait(async () => (await body()).includes(text), text)
const click = async (text, scope = '') => {
  const selector = [scope + ' button', scope + ' summary'].join(',')
  await wait(() => ev(`[...document.querySelectorAll(${JSON.stringify(selector)})].some(e=>e.textContent.trim()===${JSON.stringify(text)}&&!e.disabled)`), text)
  return ev(`(()=>{const e=[...document.querySelectorAll(${JSON.stringify(selector)})].find(e=>e.textContent.trim()===${JSON.stringify(text)});e.click()})()`)
}
const set = async (label, value) => ev(`(()=>{const e=document.querySelector(${JSON.stringify('[aria-label="' + label + '"]')});if(!e)throw Error('missing input '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
const api = async (path, payload, status = 200) => {
  const r = await fetch(base + '/api/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
  const result = await r.json(); assert.equal(r.status, status, JSON.stringify(result)); return result.data || result
}
const contract = {
  schema_id: 'flowsql.operator-data-contract.v1', title: 'Browser declared contract',
  operator: { category: 'browser', name: 'created', version: '1' },
  configuration: { invocation_schema: { type: 'object', properties: { column: {type:'string'}, mode:{type:'string',enum:['a','b','']}, scalar:{type:'string',default:'123'} }, additionalProperties: true }, affects: {} },
  inputs: [{ port_id: 'input', min_count: 1, max_count: 1, shape: 'dataframe', roles: [{
    role_id: 'measure', required: true, types: [{ system: 'arrow', name: 'double', parameters: {} }], nullable: null,
    semantic_requirements: {unit:{code:'ms',label:'milliseconds'}}, selector: { kind: 'parameter', key: 'column' }
  }] }],
  outputs: [{ port_id: 'output', shape: 'dataframe', mode: 'derived', rules: [{ kind: 'copy', source_port: 'input', all_fields: true }] }],
  limitations: ['No row-quality validation']
}
const source = 'from flowsql import OperatorBase, register_operator\n@register_operator(category="browser", name="created")\nclass Created(OperatorBase):\n    def work(self, df):\n        return df\n'
const openRow = async (name, action) => {
  await wait(() => ev(`[...document.querySelectorAll('tr')].some(row=>row.textContent.includes(${JSON.stringify(name)})&&[...row.querySelectorAll('button')].some(button=>button.textContent.trim()===${JSON.stringify(action)}&&!button.disabled))`), name + ' ' + action)
  await ev(`(()=>{const row=[...document.querySelectorAll('tr')].find(row=>row.textContent.includes(${JSON.stringify(name)}));const button=[...row.querySelectorAll('button')].find(button=>button.textContent.trim()===${JSON.stringify(action)});if(!button)throw Error('row action missing');button.click()})()`)
}
const noInvocation = async () => {
  await wait(()=>ev('!!document.querySelector("[data-testid=operator-usage]")'),'contract usage loaded')
  assert.equal(await ev('document.querySelectorAll("[data-testid=operator-check],[data-testid=invocation-form],[data-testid=contract-check-result],[data-testid=operator-contract] nav").length'),0)
  assert.doesNotMatch(await ev('document.querySelector("[data-testid=operator-contract]").innerText'),/调用检查|检查输入与参数|选择实际数据集/)
}

try {
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 1100, deviceScaleFactor: 1, mobile: false })
  await send('Page.navigate', { url: base + '/operators' }); await expectText('新建算子')
  await click('新建算子'); await set('新算子名称', 'browser.created'); await set('新算子源码', source)
  await set('静态契约 JSON', JSON.stringify(contract)); await click('保存并激活')
  await expectText('算子已激活'); await openRow('created', '查看'); await expectText('说明与契约');await click('说明与契约');await expectText('Browser declared contract')
  await expectText('measure');await noInvocation()
  await click('添加输入字段');await set('输入 1 说明','Draft input requirements');await click('刷新')
  assert.equal(await ev(`document.querySelector('[aria-label="输入 1 说明"]').value`),'Draft input requirements')
  await click('关闭', '.el-dialog__footer'); await openRow('created', '去激活'); await openRow('created', '编辑')
  await expectText('源码保存依据')
  const original = await api('operators/detail', { name: 'browser.created' })
  const draft = original.content + '\n# keep local draft\n'
  await set('编辑算子源码', draft)
  const newer = { ...contract, title: 'Updated browser contract', operator: { ...contract.operator, version: '2' } }
  const external = 'from flowsql import OperatorBase, register_operator\n@register_operator(category="browser", name="created", data_contract=__import__("json").loads(' + JSON.stringify(JSON.stringify(newer)) + '))\nclass Created(OperatorBase):\n    def work(self, df):\n        return df\n'
  await api('operators/update', { name: 'browser.created', content: external, expected_component_revision: original.component_revision })
  await click('保存', '.el-dialog__footer'); await expectText('COMPONENT_CHANGED')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'), draft)
  await send('Page.reload'); await expectText('源码保存依据')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'), draft)
  await click('说明与契约');await expectText('Updated browser contract');await noInvocation()
  assert.equal(await ev(`document.querySelector('[aria-label="输入 1 说明"]').value`),'Draft input requirements')
  // A full read from a different basis must fail without showing usage as complete JSON.
  await ev(`(()=>{const f=window.fetch;window.fetch=async function(u,o){const response=await f.apply(this,arguments);if(String(u).endsWith('/semantics/v1/describe')&&JSON.parse(o.body).view==='full'){window.fetch=f;const data=await response.json();(data.data||data).basis.component_revision='changed-during-full-read';return new Response(JSON.stringify(data),{status:response.status,headers:response.headers})}return response}})()`)
  await click('技术详情（已保存内容）');await expectText('版本已变化，请刷新后重读完整契约。')
  assert.equal(await ev('document.querySelectorAll("[data-testid=contract-technical] pre").length'),0)
  assert.equal(await ev('[...document.querySelectorAll("[data-testid=contract-technical] button")].some(e=>/复制.*JSON/.test(e.textContent))'),false)
  await click('技术详情（已保存内容）')
  await click('技术详情（已保存内容）')
  await wait(()=>ev('!!document.querySelector("[data-testid=contract-full]")'),'full read retry')
  assert.deepEqual(JSON.parse(await ev('document.querySelector("[data-testid=contract-full]").textContent')),await api('semantics/v1/describe',{target:{kind:'operator',name:'browser.created'},view:'full'}))
  assert.equal(await ev('document.querySelectorAll("[data-testid=contract-technical] pre").length'),1)
  await click('历史版本');await click('operator_contract.browser.created@1');await expectText('正在查看历史声明')
  await noInvocation()
  assert.equal(await ev('[...document.querySelectorAll("button")].find(e=>e.textContent.trim()==="保存说明").disabled'),true)
  await click('返回当前版本');await expectText('Updated browser contract')
  await click('保存', '.el-dialog__footer');await expectText('COMPONENT_CHANGED')
  await click('核对当前版本后保留源码草稿');await wait(()=>ev('!document.querySelector("[data-testid=operator-edit-error]")'),'source basis reviewed')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'),draft)
  const laterDraft = draft + '\n# typed while source save was in flight\n'
  await ev(`(()=>{[...document.querySelectorAll('.el-dialog__footer button')].find(e=>e.textContent.trim()==='保存').click();const e=document.querySelector('[aria-label=编辑算子源码]');e.value=${JSON.stringify(laterDraft)};e.dispatchEvent(new Event('input',{bubbles:true}))})()`)
  await expectText('提交时的源码已保存')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'),laterDraft)
  assert.equal((await api('operators/detail',{name:'browser.created'})).content,draft)
  await click('核对当前版本后保留源码草稿');await wait(()=>ev('[...document.querySelectorAll(".el-dialog__footer button")].some(e=>e.textContent.trim()==="保存"&&!e.disabled)'),'source basis ready');await click('保存', '.el-dialog__footer')
  await wait(async () => (await api('operators/detail',{name:'browser.created'})).content===laterDraft,'later source draft saved independently')
  await wait(()=>ev('![...document.querySelectorAll(".el-dialog")].some(e=>e.getBoundingClientRect().height>0)'),'source dialog closed')
  await openRow('created','编辑');await expectText('源码保存依据')
  await click('关闭', '.el-dialog__footer')
  const dom = await send('DOM.getDocument')
  const file = await send('DOM.querySelector', { nodeId: dom.root.nodeId, selector: 'input[type=file]' })
  await send('DOM.setFileInputFiles', { nodeId: file.nodeId, files: [uploadPath] })
  await expectText('上传成功'); await openRow('uploaded', '编辑');await click('说明与契约');await expectText('Passthrough')
  await click('关闭', '.el-dialog__footer')
  await ev("[...document.querySelectorAll('.operator-type-item')].find(e=>e.textContent.includes('C++')).click()")
  await openRow('output.so', '查看'); await click('contractdemo.output 契约'); await expectText('secondary')
  await noInvocation()
  await writeFile(evidence + '/contract.png',Buffer.from((await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true})).data,'base64'))
  await click('关闭', '.el-dialog__footer');await openRow('baseliner.so','查看');await click('explore.baseliner 契约');await expectText('Baseliner');await noInvocation()
  const baseliner = await api('semantics/v1/describe',{target:{kind:'operator',name:'explore.baseliner'},view:'full'})
  assert.equal(baseliner.contract.document.outputs.length,4,'all four complex outputs are declared')
  assert.equal(await ev('document.querySelector("[aria-label=输出说明]").value'),baseliner.contract.document.outputs.map(output=>output.port_id+'：'+output.description).join('\n\n'),'all complex output descriptions are directly editable and match the author contract')
  assert.equal(await ev('document.querySelectorAll(".author-output").length'),0,'output descriptions are not duplicated outside the editor')
  await writeFile(evidence+'/baseliner.png',Buffer.from((await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true})).data,'base64'))
  await send('Page.navigate', { url: base + '/semantics' }); await expectText('新建绑定')
  await click('新建绑定')
  assert.equal(await ev('[...document.querySelector("[aria-label=字典精确版本]").options].some(option=>option.value.startsWith("operator_"))'), false)
  await send('Page.navigate', { url: base + '/semantics?ref=operator_contract.browser.created%402' })
  await expectText('算子契约和使用依据为只读精确记录')
  assert.equal(await ev('[...document.querySelectorAll("button")].some(button=>button.textContent==="编辑当前版本")'), false)
  assert.equal(await ev('window.__calls.some(u=>["/preview","/batch/execute","/stream/execute"].some(path=>u.includes(path)))'),false,'contract UI never loads business rows or executes algorithms')
  assert.equal(await ev('window.__calls.some(u=>u.includes("/operators/check")||u.includes("/semantics/v1/channels/")||["/channels/dataframe","/channels/database","/channels/kafka"].some(path=>u.includes(path)))'),false,'contract UI never queries input sources or preflight')
  await writeFile(evidence+'/requests.json',JSON.stringify(await ev('window.__calls'),null,2))
  assert.deepEqual(browser.exceptions, [])
  console.log('PASS: actual Chromium creation/upload/contract reading/history and conflict/reload draft preservation without invocation UI')
} catch(error) { console.error(error);await writeFile(evidence+'/failure-dom.txt',await body());throw error } finally { await browser.close() }
