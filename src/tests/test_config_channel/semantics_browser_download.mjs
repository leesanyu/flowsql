import { mkdir, mkdtemp, readdir, readFile } from 'node:fs/promises'
import path from 'node:path'
import { parseJson } from '../../frontend/src/utils/dataSemantics.js'

export function downloads(browser, evidence) {
  return async trigger => {
    const root = path.resolve(evidence, 'downloads')
    await mkdir(root, { recursive: true })
    const directory = await mkdtemp(path.join(root, 'json-'))
    await browser.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: directory })
    await trigger()
    const filename = await browser.wait(async () => (await readdir(directory)).find(name => name.endsWith('.json')), 'completed JSON download')
    const content = await readFile(path.join(directory, filename), 'utf8')
    return { filename, content, value: parseJson(content) }
  }
}
