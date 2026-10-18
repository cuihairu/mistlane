<script setup lang="ts">
// SPDX-License-Identifier: Apache-2.0
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { useData } from 'vitepress'
import { renderDiagram } from './mermaid'
const props = defineProps<{ source: string }>()
const { isDark, lang } = useData()
const chinese = computed(() => lang.value.startsWith('zh'))
const svg = ref('')
const error = ref('')
const expanded = ref(false)
let generation = 0
let mounted = false
async function render() {
  if (!mounted) return
  const current = ++generation
  error.value = ''
  try {
    const result = await renderDiagram(props.source, isDark.value)
    if (current === generation) svg.value = result.svg
  } catch (cause) {
    if (current === generation) error.value = String(cause)
    console.error('Mermaid render failed', cause)
  }
}
onMounted(() => { mounted = true; render() })
onBeforeUnmount(() => { mounted = false; ++generation })
watch([isDark, () => props.source], render)
</script>

<template>
  <figure class="architecture-diagram" :class="{ expanded }">
    <figcaption class="diagram-toolbar">
      <span>{{ chinese ? '架构图' : 'Architecture diagram' }}</span>
      <button type="button" :aria-pressed="expanded" @click="expanded = !expanded">
        {{ expanded ? (chinese ? '适合页面' : 'Fit to page') : (chinese ? '展开查看' : 'Expand diagram') }}
      </button>
    </figcaption>
    <div v-if="svg && !error" class="diagram-canvas" tabindex="0"
      :aria-label="chinese ? '架构图，可横向滚动查看' : 'Architecture diagram, scroll horizontally to explore'" v-html="svg" />
    <p v-else-if="!error" role="status">{{ chinese ? '正在绘制图表…' : 'Rendering diagram…' }}</p>
    <p v-if="error" role="alert">{{ chinese ? '图表未能绘制，请查看下方源码。' : 'Diagram could not render. Read the source below.' }} {{ error }}</p>
    <details>
      <summary>{{ chinese ? '查看 Mermaid 源码' : 'View Mermaid source' }}</summary>
      <pre><code>{{ source }}</code></pre>
    </details>
  </figure>
</template>

<style scoped>
.architecture-diagram { margin: 28px 0; border: 1px solid var(--vp-c-divider); border-radius: 12px; background: var(--vp-c-bg); overflow: hidden; }
.diagram-toolbar { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 10px 16px; background: var(--vp-c-bg-soft); font-size: 13px; color: var(--vp-c-text-2); }
button { border: 1px solid var(--vp-c-divider); border-radius: 6px; padding: 3px 10px; color: var(--vp-c-brand-1); font-weight: 600; }
button:hover { background: var(--vp-c-bg); }
.diagram-canvas { overflow-x: auto; padding: 24px 16px; text-align: center; }
.diagram-canvas :deep(svg) { display: block; height: auto; max-width: 100%; margin: auto; }
.expanded .diagram-canvas :deep(svg) { max-width: none; }
details { border-top: 1px solid var(--vp-c-divider); padding: 10px 16px; }
summary { cursor: pointer; color: var(--vp-c-text-2); font-size: 13px; }
pre { overflow-x: auto; font-size: 12px; padding: 16px 0; }
p { padding: 0 16px; }
[role='alert'] { color: var(--vp-c-danger-1); }
</style>
