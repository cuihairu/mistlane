# Mistlane documentation

<!-- impeccable:product-schema 1 -->

## Platform

web

## Product Purpose

Explain the extracted C++20 / Lua framework, its current implementation, and migration boundaries. Source: README.md and docs/architecture.md. The site uses VitePress with separate English and Chinese routes.

## Capabilities and Constraints

The user requires readable architecture diagrams, working Mermaid, and a redesigned documentation theme. Game ingress and the HTTP service must both support configurable WS/WSS listeners as a future runtime requirement. Current --transport selects one protocol; documentation must identify this limitation. Approved target responsibilities are gateway, login, coordinator, gameServer, crossServer, data and optional apiServer. Existing CLI names remain implementation facts until their planned migration. Distinguish implemented behavior from planned work.

## Evidence on Hand

Runtime source, transport tests, existing guides, diagrams, logo, and Apache-2.0 licensing. No new production capacity or payment implementation claims are authorized.

## Open Decisions

Theme preference was requested asynchronously. The current engineering-manual treatment is a provisional implementation choice, not a confirmed brand preference.
