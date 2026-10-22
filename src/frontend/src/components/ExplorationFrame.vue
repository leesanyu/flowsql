<template>
  <section class="exploration-host">
    <div v-if="error" class="exploration-error" role="alert">{{ error }} <el-button @click="load">重试</el-button></div>
    <p v-else-if="!ready" class="exploration-loading">正在连接数据探索服务…</p>
    <iframe v-if="src" v-show="!error" ref="frame" :src="src" title="数据探索" class="exploration-frame" />
  </section>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch } from 'vue'
const props = defineProps({ visible: Boolean, theme: String })
const frame = ref(null)
const src = ref('')
const error = ref('')
const ready = ref(false)
let timer
let generation = 0
const send = (type) => frame.value?.contentWindow?.postMessage({ version: 1, type,
  payload: { theme: props.theme, language: 'zh-CN', visible: props.visible } }, location.origin)
const receive = (event) => {
  if (event.origin !== location.origin || event.source !== frame.value?.contentWindow) return
  const { version, type, payload } = event.data || {}
  if (!['ready', 'error'].includes(type)) return
  if (version !== 1) { error.value = '数据探索版本不兼容'; clearTimeout(timer); return }
  if (type === 'error') { error.value = payload?.message || '数据探索加载失败'; clearTimeout(timer); return }
  if (type !== 'ready') return
  clearTimeout(timer)
  if (typeof payload?.ui_version !== 'string' || payload?.bridge_version !== 1 || payload?.api_version !== 1 || payload?.workflow_version !== 'graphic-walker-0.5.3') {
    error.value = '数据探索版本不兼容'; return
  }
  ready.value = true
  send('context')
}
const load = async () => {
  const current = ++generation
  error.value = ''; ready.value = false; src.value = ''; clearTimeout(timer)
  try {
    const response = await fetch('/api/visualization/v1/capabilities', { signal: AbortSignal.timeout(5000), cache: 'no-store' })
    if (!response.ok) throw new Error((await response.text()) || '数据探索服务暂不可用')
    const api = await response.json()
    if (api.api_version !== 1 || api.workflow_version !== 'graphic-walker-0.5.3') throw new Error('数据探索版本不兼容')
    const packageResponse = await fetch('/exploration/manifest.json', { signal: AbortSignal.timeout(5000), cache: 'no-store' })
    if (!packageResponse.ok) throw new Error((await packageResponse.text()) || '数据探索静态包不可用')
    const manifest = await packageResponse.json()
    if (manifest.bridge_version !== 1 || manifest.api_version !== 1 || manifest.workflow_version !== 'graphic-walker-0.5.3') throw new Error('数据探索版本不兼容')
    if (generation !== current) return
    src.value = '/exploration/?embed=1'
    timer = setTimeout(() => { error.value = '数据探索握手超时，请重试' }, 12000)
  } catch (e) { if (generation === current) error.value = e.message }
}
watch(() => props.visible, () => send('visibility'))
watch(() => props.theme, () => send('context'))
onMounted(() => { window.addEventListener('message', receive); load() })
onUnmounted(() => { ++generation; clearTimeout(timer); window.removeEventListener('message', receive) })
</script>

<style scoped>
.exploration-host { height: 100%; position: relative; min-height: 0; }
.exploration-frame { width: 100%; height: 100%; border: 0; display: block; }
.exploration-error, .exploration-loading { padding: 24px; }
.exploration-error { color: var(--text-primary); }
</style>
