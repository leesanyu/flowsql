import React, { useEffect, useState, useMemo, useRef, useCallback } from 'react'
import { createRoot } from 'react-dom/client'
import { GraphicWalker, PureRenderer, normalize } from '@kanaries/graphic-walker'
import '@kanaries/graphic-walker/dist/style.css'
import { connectBridge, contract } from './bridge.js'
import { api, computationClient } from './compute.js'
import { snapshotClient, snapshotLabel } from './dataframe.js'
import { controls, editChart, marks, aggregates, chartKinds, chartProblem, roleFields, countField, histogramWorkflow, histogramRows, statisticsWorkflow } from './chart.js'
import './style.css'

function AdvancedEditor({ chart, fields, compute, context, store, onChange }) {
  const [initial] = useState(() => [chart])
  const [instance, setInstance] = useState(null)
  const bridge = useMemo(() => ({ get current() { return store.current },
    set current(value) { store.current = value; setInstance(value) } }), [store])
  const reportError = useCallback(() => { queueMicrotask(() => store.current?.updateShowErrorResolutionPanel(0)) }, [store])
  const latest = useRef(chart)
  latest.current = chart
  const changed = useRef(onChange)
  changed.current = onChange
  useEffect(() => {
    if (!instance) return
    instance.setShowAutoVizPanel(false)
    instance.applyChart(latest.current)
    const edit = (event) => {
      if (event.detail.instanceID === instance.instanceID)
        changed.current(JSON.parse(JSON.stringify(instance.exportCode()[0])))
    }
    document.addEventListener('edit-graphic-walker', edit)
    return () => { document.removeEventListener('edit-graphic-walker', edit); if (store.current === instance) store.current = null }
  }, [instance])
  return <GraphicWalker fields={fields.filter((f) => f.fid !== countField.fid)} chart={initial} computation={compute} storeRef={bridge}
    className="native-editor" style={{ height: '100%', width: '100%' }}
    onError={reportError}
    appearance={context.theme} i18nLang={context.language} hideChartNav hideSegmentNav hideProfiling
    experimentalFeatures={{ computedField: false }}
    toolbar={{ exclude: ['export_chart', 'csv', 'export_code', 'autoviz', 'mark_type', 'painter', 'coord_system', 'debug', 'kanaries'] }} />
}

function sourceFields(source) {
  const fields = source.fields.filter((f) => f.type !== 'unsupported').map((f) => ({ fid: f.field_id,
    name: f.label, semanticType: f.type === 'number' ? 'quantitative' : 'nominal',
    analyticType: f.type === 'number' ? 'measure' : 'dimension' }))
  fields.push(countField)
  return fields
}
function newWorkspace(source) {
  const fields = sourceFields(source)
  const measure = fields.find((f) => f.analyticType === 'measure')
  const chart = normalize({ mark: 'bar',
    x: 'fid:' + (fields.find((f) => f.analyticType === 'dimension') || measure).fid,
    y: { field: 'fid:' + measure.fid, aggregate: measure.aggName || 'mean' },
    layout: { size: { mode: 'full', width: 800, height: 460 } } }, fields.filter((f) => f.fid !== countField.fid))
  return { source, source_id: source.source_id, schema_revision: source.schema_revision, chart,
    kind: source.kind === 'dataframe' ? 'dataframe' : 'database', snapshot: null, stale: '',
    editor_mode: 'simple', detail_mode: 'complete', temporal_field: undefined, request_generation: 0 }
}
// A DataFrame page binds one snapshot; fields come from the acquired snapshot, not from discovery.
function newDataFrameWorkspace(source, snapshot) {
  const detailed = { ...source, kind: 'dataframe', fields: snapshot.fields || [],
    schema_revision: snapshot.schema_revision, time_bucket_fields: snapshot.time_bucket_fields || [] }
  return { ...newWorkspace(detailed), source: detailed, snapshot: { ...snapshot },
    schema_revision: snapshot.schema_revision }
}
function TableEditor({ page, context, update, acquire }) {
  const { source, chart, editor_mode: mode } = page
  const dataframe = page.kind === 'dataframe'
  const fields = useMemo(() => sourceFields(source), [source])
  const detail = page.detail_mode === 'limited'
  const setChart = (chart) => update({ chart })
  const setMode = (editor_mode) => update({ editor_mode })
  const setDetail = (checked) => update({ detail_mode: checked ? 'limited' : 'complete' })
  const [settings, setSettings] = useState(false)
  const [recommend, setRecommend] = useState(false)
  const [info, setInfo] = useState(false)
  const [precision, setPrecision] = useState(false)
  const [error, setError] = useState('')
  const [result, setResult] = useState(null)
  const [busy, setBusy] = useState(false)
  const [run, setRun] = useState(0)
  const store = useRef(null)
  const values = controls(chart)
  const kind = chartKinds[values.mark]
  const problem = chartProblem(chart, fields)
  const temporal = page.temporal_field === values.x
  const labels = page.snapshot ? snapshotLabel(page.snapshot) : null
  const client = useMemo(() => computationClient({ ...source, snapshot_id: page.snapshot?.snapshot_id,
      detail_mode: detail ? 'limited' : 'complete',
      on_expired: () => update({ stale: '快照已过期，请重新获取快照后绘图。' }) },
    (value) => { setResult(value); setError(''); setBusy(false) },
    (message) => { setError(message); setResult(null); setBusy(false) }), [source, detail, page.snapshot?.snapshot_id])
  useEffect(() => { client.setVisible(context.visible); return () => client.setVisible(false) }, [client, context.visible])
  const compute = useCallback(async (workflow) => {
    setBusy(true)
    try {
      let query
      try {
        if (problem) throw new Error(problem)
        query = statisticsWorkflow(chart, histogramWorkflow(chart, workflow))
      } catch (error) { setError(error.message); setResult(null); throw error }
      const rows = await client.compute(query)
      return histogramRows(chart, workflow, rows)
    }
    finally { setBusy(false) }
  }, [client, chart, problem])
  const change = (patch) => {
    client.cancel(); setError(''); setResult(null)
    const next = editChart(chart, patch, fields)
    setChart(next)
    store.current?.applyChart(next)
  }
  const onNativeChange = (value) => {
    if (value.config.flowsqlHistogram && value.encodings.columns[0]?.fid !== 'gw_histogram_bin') delete value.config.flowsqlHistogram
    if (value.config.flowsqlTimeBucket && (controls(value).x !== value.config.flowsqlTimeBucket.field ||
      !value.config.defaultAggregated || ['histogram', 'point', 'circle', 'boxplot'].includes(controls(value).mark))) delete value.config.flowsqlTimeBucket
    setChart(value)
  }
  const toggleTemporal = (checked) => {
    const next = structuredClone(chart)
    for (const channel of Object.values(next.encodings))
      for (const field of channel) if (field.fid === values.x) field.semanticType = checked ? 'temporal' :
        fields.find((f) => f.fid === values.x)?.semanticType
    update({ temporal_field: checked ? values.x : undefined })
    setChart(next)
    store.current?.applyChart(next)
  }
  return <section className="table-editor" data-mode={mode} data-kind={page.kind}>
    <div className="quick-controls">
      <label>图表 <select aria-label="图表" value={values.mark} onChange={(e) => change({ mark: e.target.value })}>{Object.entries(marks)
        .filter(([value]) => !(dataframe && value === 'boxplot'))
        .map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label>
      {dataframe && <span className="control-hint">DataFrame 快照不支持箱线图：分位数不在当前计算范围内</span>}
      {kind?.roles.map((role) => <label key={values.mark + role.key}>{role.label}
        <select aria-label={['bar', 'stacked_bar', 'grouped_bar', 'line', 'area', 'point', 'circle'].includes(values.mark) && ['x', 'y'].includes(role.key) ? role.key === 'x' ? '横轴' : '纵轴' : role.label}
          value={values[role.key]} onChange={(e) => change({ [role.key]: e.target.value })}>
          <option value="">{role.optional ? '不分组' : `选择${role.label}`}</option>
          {roleFields(fields, role, values.mark, values.agg).map((f) => <option key={f.fid} value={f.fid}>{f.name}</option>)}
        </select>
      </label>)}
      {values.mark === 'histogram' ? <>
        <label>箱数 <input aria-label="箱数" type="number" min="1" max="100" step="1" value={values.bins} onChange={(e) => change({ bins: Number(e.target.value) })} /></label>
        <span className="control-hint">频数自动计算</span>
      </> : !['point', 'circle', 'boxplot'].includes(values.mark) &&
        <label>聚合 <select aria-label="聚合" value={values.agg} onChange={(e) => change({ agg: e.target.value })}>{[...aggregates, 'none'].map((v) => <option key={v} value={v}>{v === 'none' ? '不聚合' : v === 'distinctCount' ? '去重计数' : v}</option>)}</select></label>}
      {source.time_bucket_fields?.includes(values.x) && values.agg !== 'none' && !['histogram', 'point', 'circle', 'boxplot'].includes(values.mark) &&
        <label>时间分桶 <select aria-label="时间分桶" value={values.timeBucket} onChange={(e) => change({ timeBucket: e.target.value })}>
          <option value="none">原始值</option><option value="day">按日</option><option value="month">按月</option>
        </select></label>}
      {values.timeBucket !== 'none' && <span className="control-hint">源库日历日期 · 不转换时区</span>}
      <button className="primary" disabled={!!problem} onClick={() => { client.cancel(); setError(''); setResult(null); setRun((v) => v + 1) }}>绘图</button>
      <button aria-expanded={settings} onClick={() => setSettings(!settings)}>更多设置</button>
      <button aria-pressed={mode === 'advanced'} onClick={() => { client.cancel(); setMode(mode === 'simple' ? 'advanced' : 'simple') }}>{mode === 'simple' ? '高级编辑' : '简洁绘图'}</button>
      <button aria-expanded={recommend} onClick={() => setRecommend(!recommend)}>图表推荐</button>
      <div className="source-info">
        <button aria-label="来源信息" aria-describedby="source-information" aria-expanded={info} onClick={() => setInfo(!info)}>ⓘ</button>
        <div id="source-information" role="tooltip" className={'popover' + (info ? ' shown' : '')}>
          <strong>{dataframe ? 'DataFrame 快照' : source.dialect + ' 原生计算'}</strong><p>日期时间保留源时区；文本排序遵循源列规则。支持 count/去重计数/sum/mean/min/max。</p>
          <p>直方图、日/月时间桶和去重均由源库计算。时间桶按源库日历，MySQL/PG 带时区时间遵循会话，ClickHouse 遵循列时区，SQLite 使用 ISO 文本日期部分；不自动转换时区。任意表达式尚不支持。</p>
          {source.fields.map((f) => <div key={f.field_id}>{f.label}：{f.native_type || '动态类型'}（{f.type}）</div>)}
        </div>
      </div>
    </div>
    {settings && <div className="settings-panel">
      <label>过滤字段 <select aria-label="过滤字段" value={values.filter} onChange={(e) => change({ filter: e.target.value })}><option value="">无</option>{fields.filter((f) => f.fid !== countField.fid).map((f) => <option key={f.fid} value={f.fid}>{f.name}</option>)}</select></label>
      <input aria-label="过滤值" value={values.filterValue} onChange={(e) => change({ filterValue: e.target.value })} placeholder="集合中的一个值" />
      <label><input type="checkbox" checked={detail} onChange={(e) => setDetail(e.target.checked)} />受限明细</label>
      {values.timeBucket === 'none' && !['histogram', 'pie', 'heatmap', 'highlight_table'].includes(values.mark) && <label><input aria-label="UTC 时间横轴" type="checkbox" checked={temporal} onChange={(e) => toggleTemporal(e.target.checked)} />UTC 时间横轴</label>}
    </div>}
    {recommend && <aside className="recommendations" aria-label="图表推荐">
      <span>根据当前字段调整同一张图表：</span>{Object.entries(marks).map(([mark, label]) => <button key={mark} onClick={() => change({ mark })}>{label}</button>)}

    </aside>}
    {problem && <p className="control-hint" role="status">{problem}</p>}
    <div className="result-status" aria-live="polite">
      {dataframe && <span className="snapshot-bar" aria-label="DataFrame 快照" data-snapshot-id={page.snapshot?.snapshot_id || ''}>
        <strong>DataFrame 快照</strong>
        <span>采集时间 {labels?.captured || '—'}</span>
        <span>行数 {labels?.rows ?? '—'}</span>
        <span>{page.stale ? page.stale : '到期前自动续期'}</span>
        <button onClick={async () => { const renewed = await acquire(page.source_id, page.snapshot?.snapshot_id); if (renewed) update(renewed) }}>获取最新快照</button>
      </span>}
      <output aria-label="结果范围">{error ? '查询失败' : busy ? '正在查询…' : result ? `${result.scope === 'complete' ? '全量计算结果' : result.scope === 'top_n' ? 'Top-N 结果' : '受限明细，非全量'} · ${result.returned_rows} 行 · ${result.elapsed_ms} ms` : '选择字段后绘图'}</output>
      {!!result?.numeric_display?.length && <button className="precision-badge" aria-expanded={precision} onClick={() => setPrecision(!precision)}>
        {result.numeric_display.some((d) => d.mode === 'approximate') ? '≈ 近似数值' : 'Decimal 精确数值'}</button>}
      <button onClick={() => { client.cancel(); setBusy(false) }}>取消查询</button>
    </div>
    {precision && result?.numeric_display?.length > 0 && <aside className="precision-info" aria-label="数值精度">
      <p>近似数值仅用于图形展示，过滤、分组与统计仍由源库执行。</p>
      {result.numeric_display.map((d) => <details key={d.field_id}><summary>{d.field_id} · {d.mode === 'approximate' ? '近似' : '精确'}</summary>
        {d.origin === 'native_decimal_mean' ? <p>源库 Decimal 均值原生返回 Float64，未提供精确均值文本。</p> :
          <ol>{d.exact_values.map((value, i) => <li key={i}>{value === null ? 'NULL' : value}</li>)}</ol>}
      </details>)}
    </aside>}
    {error && <div role="alert">{error}；请调整字段或设置后重新绘图。</div>}
    <div className="plot-area" aria-label="绘图区">
      {!problem && (mode === 'simple' ? <PureRenderer key={run} type="remote" computation={compute} appearance={context.theme} locale={context.language}
        visualState={chart.encodings} visualConfig={chart.config} visualLayout={chart.layout}
        overrideSize={{ mode: 'full', width: 800, height: 460 }} /> :
        <AdvancedEditor key={run} chart={chart} fields={fields} compute={compute} context={context} store={store} onChange={onNativeChange} />)}
    </div>
  </section>
}

function Explorer({ context }) {
  const [sources, setSources] = useState([])
  const [selected, setSelected] = useState('')
  const [pages, setPages] = useState({})
  const [checking, setChecking] = useState(false)
  const [error, setError] = useState('')
  const generation = useRef(0)
  const pageRef = useRef(pages)
  pageRef.current = pages
  const snapshots = useMemo(() => snapshotClient((snapshot_id) => {
    for (const page of Object.values(pageRef.current))
      if (page.snapshot?.snapshot_id === snapshot_id)
        setPages((old) => ({ ...old, [page.source_id]: { ...old[page.source_id], stale: '快照已过期，请重新获取快照后绘图。' } }))
  }), [])
  const refresh = async () => {
    const token = ++generation.current
    setChecking(true); setError('')
    try {
      const listing = await api('sources')
      if (token !== generation.current) return
      setSources(listing.sources)
      const validate = async (page) => {
        if (!listing.sources.some((s) => s.source_id === page.source_id))
          return [page.source_id, page.kind === 'dataframe'
            ? { invalid: '来源已删除；已取得的快照在有效期内仍可使用，重新获取快照会失败。', replacement: null }
            : { invalid: '来源已消失，请关闭工作页或恢复来源后刷新。', replacement: null }]
        if (page.kind === 'dataframe') return [page.source_id, { invalid: '', replacement: null }]
        try {
          const source = await api('sources/' + page.source_id)
          return [page.source_id, source.schema_revision === page.schema_revision ? { invalid: '', source, replacement: null } :
            { invalid: '字段结构已变化，请重新选择字段。', replacement: source }]
        } catch (e) { return [page.source_id, { invalid: e.message, replacement: null }] }
      }
      const validated = []
      for (const page of Object.values(pageRef.current)) validated.push(await validate(page))
      if (token !== generation.current) return
      setPages((old) => Object.fromEntries(Object.entries(old).map(([id, page]) => [id,
        { ...page, ...validated.find(([key]) => key === id)?.[1] }])))
    } catch (e) { if (token === generation.current) setError(e.message) }
    finally { if (token === generation.current) setChecking(false) }
  }
  useEffect(() => { refresh(); return () => { ++generation.current; snapshots.close() } }, [])
  // Renewal follows page activity: visible DataFrame pages extend their lease before it lapses.
  useEffect(() => {
    if (!context.visible) return undefined
    const timer = setInterval(() => {
      for (const page of Object.values(pageRef.current))
        if (page.snapshot?.snapshot_id && !page.stale) snapshots.renew(page.snapshot.snapshot_id).catch(() => {})
    }, Number(globalThis.__flowsqlRenewMilliseconds) || 5 * 60 * 1000)
    return () => clearInterval(timer)
  }, [context.visible, snapshots])
  useEffect(() => {
    if (!selected || pages[selected] || checking) return
    const controller = new AbortController()
    let live = true
    setError('')
    api('sources/' + selected, { signal: controller.signal }).then(async (source) => {
      if (source.kind !== 'dataframe') { if (live) setPages((old) => ({ ...old, [selected]: newWorkspace(source) })); return }
      try {
        const snapshot = await snapshots.acquire(selected)
        if (live) setPages((old) => ({ ...old, [selected]: newDataFrameWorkspace(source, snapshot) }))
        else await snapshots.release(snapshot.snapshot_id)
      } catch (e) { if (live) setError(e.code === 'DATA_PLANE_UNAVAILABLE' ? 'DataFrame 数据面不可用：请确认来源服务与共享目录配置。' : e.message) }
    }).catch((e) => { if (live && e.name !== 'AbortError') setError(e.message) })
    return () => { live = false; controller.abort() }
  }, [selected, checking, !!pages[selected]])
  const acquire = useCallback(async (source_id, previous) => {
    try {
      const snapshot = await snapshots.acquire(source_id)
      const current = pageRef.current[source_id]
      if (!current) { await snapshots.release(snapshot.snapshot_id); return null }
      if (current.snapshot?.schema_revision === snapshot.schema_revision) {
        setPages((old) => old[source_id] ? { ...old, [source_id]: { ...old[source_id], snapshot, source: { ...old[source_id].source,
          schema_revision: snapshot.schema_revision }, schema_revision: snapshot.schema_revision, stale: '' } } : old)
      } else {
        // A changed schema cannot reuse field bindings; keep the page and ask for a new field selection.
        setPages((old) => old[source_id] ? { ...old, [source_id]: { ...newDataFrameWorkspace(old[source_id].source, snapshot),
          editor_mode: old[source_id].editor_mode, detail_mode: old[source_id].detail_mode,
          stale: '字段结构已变化，请重新选择字段。' } } : old)
      }
      if (previous && previous !== snapshot.snapshot_id) await snapshots.release(previous)
      return { snapshot }
    } catch (e) {
      // The still-valid previous snapshot stays bound; the failure is reported without mixing versions.
      setError(e.message)
      return null
    }
  }, [snapshots])
  const page = pages[selected]
  const update = useCallback((patch) => setPages((old) => old[selected] ?
    { ...old, [selected]: { ...old[selected], ...patch, request_generation: old[selected].request_generation + 1 } } : old), [selected])
  const close = (id) => {
    const closing = pageRef.current[id]
    if (closing?.snapshot?.snapshot_id) snapshots.release(closing.snapshot.snapshot_id)
    setPages((old) => { const next = { ...old }; delete next[id]; return next })
    if (id === selected) setSelected(Object.keys(pages).find((key) => key !== id) || '')
  }
  return <>
    <div className="source-status">
      <label>数据来源 <select aria-label="数据来源" value={selected} onChange={(e) => setSelected(e.target.value)}><option value="">请选择数据表或 DataFrame</option>
        <optgroup label="数据库表">{sources.filter((s) => s.kind !== 'dataframe').map((s) => <option key={s.source_id} value={s.source_id}>{s.label}</option>)}</optgroup>
        <optgroup label="DataFrame 快照">{sources.filter((s) => s.kind === 'dataframe').map((s) => <option key={s.source_id} value={s.source_id}>{s.label}</option>)}</optgroup>
      </select></label>
      <button onClick={refresh} disabled={checking}>刷新来源</button><a className="brand" href="https://kanaries.net/" target="_blank" rel="noreferrer">Graphic Walker · Kanaries</a>
    </div>
    {Object.keys(pages).length > 0 && <nav className="workspace-tabs" aria-label="数据表工作页">
      {Object.values(pages).map((page) => <div key={page.source_id} className="workspace-tab">
        <button role="tab" aria-selected={page.source_id === selected} onClick={() => setSelected(page.source_id)} title={page.source.label}>{page.source.label}</button>
        <button aria-label={'关闭 ' + page.source.label} onClick={() => close(page.source_id)}>×</button>
      </div>)}
    </nav>}
    {error && <div role="alert">{error}</div>}
    {checking ? <div className="empty-state">正在核验来源与字段…</div> : page?.invalid ? <div className="empty-state"><div role="alert">{page.invalid}
      {page.replacement && <button onClick={() => setPages((old) => ({ ...old, [selected]: newWorkspace(page.replacement) }))}>重新选择字段</button>}
    </div></div> : page && context.visible ? <TableEditor key={selected} page={page} context={context} update={update} acquire={acquire} /> :
      <div className="empty-state">{!context.visible ? '工作区已隐藏，查询已暂停' : selected ? '正在读取字段…' : '选择数据表或 DataFrame 开始探索'}</div>}
  </>
}

function Workspace() {
  const [context, setContext] = useState({ theme: localStorage.getItem('theme') || 'light', language: 'zh-CN', visible: true })
  const [error, setError] = useState('')
  const [ready, setReady] = useState(false)
  const [bridge] = useState(() => connectBridge((value) => setContext((old) => ({ ...old, ...value }))))
  useEffect(() => {
    document.documentElement.dataset.theme = context.theme
    document.body.dataset.visible = String(context.visible)
  }, [context])
  useEffect(() => {
    let live = true
    Promise.all([fetch('/api/visualization/v1/capabilities').then((r) => { if (!r.ok) throw new Error('数据探索服务不可用'); return r.json() }),
      fetch('/exploration/manifest.json', { cache: 'no-store' }).then((r) => r.json())])
      .then(([api, manifest]) => {
        if (!live) return
        for (const key of ['api_version', 'workflow_version']) if (api[key] !== contract[key] || manifest[key] !== contract[key]) throw new Error('数据探索版本不兼容')
        if (manifest.bridge_version !== 1) throw new Error('桥接版本不兼容')
        setReady(true)
        bridge.send('ready', { ...contract, ui_version: manifest.ui_version })
      }).catch((e) => { if (live) { setError(e.message); bridge.send('error', { message: e.message }) } })
    return () => { live = false; bridge.close() }
  }, [])
  return <div className="workspace">
    {!bridge.embedded && <header><strong>FlowSQL · 数据探索</strong><button onClick={() => setContext((c) => ({ ...c, theme: c.theme === 'dark' ? 'light' : 'dark' }))}>切换主题</button></header>}
    {error ? <div role="alert">{error}<button onClick={() => location.reload()}>重试</button></div> : !ready ? <p>正在加载数据探索…</p> : <>
      <Explorer context={context} />
    </>}
  </div>
}
createRoot(document.getElementById('root')).render(<Workspace />)
