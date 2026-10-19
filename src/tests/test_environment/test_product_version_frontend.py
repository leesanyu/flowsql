"""Real isolated Vite build/dev and Vue render checks; local Node/npm, no external services."""
import json, os, shutil, socket, subprocess, tempfile, time, urllib.request
from pathlib import Path
root=Path(__file__).resolve().parents[3]
subprocess.run(['node','--test',str(root/'src/frontend/test/productVersion.test.mjs')],check=True,timeout=30)
with tempfile.TemporaryDirectory(prefix='flowsql-vite-') as temp:
    directory=Path(temp)
    frontend=directory/'src/frontend'
    frontend.mkdir(parents=True)
    for name in ('src','public'):
        shutil.copytree(root/'src/frontend'/name,frontend/name)
    for name in ('vite.config.js','index.html','package.json','package-lock.json'):
        shutil.copyfile(root/'src/frontend'/name,frontend/name)
    config=frontend/'vite.config.js'
    config.write_text(config.read_text().replace('export default defineConfig({', 'export default defineConfig({\n  optimizeDeps: { noDiscovery: true, include: [] },'))
    (frontend/'node_modules').symlink_to(root/'src/frontend/node_modules',target_is_directory=True)
    (directory/'tools').mkdir()
    shutil.copyfile(root/'tools/product_version.py',directory/'tools/product_version.py')
    (directory/'VERSION').write_text('0.1.0\n')
    subprocess.run(['npm','run','build','--','--outDir',str(directory/'dist'),'--emptyOutDir'],cwd=frontend,check=True,timeout=120)
    expected={'version':'0.1.0','revision':None,'dirty':None}
    assert json.loads((directory/'dist/build-info.json').read_text())==expected
    import sys
    sys.path.insert(0,str(root/'tools'))
    from release import embedded_info
    chunks=list((directory/'dist/assets').glob('*.js'))
    assert chunks and all(embedded_info(path)==expected for path in chunks)
    render=frontend/'render.mjs'
    render.write_text('''import { createServer } from 'vite'
import { createSSRApp } from 'vue'
import { renderToString } from '@vue/server-renderer'
import { routeLocationKey } from 'vue-router'
import assert from 'node:assert/strict'
globalThis.localStorage = { getItem() { return null } }
const server = await createServer({ optimizeDeps: { noDiscovery: true, include: [] }, server: { middlewareMode: true, preTransformRequests: false } })
try {
 for (const file of ['/src/App.vue','/src/components/Sidebar.vue']) {
  const { default: Component } = await server.ssrLoadModule(file)
  const app=createSSRApp(Component)
  app.provide(routeLocationKey, { path: '/dashboard' })
  app.config.warnHandler=()=>{}
  app.component('router-view',{template:'<div />'})
  app.component('router-link',{template:'<a><slot /></a>'})
  const html=await renderToString(app)
  assert.match(html,/v0\\.1\\.0/)
  assert.match(html,/提交：未知；源码状态：未知/)
  assert.doesNotMatch(html,/v0\\.0\\.0/)
 }
} finally { await server.close() }
''')
    subprocess.run(['node',str(render)],cwd=frontend,check=True,timeout=30)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    with (directory/'dev.log').open('w') as log:
        process=subprocess.Popen(['npm','run','dev','--','--host','127.0.0.1','--port',str(port),'--strictPort'],cwd=frontend,stdout=log,stderr=log,start_new_session=True)
        try:
            endpoint=f'http://127.0.0.1:{port}/@id/__x00__virtual:flowsql-build-info'
            for _ in range(100):
                try:
                    text=urllib.request.urlopen(endpoint,timeout=1).read().decode()
                    if '0.1.0' in text:break
                except OSError:pass
                time.sleep(.1)
            else:raise AssertionError((directory/'dev.log').read_text())
            (directory/'VERSION').write_text('0.2.0-rc.1\n')
            for _ in range(100):
                text=urllib.request.urlopen(endpoint,timeout=1).read().decode()
                if '0.2.0-rc.1' in text:break
                time.sleep(.1)
            else:raise AssertionError('Vite dev did not refresh version source')
        finally:
            import signal
            os.killpg(process.pid,signal.SIGTERM)
            process.wait(timeout=10)
    print('PASS: independent build snapshot, App/Sidebar rendered version and tooltip, Vite dev VERSION refresh')
