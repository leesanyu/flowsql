import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { openBrowser } from '../test_visualization/browser_support.mjs'
const [base, evidence, uploadPath] = process.argv.slice(2)
const browser = await openBrowser(base)
const { evaluate: ev, wait, send } = browser
const body = () => ev('document.body.innerText')
const expectText = text => wait(async () => (await body()).includes(text), text)
const click = async (text, scope = '') => {
  await wait(() => ev(`[...document.querySelectorAll(${JSON.stringify(scope + ' button')})].some(e=>e.textContent.trim()===${JSON.stringify(text)}&&!e.disabled)`), text)
  return ev(`(()=>{const e=[...document.querySelectorAll(${JSON.stringify(scope + ' button')})].find(e=>e.textContent.trim()===${JSON.stringify(text)});e.click()})()`)
}
const set = async (label, value) => ev(`(()=>{const e=document.querySelector(${JSON.stringify('[aria-label="' + label + '"]')});if(!e)throw Error('missing input '+${JSON.stringify(label)});e.value=${JSON.stringify(value)};e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}))})()`)
const api = async (path, payload, status = 200) => {
  const r = await fetch(base + '/api/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })
  const result = await r.json(); assert.equal(r.status, status, JSON.stringify(result)); return result.data || result
}
const contract = {
  schema_id: 'flowsql.operator-data-contract.v1', title: 'Browser declared contract',
  operator: { category: 'browser', name: 'created', version: '1' },
  configuration: { invocation_schema: { type: 'object', additionalProperties: true }, affects: {} },
  inputs: [{ port_id: 'input', min_count: 1, max_count: 1, shape: 'dataframe', roles: [{
    role_id: 'measure', required: true, types: [{ system: 'arrow', name: 'double', parameters: {} }], nullable: null,
    semantic_requirements: {}, selector: { kind: 'column', column: 'latency' }
  }] }],
  outputs: [{ port_id: 'output', shape: 'dataframe', mode: 'derived', rules: [{ kind: 'copy', source_port: 'input', all_fields: true }] }],
  limitations: ['No row-quality validation']
}
const source = 'from flowsql import OperatorBase, register_operator\n@register_operator(category="browser", name="created")\nclass Created(OperatorBase):\n    def work(self, df):\n        return df\n'
const openRow = async (name, action) => {
  await wait(() => ev(`[...document.querySelectorAll('tr')].some(row=>row.textContent.includes(${JSON.stringify(name)})&&[...row.querySelectorAll('button')].some(button=>button.textContent.trim()===${JSON.stringify(action)}&&!button.disabled))`), name + ' ' + action)
  await ev(`(()=>{const row=[...document.querySelectorAll('tr')].find(row=>row.textContent.includes(${JSON.stringify(name)}));const button=[...row.querySelectorAll('button')].find(button=>button.textContent.trim()===${JSON.stringify(action)});if(!button)throw Error('row action missing');button.click()})()`)
}
try {
  await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 1100, deviceScaleFactor: 1, mobile: false })
  await send('Page.navigate', { url: base + '/operators' }); await expectText('新建算子')
  await click('新建算子'); await set('新算子名称', 'browser.created'); await set('新算子源码', source)
  await set('静态契约 JSON', JSON.stringify(contract)); await click('保存并激活')
  await expectText('算子已激活'); await openRow('created', '查看'); await expectText('输入输出契约')
  await expectText('operator_contract.browser.created@1'); await expectText('measure')
  await set('选择实际数据集', 'input'); await set('已有算法参数', '{}'); await click('检查实际对象')
  await wait(() => ev('document.querySelector("[data-testid=contract-check-result]")?.textContent.includes("compatible")'), 'actual preflight')
  assert.match(await ev('document.querySelector("[data-testid=contract-check-result]").textContent'), /latency/)
  await set('已有算法参数', '{"threshold":"0.05"}'); await click('刷新声明与来源列表')
  assert.equal(await ev('document.querySelector("[aria-label=已有算法参数]").value'), '{"threshold":"0.05"}')
  await click('关闭', '.el-dialog__footer'); await openRow('created', '去激活'); await openRow('created', '编辑')
  await expectText('编辑依据')
  const original = await api('operators/detail', { name: 'browser.created' })
  const draft = original.content + '\n# keep local draft\n'
  await set('编辑算子源码', draft)
  const newer = { ...contract, title: 'Updated browser contract', operator: { ...contract.operator, version: '2' } }
  const external = 'from flowsql import OperatorBase, register_operator\n@register_operator(category="browser", name="created", data_contract=__import__("json").loads(' + JSON.stringify(JSON.stringify(newer)) + '))\nclass Created(OperatorBase):\n    def work(self, df):\n        return df\n'
  await api('operators/update', { name: 'browser.created', content: external, expected_component_revision: original.component_revision })
  await click('保存', '.el-dialog__footer'); await expectText('COMPONENT_CHANGED')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'), draft)
  await send('Page.reload'); await expectText('编辑依据')
  assert.equal(await ev('document.querySelector("[aria-label=编辑算子源码]").value'), draft)
  assert.equal(await ev('document.querySelector("[aria-label=已有算法参数]").value'), '{"threshold":"0.05"}')
  await expectText('operator_contract.browser.created@2')
  await set('契约精确版本', 'operator_contract.browser.created@1'); await expectText('正在查看历史声明')
  await click('关闭', '.el-dialog__footer')
  const dom = await send('DOM.getDocument')
  const file = await send('DOM.querySelector', { nodeId: dom.root.nodeId, selector: 'input[type=file]' })
  await send('DOM.setFileInputFiles', { nodeId: file.nodeId, files: [uploadPath] })
  await expectText('上传成功'); await openRow('uploaded', '编辑'); await expectText('operator_contract.browser.uploaded@1')
  await click('关闭', '.el-dialog__footer')
  await ev("[...document.querySelectorAll('.operator-type-item')].find(e=>e.textContent.includes('C++')).click()")
  await openRow('output.so', '查看'); await click('contractdemo.output 契约'); await expectText('secondary')
  await set('输入来源定位符', '[{"kind":"dataframe","name":"input"}]')
  await set('已有算法参数', '{"operation":"dual"}'); await click('检查实际对象')
  await wait(() => ev('document.querySelector("[data-testid=contract-check-result]")?.textContent.includes("compatible")'), 'C++ preflight')
  await writeFile(evidence + '/contract.png' , Buffer.from((await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true })).data, 'base64'))
  await send('Page.navigate', { url: base + '/semantics' }); await expectText('新建绑定')
  await click('新建绑定')
  assert.equal(await ev('[...document.querySelector("[aria-label=字典精确版本]").options].some(option=>option.value.startsWith("operator_"))'), false)
  await send('Page.navigate', { url: base + '/semantics?ref=operator_contract.browser.created%402' })
  await expectText('算子契约和使用依据为只读精确记录')
  assert.equal(await ev('[...document.querySelectorAll("button")].some(button=>button.textContent==="编辑当前版本")'), false)
  assert.deepEqual(browser.exceptions, [])
  console.log('PASS: actual Chromium creation/upload/detail/preflight/history and conflict/reload draft preservation')
} finally { await browser.close() }
