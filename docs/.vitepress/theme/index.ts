// SPDX-License-Identifier: Apache-2.0
import DefaultTheme from 'vitepress/theme'
import MermaidDiagram from './MermaidDiagram.vue'
export default {
  extends: DefaultTheme,
  enhanceApp({ app }) { app.component('MermaidDiagram', MermaidDiagram) }
}
