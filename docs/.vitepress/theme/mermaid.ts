// SPDX-License-Identifier: Apache-2.0
let sequence = 0
let queue: Promise<unknown> = Promise.resolve()
export function renderDiagram(source: string, dark: boolean) {
  const task = queue.then(async () => {
    const { default: mermaid } = await import('mermaid')
    await document.fonts.ready
    mermaid.initialize({
      startOnLoad: false, securityLevel: 'strict', theme: 'base',
      look: 'classic', layout: 'dagre', htmlLabels: false,
      fontFamily: 'system-ui, sans-serif',
      themeVariables: {
        darkMode: dark, fontSize: '15px',
        primaryColor: dark ? '#243e3d' : '#e8f1ee',
        primaryTextColor: dark ? '#eef3ef' : '#203630',
        primaryBorderColor: dark ? '#7aaba0' : '#54776b',
        lineColor: dark ? '#a4b8b0' : '#51665e',
        secondaryColor: dark ? '#303b41' : '#edf0f2',
        tertiaryColor: dark ? '#262d2b' : '#f5f4ee',
        clusterBkg: dark ? '#202927' : '#f6f7f3',
        clusterBorder: dark ? '#6c837b' : '#a1b5ac',
      },
      flowchart: { useMaxWidth: false, htmlLabels: false, nodeSpacing: 28, rankSpacing: 38 },
      sequence: { useMaxWidth: false, wrap: true },
    })
    return mermaid.render(`mistlane-diagram-${++sequence}`, source)
  })
  queue = task.catch(() => undefined)
  return task
}
