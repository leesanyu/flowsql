import { defineConfig } from 'vite'
import { createHash } from 'node:crypto'
import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { contract } from './src/bridge.js'

const root = fileURLToPath(new URL('../../', import.meta.url))
let build
export default defineConfig({
  base: '/exploration/',
  plugins: [{
    name: 'exploration-manifest',
    transform(code, id) {
      const remote = '"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"'
      if (!id.includes('/@kanaries/graphic-walker/') || !code.includes(remote)) return
      // The bundled widget includes an unconditional CDN stylesheet. Package it locally.
      const base = new URL('./node_modules/leaflet/dist/', import.meta.url)
      const css = readFileSync(new URL('leaflet.css', base), 'utf8').replace(/url\(["']?(images\/[^)"']+)["']?\)/g,
        (_, file) => `url(data:image/png;base64,${readFileSync(new URL(file, base)).toString('base64')})`)
      const asset = this.emitFile({ type: 'asset', name: 'leaflet.css', source: css })
      return code.replace(remote, `import.meta.ROLLUP_FILE_URL_${asset}`)
        // Preserve the package's own Kanaries logo, using its bundled fallback image.
        .replace(/src: "https:\/\/imagedelivery\.net\/[^\"]+", fallbackSrc: ([\w$]+)/g,
          (_, logo) => `src: ${logo}, fallbackSrc: ${logo}`)
    },
    buildStart() {
      build = process.env.FLOWSQL_BUILD_INFO_FILE
        ? JSON.parse(readFileSync(process.env.FLOWSQL_BUILD_INFO_FILE, 'utf8'))
        : JSON.parse(execFileSync('python3', [root + 'tools/product_version.py', '--root', root], { encoding: 'utf8' }))
    },
    augmentChunkHash() { return JSON.stringify(build) },
    generateBundle(options, bundle) {
      for (const name of ['LICENSE', 'LICENSE2']) {
        this.emitFile({ type: 'asset', fileName: `licenses/graphic-walker-${name}`,
          source: readFileSync(new URL(`./node_modules/@kanaries/graphic-walker/${name}`, import.meta.url)) })
      }
      this.emitFile({ type: 'asset', fileName: 'licenses/NOTICE.txt', source:
        'FlowSQL incorporates Graphic Walker 0.5.3 by Kanaries. Kanaries branding is retained.\n' +
        'Build adaptation: the external Leaflet stylesheet and original fallback logo are served locally.\n' })
      for (const output of Object.values(bundle)) {
        if (output.type === 'chunk') output.code = `/*FLOWSQL_BUILD_INFO_BEGIN${JSON.stringify(build)}FLOWSQL_BUILD_INFO_END*/\n` + output.code
      }
    },
    writeBundle(options, bundle) {
      const files = {}
      for (const name of Object.keys(bundle)) {
        files[name] = createHash('sha256').update(readFileSync(path.join(options.dir, name))).digest('hex')
      }
      const uiVersion = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8')).version
      writeFileSync(path.join(options.dir, 'manifest.json'), JSON.stringify({ ...contract, ui_version: uiVersion, build, files }) + '\n')
    }
  }],
  build: { chunkSizeWarningLimit: 5000 }
})
