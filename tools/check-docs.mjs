// SPDX-License-Identifier: Apache-2.0
// Render built Markdown diagrams: an SSR build alone cannot validate Mermaid.
import { chromium } from 'playwright'
import { execFileSync } from 'node:child_process'
import { createServer } from 'node:http'
import { readFileSync, mkdirSync } from 'node:fs'
import { resolve, extname, sep } from 'node:path'

const dist = resolve('docs/.vitepress/dist')
const base = process.env.DOCS_BASE || '/mistlane/'
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.woff2': 'font/woff2', '.json': 'application/json' }
const server = createServer((req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname)
    if (!path.startsWith(base)) throw new Error('Outside documentation base')
    const file = resolve(dist, path.slice(base.length) + (path.endsWith('/') ? 'index.html' : ''))
    if (!file.startsWith(dist + sep)) throw new Error('Outside distribution')
    res.setHeader('Content-Type', types[extname(file)] || 'application/octet-stream')
    res.end(readFileSync(file))
  } catch { res.writeHead(404); res.end('Not found') }
})
await new Promise(r => server.listen(0, '127.0.0.1', r))
let browser
try {
  browser = await chromium.launch({
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  })
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } })
  const failures = []
  page.on('pageerror', e => failures.push(e.message))
  page.on('console', m => { if (m.type() === 'error') failures.push(m.text()) })
  const origin = `http://127.0.0.1:${server.address().port}${base}`
  const files = [...new Set(execFileSync('git', ['ls-files', '--cached', '--others', '--exclude-standard', 'docs'], { encoding: 'utf8' }).trim().split('\n'))]
  let diagrams = 0
  for (const file of files.filter(f => f.endsWith('.md'))) {
    const expected = [...readFileSync(file, 'utf8').matchAll(/^```mermaid\s*$/gm)].length
    if (!expected) continue
    await page.goto(origin + file.slice(5, -3) + '.html')
    await page.waitForFunction(n => document.querySelectorAll('.diagram-canvas svg').length === n || !!document.querySelector('.architecture-diagram [role=alert]'), expected)
    const actual = await page.locator('.diagram-canvas svg').count()
    if (actual !== expected) throw new Error(`${file}: expected ${expected} diagrams, rendered ${actual}; ${await page.locator('[role=alert]').allTextContents()}`)
    const clipped = await page.evaluate(() => [...document.querySelectorAll('.diagram-canvas .node')].flatMap(node => {
      const shape = node.querySelector(':scope > rect')
      const label = node.querySelector('.label')
      if (!shape || !label) return []
      const a = shape.getBoundingClientRect(), b = label.getBoundingClientRect()
      return b.top < a.top - 2 || b.bottom > a.bottom + 2 || b.left < a.left - 2 || b.right > a.right + 2 ? [label.textContent] : []
    }))
    if (clipped.length) throw new Error(`${file}: clipped labels: ${clipped.join(', ')}`)
    diagrams += actual
  }
  for (const width of [1440, 390]) for (const dark of [false, true]) {
    await page.setViewportSize({ width, height: 1000 })
    await page.emulateMedia({ colorScheme: dark ? 'dark' : 'light' })
    await page.goto(origin + 'zh/')
    await page.waitForSelector('.diagram-canvas svg')
    await page.evaluate(d => document.documentElement.classList.toggle('dark', d), dark)
    await page.waitForFunction(d => getComputedStyle(document.documentElement).getPropertyValue('--vp-c-brand-1').trim() === (d ? '#9bd1bb' : '#246553'), dark)
    await page.waitForTimeout(350)
    if (process.env.DOCS_CAPTURE_DIR) {
      mkdirSync(process.env.DOCS_CAPTURE_DIR, { recursive: true })
      await page.screenshot({ path: resolve(process.env.DOCS_CAPTURE_DIR, `${width === 1440 ? 'desktop' : 'mobile'}${dark ? '-dark' : ''}.png`), fullPage: true })
    }
    if (await page.evaluate(() => document.documentElement.scrollWidth > innerWidth)) throw new Error(`Page overflow at ${width}px`)
    await page.locator('.diagram-toolbar button').click()
    if (await page.locator('figure.expanded').count() !== 1) throw new Error('Expand control failed')
    if (width === 390) {
      const expanded = await page.locator('.diagram-canvas svg').evaluate(svg => svg.getBoundingClientRect().width)
      await page.locator('.diagram-toolbar button').click()
      const fitted = await page.locator('.diagram-canvas svg').evaluate(svg => svg.getBoundingClientRect().width)
      if (expanded <= fitted + 10) throw new Error('Mobile expand/fit did not change diagram width')
      const capacity = await page.locator('.diagram-canvas').evaluate(el => { const s = getComputedStyle(el); return el.clientWidth - parseFloat(s.paddingLeft) - parseFloat(s.paddingRight) })
      if (fitted > capacity + 1) throw new Error('Mobile Fit still overflows its canvas')
    }
  }
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.goto(origin + 'architecture.html')
  await page.locator(`a[href="${base}http.html"]`).first().click()
  await page.waitForURL('**/http.html')
  await page.waitForFunction(() => document.querySelector('h1')?.textContent?.includes('HTTP') && document.querySelectorAll('.diagram-canvas svg').length === 2)
  if (failures.length) throw new Error(failures.join('\n'))
  console.log(`${diagrams} Mermaid diagrams rendered; desktop/mobile, light/dark, expansion and client navigation passed`)
} finally {
  await browser?.close()
  await new Promise(r => server.close(r))
}
