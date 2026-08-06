# ADR 0037 — The ES module loader's host half

**Status:** accepted · **Date:** 2026-08-06 · **Implemented:** 2026-08-05 (session 50)

## Context

ADR 0011 named one unanswered design question: `Interpreter::SetModuleResolver` is synchronous, so
how does `<script type="module">` reach the network without blocking the one loop this browser
has?

Gate B (`www.reddit.com` past the challenge) depends on the answer. Four roadmap sessions (13–16)
each expected reddit's bundle to run; all four stopped on `ReferenceError: PerformanceObserver is
not defined` until session 50. After `PerformanceObserver` landed, the module loader's host half
landed in the same session, and the page moved from **143 display-list commands / 3 script runs** to
**210 commands / 48 runs** — sidebar chrome and search UI appeared. Gate B is still not met: the
feed does not fill in.

The survey (2026-08-04) named what the bundles actually use:

| Shape | reddit.com | youtube.com |
|---|---|---|
| `<script type="module">` | 3× `data:text/javascript,…` entry scripts | kevlar graph via static `import` |
| `import()` | `apply-polyfill-….js` (Navigation API polyfill) | chunk loads behind route changes |
| Bare specifiers | No import map — CDN URLs are absolute or `./` relative | Same |

reddit's entry is a **`data:` module** that static-imports telemetry and static-imports three more
scripts from `www.redditstatic.com`. youtube's kevlar bundle is a **closed static graph** fetched
as one 10.7MB script today, with dynamic `import()` for lazy chunks once routing runs.

`src/engine/ModuleLoader.h` and `PageModules.cpp` are the implementation. This ADR records the
decision they encode so ADR 0011's addendum can point here instead of leaving the question open.

## Decision

### 1. Two mechanisms, because static and dynamic import are different questions

**A static module graph is closed before anything in it runs.**

1. Each module's source arrives through `PageScript::AddFetched` (or `AddDataUrl` for `data:`).
2. `ModuleLoader::MissingFrom` parses the module for `import` specifiers and returns URLs not yet
   in the table.
3. The engine fetches those URLs (privacy verdict, connection pool — same as any subresource).
4. Repeat until `MissingFrom` is empty.
5. Only then does evaluation start. `SetModuleResolver` is a **table lookup**; a miss is the
   `TypeError` the language specifies, not a stall.

This is why the resolver stays synchronous: by the time bytecode asks, the network half has
already finished. Blocking inside the resolver would violate ADR 0011.

**A dynamic `import()` is answered later.**

1. `SetDynamicImportStarter` receives the specifier and a fresh promise.
2. If the source is not in the table, the URL joins `module_fetches_` and `Want()` starts a fetch.
3. The promise stays pending across turns; `AdvanceModules` settles it when the source arrives.
4. The promise is rooted in a JavaScript array on the global until settled — a raw `Object*`
   would survive collection incorrectly.

`tests/ModuleLoaderTests.cpp` asserts both halves: a static import whose dependency has not
arrived is not evaluated, and a dynamic import's promise is pending across a loop turn.

### 2. URL resolution rules that target pages actually hit

| Case | Rule |
|---|---|
| Bare specifier (`"react"`) | **Refuse** — no import map. Resolve only full URLs and paths starting with `/`, `./`, or `../`. A bare name resolving to `https://page.example/react` was a request the page never named. |
| `data:` module | Source is the URL itself; imports resolve against the **document URL**, not the `data:` referrer (`new URL("./x", "data:…")` is useless). reddit's entry depends on this. |
| Document URL | Set at parse time (`SetModuleDocumentUrl`), **not** cleared on `InstallModuleHost` — sources arrive before the interpreter exists. |
| Navigation | `ModuleLoader::Clear` with the document — one graph per document, same rule as a fresh global. |

### 3. What reddit and youtube need — and what is still missing for Gate B

**Landed and verified (session 50):**

- Static `import` / `export` / `import.meta` in the VM (`src/js/Modules.cpp`).
- `data:` module sources without a fetch.
- Pre-pass graph closure for module scripts.
- Dynamic `import()` with pending settlement (`js.dynamic_imports` / `js.dynamic_imports_settled`
  counters).

**Measured on www.reddit.com after session 50:**

```
performance.observer_callbacks   3
performance.entries              7
js.compile_bailouts              0
js.dynamic_imports               0   ← feed paths not reached yet
```

The feed lives in two `<template for="s_…">` elements (729 and 1668 nodes) that reddit's own
`<suspense-replace>` custom element must hoist (session 14). That is **TD-0016**, not a module-loader
gap. `js.dynamic_imports` is zero because the code paths that call `import()` sit behind
`window.Sentry?.…` and **`requestIdleCallback`**, which is absent — so the Navigation API polyfill
never runs.

**youtube.com** still loads kevlar as a classic script in practice for the main bundle; the module
loader matters for chunks once the app boots. The white-page blocker is Polymer rendering and
post-bundle layout (TD-0013), not graph resolution.

### 4. What this ADR does not decide

- **Import maps** — absent. No page in the ADR 0007 set requires one; bare specifiers stay errors.
- **Worker modules** — ADR 0022 refuses workers for now.
- **Cyclic static graphs** — handled by evaluation order in the VM; host only guarantees sources
  exist before `EvaluateModuleGraph` runs.
- **`import.meta.url` on `data:`** — answered as the `data:` URL itself; pages must not assume a
  filesystem path.

## Consequences

- **Positive:** ADR 0011's "unblocks the module loader" consequence is satisfied. `fetch()` and
  module fetches share one queue and one partition key.
- **Positive:** reddit's sidebar chrome renders without a second request path or an async resolver
  inside the VM.
- **Negative:** Gate B needs **`requestIdleCallback`** (or the feed hoisted by other means) and
  **`<suspense-replace>`** — see TD-0016. The loader alone does not fill the feed.
- **Negative:** A page that static-imports a graph larger than memory allows still fails at fetch
  or compile bounds; the loader does not stream individual modules during evaluation.

## Alternatives considered

**Async `SetModuleResolver` that fetches inside the callback.** Rejected. It blocks the loop on
every cache miss and duplicates `RequestQueue`. The pre-pass closure keeps network waits where
ADR 0011 already puts them — between turns.

**Fetch the whole graph in one `<script type="module">` request only.** Rejected. Dynamic
`import()` is not optional on reddit and youtube; a static-only loader would be wrong for both.

**Import maps to make bare specifiers work.** Rejected for now. No target page needs one; enabling
them would let a page name modules the host did not fetch in the pre-pass, recreating the async
resolver problem under another spelling.

**Pre-pass only; no dynamic `import()`.** Rejected. reddit's `data:` entry eventually
`import()`s the Navigation API polyfill; settling synchronously would block or lie.
