<script setup lang="ts">
import { onMounted, ref, watch } from 'vue'
import { useData } from 'vitepress'
const props = defineProps<{ source: string }>()
const { isDark, lang } = useData()
const svg = ref('')
const error = ref('')
let generation = 0
async function render() {
  const current = ++generation
  try {
    const { default: mermaid } = await import('mermaid')
    mermaid.initialize({
      startOnLoad: false, securityLevel: 'strict',
      theme: isDark.value ? 'dark' : 'default',
      fontFamily: 'system-ui, sans-serif',
    })
    const result = await mermaid.render(`diagram-${crypto.randomUUID()}`, props.source)
    if (current === generation) { svg.value = result.svg; error.value = '' }
  } catch (cause) {
    if (current === generation) error.value = String(cause)
    console.error('Mermaid render failed', cause)
  }
}
onMounted(render)
watch([isDark, () => props.source], render)
</script>

<template>
  <figure class="architecture-diagram">
    <div v-if="svg" class="diagram-canvas" v-html="svg" />
    <pre v-else><code>{{ source }}</code></pre>
    <p v-if="error" role="alert">{{ error }}</p>
    <details>
      <summary>{{ lang === 'zh-CN' ? '查看 Mermaid 源码' : 'View Mermaid source' }}</summary>
      <pre><code>{{ source }}</code></pre>
    </details>
  </figure>
</template>

<style scoped>
.architecture-diagram { margin: 24px 0; }
.diagram-canvas { overflow-x: auto; padding: 16px 0; text-align: center; }
.diagram-canvas :deep(svg) { height: auto; margin: auto; }
summary { cursor: pointer; color: var(--vp-c-text-2); font-size: 13px; }
pre { overflow-x: auto; font-size: 12px; padding: 16px; }
[role='alert'] { color: var(--vp-c-danger-1); }
</style>
