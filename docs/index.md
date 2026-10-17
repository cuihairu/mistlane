# Mistlane

Mistlane is a C++20 / Lua framework extracted and reorganized from a **legacy game backend**. It is published for **research and learning**: studying service separation, asynchronous networking, Lua integration, and the modernization of an existing codebase.

It is an educational framework candidate, not a recommendation for production deployment. The extraction removes the original game's gameplay, content tables, account integration, payment code, channel SDKs, production configuration, and repository history.

The new implementation keeps the gateway / router / worker topology while introducing new interfaces and a new wire protocol. It is not a drop-in replacement for the original servers.

## What you can study

| Area | Implementation |
|---|---|
| Asynchronous I/O | Boost.Asio TCP, timers, signals, coroutines |
| WS and WSS | Boost.Beast, with OpenSSL TLS |
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

The framework was derived from FOGS. The original meaning of that project name is unconfirmed. New code follows the independent repository's MIT license; two derived utility files retain Apache-2.0 markings and notices. See [LICENSE](https://github.com/cuihairu/mistlane/blob/main/LICENSE), [NOTICE](https://github.com/cuihairu/mistlane/blob/main/NOTICE), and [third-party information](https://github.com/cuihairu/mistlane/blob/main/THIRD_PARTY.md).
