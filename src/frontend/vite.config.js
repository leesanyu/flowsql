import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { execFileSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../../', import.meta.url))
const virtualId = 'virtual:flowsql-build-info'
const resolvedId = '\0' + virtualId
const readInfo = () => process.env.FLOWSQL_BUILD_INFO_FILE
  ? JSON.parse(readFileSync(process.env.FLOWSQL_BUILD_INFO_FILE, 'utf8'))
  : JSON.parse(execFileSync('python3', [root + 'tools/product_version.py', '--root', root], { encoding: 'utf8' }))

function productVersion() {
  let info
  return {
    name: 'flowsql-product-version',
    buildStart() { info = readInfo() },
    resolveId(id) { if (id === virtualId) return resolvedId },
    load(id) { if (id === resolvedId) return `export default ${JSON.stringify(readInfo())}` },
    augmentChunkHash() { return JSON.stringify(info) },
    configureServer(server) { server.watcher.add(root + 'VERSION') },
    handleHotUpdate({ file, server }) {
      if (file === root + 'VERSION') {
        server.moduleGraph.invalidateAll()
        server.ws.send({ type: 'full-reload' })
        return []
      }
    },
    generateBundle(options, bundle) {
      // Attach after minification so each delivered chunk retains its own identity.
      for (const output of Object.values(bundle)) {
        if (output.type === 'chunk') {
          output.code = `/*FLOWSQL_BUILD_INFO_BEGIN${JSON.stringify(info)}FLOWSQL_BUILD_INFO_END*/\n${output.code}`
        }
      }
      this.emitFile({ type: 'asset', fileName: 'build-info.json', source: JSON.stringify(info) + '\n' })
    }
  }
}

// https://vite.dev/config/
export default defineConfig({
  plugins: [vue(), productVersion()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://localhost:8081',
        changeOrigin: true
      }
    }
  }
})
