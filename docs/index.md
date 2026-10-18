<script setup lang="ts">
import { withBase } from 'vitepress'
// 首次访问按浏览器语言选择文档语言（vitepress 无此能力，GitHub Pages 不支持 _redirects）
// 优先级：localStorage 手动选择 > navigator.language > 保持当前 locale
import { onBeforeMount } from 'vue'
const KEY = 'mistlane-lang-auto-picked'
const BASE = import.meta.env.BASE_URL
const ROOT_PATH = BASE
const ZH_PATH = BASE + 'zh/'
onBeforeMount(() => {{
  if (typeof window === 'undefined') return
  const p = window.location.pathname
  if (p !== ROOT_PATH && p !== ROOT_PATH.slice(0, -1)) return
  const picked = localStorage.getItem(KEY)
  if (picked) {{ if (picked !== p) window.location.replace(picked); return }}
  const langs = (navigator.languages?.length ? navigator.languages : [navigator.language]).map(l => l.toLowerCase())
  if (langs.some(l => l.startsWith('zh'))) window.location.replace(ZH_PATH)
}})
if (typeof window !== 'undefined') {{
  window.addEventListener('click', (e) => {{
    const a = (e.target as HTMLElement)?.closest?.('a') as HTMLAnchorElement | null
    const href = a?.getAttribute('href') || ''
    if (href.startsWith(ZH_PATH) || href.startsWith('zh/')) localStorage.setItem(KEY, ZH_PATH)
    else if (href === ROOT_PATH || href === ROOT_PATH.slice(0, -1)) localStorage.setItem(KEY, ROOT_PATH)
  }}, true)
}}
</script>

<p align="center">
  <img :src="withBase('/logo.svg')" alt="Mistlane logo" width="64" height="64" style="margin-inline: auto;">
</p>

# Mistlane

Mistlane is a C++20 / Lua framework extracted and reorganized from a **legacy game backend**. It is published for **research and learning**: studying service separation, asynchronous networking, Lua integration, and the modernization of an existing codebase.

It is an educational framework candidate, not a recommendation for production deployment. The extraction removes the original game's gameplay, content tables, account integration, payment code, channel SDKs, production configuration, and repository history.

The new implementation keeps the gateway / router / worker topology while introducing new interfaces and a new wire protocol. It is not a drop-in replacement for the original servers.

## What you can study

| Area | Implementation |
|---|---|
| Asynchronous I/O | Boost.Asio TCP, timers, signals, coroutines |
| WS and WSS | Drogon, with OpenSSL TLS |
| Database integration | Boost.MySQL's existing connection_pool and parameter binding |
| Application scripting | Single-threaded Lua module lifecycle and transactional VM replacement |
| Reproducible dependencies | CMake Presets and a pinned vcpkg manifest baseline |

```text
TCP / WS / WSS client
        |
     gateway
        |
      router
     /      \
 worker    worker
  + Lua     + Lua

Application extension --> Boost.MySQL connection_pool --> database
```

The database adapter is a separate C++ integration, not yet a Lua database bridge. Current routing is per connection; player migration, shared connection multiplexing, and distributed recovery are outside this version.

Start with the [guide](./guide.md), read the [architecture and limitations](./architecture.md), and check the [validation record](./validation.md).

## Origin and licensing

The framework was derived from FOGS. The original meaning of that project name is unconfirmed. Mistlane is licensed under Apache License 2.0. Source provenance and third-party licenses are documented in [NOTICE](https://github.com/cuihairu/mistlane/blob/main/NOTICE) and [third-party information](https://github.com/cuihairu/mistlane/blob/main/THIRD_PARTY.md).
