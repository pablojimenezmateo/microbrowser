# ADR 0006 — Content Blocking Engine

**Status:** accepted · **Date:** 2026-08-01

## Context

`guidelines/privacy.md` commits to content blocking as an engine-level invariant rather than an
extension, in one bullet, with no design behind it. This ADR is the design.

uBlock Origin is the reference, and the thing to copy is not the filter syntax — it is the
*architecture*. uBO is fast because it does almost no work per request: filters are compiled once
into token-indexed buckets, and a URL lookup probes a handful of buckets rather than testing tens of
thousands of patterns. A naive engine that tests patterns in a loop is somewhere between 100x and
1000x slower, and it is slow on the request path, which is the path a user waits on.

The scale is the constraint. EasyList + EasyPrivacy + uBO's own lists are roughly 300,000 network
rules and 100,000 cosmetic rules. Every one of them has to be in memory, in a browser whose stated
goal is a low footprint, and every one of them has to be considered on every subresource request.

There is also a privacy problem hiding inside a privacy feature: filter lists rot, updating them
means a network request, and this project's first rule is that there are no network requests the
user did not cause. That tension has to be resolved explicitly, because the failure mode — an
"obviously fine" background update timer — is exactly the one `guidelines/privacy.md` warns about
by name.

## Decision

Blocking lives in `src/privacy`, sits in the network process (ADR 0004), and is the first thing a
request meets.

### Filter syntax: an ABP/uBO subset, chosen by what it costs

**In, at M2** — network filters:

- Pattern forms: `||host^` (hostname-anchored), `|https://` (start-anchored), plain substring,
  `*` wildcards, `/regex/` as a last resort.
- Exceptions: `@@`.
- Options: `$third-party` / `$1p`, resource types (`script`, `image`, `stylesheet`, `xhr`, `font`,
  `media`, `subdocument`, `websocket`, `ping`, `document`), `$domain=a.com|~b.com`, `$important`,
  `$removeparam=`, `$redirect=`, `$csp=`.

**In, at M4** (needs the style engine) — cosmetic filters: `example.com##.ad`, `#@#` exceptions,
generic rules, and `##+js(...)` scriptlet injection.

**Deliberately out**: procedural cosmetic filters (`:has-text()`, `:xpath()`, `:matches-path()`) and
HTML filtering (`##^script:has-text()`). They need either a DOM-mutation observer or a streaming
HTML rewriter, they are where uBO spends most of its per-page CPU, and they cover a small tail of
sites. Revisit with a measurement, not with a broken site report.

An unrecognized rule is **skipped and counted**, never guessed at. A partially-understood filter is
worse than an absent one: it fails open on the request it was written to block, silently.

### The matcher: compile once, probe a few buckets

```
compile:  list text ──► parsed rules ──► flat arena + indices ──► cached on disk
match:    (url, source site, resource type) ──► tokens ──► bucket probes ──► verdict
```

The compiled form is a **flat arena of fixed-size rule records plus integer indices**. No
per-rule allocation, no pointers, no `std::string` per pattern — 300,000 rules as 300,000 heap
objects is the difference between a few megabytes and a few hundred. `CompiledRule` gets a
`static_assert(sizeof(...) <= N)` size budget for the reason ADR 0002 gives: its size is multiplied
by the list.

Three indices, in the order they are consulted:

1. **Hostname trie**, for `||host^` rules — the large majority of real lists. A reverse-label walk
   over the request's hostname, which also gets the "and all its subdomains" semantics for free
   rather than as a loop over suffixes.
2. **Token bucket index**, for substring patterns. This is uBO's central trick. At compile time each
   pattern contributes its most *selective* literal token; at match time the URL is tokenized and
   only the buckets for tokens the URL actually contains are probed. A URL with eight tokens tests
   the rules that could possibly match instead of all of them.
3. **Regex bucket**, tested last and only if nothing above decided. Kept small on purpose: a list
   that grows regex rules grows the one part of the engine with no index.

Exceptions (`@@`) are a parallel set of the same three structures, consulted after a block match, so
the common case — no match at all — never touches them.

**The match path allocates nothing.** Tokenization writes into a stack buffer, the verdict is a
value, and the URL is borrowed as a `string_view`. This is asserted by a test once the allocation
counter that `MICROBROWSER_PERF_HARNESS_BUILD` is supposed to arm actually exists; until then it is
asserted by review, which is a real gap and is recorded as one.

Compilation is expensive and its result is deterministic, so the compiled arena is written to
`AppDirectories::Cache()` — which is exactly what that directory is for, "anything reconstructible"
— keyed by a hash of the source lists. A cache miss costs a recompile, never a wrong answer.

### Scriptlets and redirect resources are code, and a list may never supply code

`##+js(set-constant, foo, true)` names a scriptlet; `$redirect=noop.js` names a resource. In both
cases **the name is looked up in a table compiled into the binary, and the list supplies only the
name and its arguments.**

This is the single most important security decision in this ADR. A filter list is third-party text,
fetched over the network, maintained by people we do not know. If a list could supply JavaScript, a
compromised list maintainer — or anyone who can MITM a list URL — would have script execution in the
page context of every site the user visits, which is a better position than most browser exploits
achieve. The indirection through a fixed table costs a hash lookup and removes the entire class.

Arguments are still list-controlled and are still untrusted: they are passed as data to a scriptlet
that treats them as data, never concatenated into source.

### List updating is a user action, or it does not happen

Lists ship compiled into the binary, so a fresh install blocks correctly with no network at all.

Updating is **opt-in and user-visible**. Either the user presses a button, or the user turns on
scheduled updates and is told what that means. There is no default-on timer, and there is no update
check that runs because the browser started.

When updates are on:

- The wakeup goes through `IdleWaitState::next_deadline_ms` like every other scheduled thing. It
  does not get a thread and it does not get a poll loop.
- The fetch goes through the same privacy layer as any other request. It carries no identifier, no
  query parameters, and no cookies, and it is partitioned like everything else.
- The schedule is jittered. A browser that phones a list server at a predictable interval has minted
  a timing fingerprint out of a privacy feature.
- A failed update is a no-op with a counter, not a retry storm. The old list keeps working.

### `$removeparam` and the URL sanitizer are one mechanism

The sanitizer's `utm_*`/`fbclid` list is just a `$removeparam` rule set that happens to ship with
us. Implementing them separately would mean two places that rewrite URLs, two orders of application,
and two answers to "what does the omnibox show". One engine, one pass.

## Consequences

- **A blocked request must be indistinguishable from a network failure the page could have caused
  on its own**, and must be *fast*. A blocking decision that takes measurably longer than a cache
  hit is a detectable signal, and detectable blocking is what anti-adblock scripts key on.
- **Every request pays the matcher**, so it is instrumented from the first commit: a scope per
  request (`privacy::MatchNetworkFilter` — per-request granularity, which `guidelines/observability.md`
  explicitly sanctions) and counters for lookups, blocks, exceptions, buckets probed, and rules
  skipped as unrecognized. The last one is the one that catches a list-format change silently
  degrading blocking.
- **The filter list parser is a hostile-input parser** and gets a libFuzzer target on the commit it
  lands, per `guidelines/testing.md`. So does the compiled-cache reader, which is the more dangerous
  of the two: it parses a binary format, and a corrupted or attacker-written cache file must fail to
  load rather than be trusted.
- **Blocking is per partition key**, so a container can carry a different list configuration.
  Falls out of ADR 0005 rather than needing its own mechanism.
- **Memory gets a stated budget** — the compiled lists are the largest single data structure in the
  browser before there is a page loaded, and "low footprint" is a project goal, so the number goes in
  `docs/performance/` and is tracked.
- **Cosmetic filtering is the expensive half** and lands later for that reason. Hostname-specific
  rules are cheap (a hash lookup at style time); generic rules require surveying the document's
  classes and ids, which is per-element work on the critical path.

## Alternatives Considered

**Vendor an existing engine** — Brave's `adblock-rust`, or uBO's own compiled matcher. This is
genuinely tempting and it fails ADR 0001's criteria on two counts: it is not a small readable
dependency, and writing it *does* teach us about browsers — the request path is where a privacy
browser either is or is not fast. It would also mean a Rust toolchain in the build for one component.

**Declarative-net-request style rule limits**, as Chrome's MV3 does. Rejected without much
deliberation: the rule cap is the reason MV3 blocking is worse than uBO, and adopting a limitation
invented to make blocking cheaper for browser vendors would be adopting it for no reason.

**Blocking in the renderer**, which is where an extension would do it. Rejected on the argument in
`guidelines/security.md`: policy runs where the attacker is not. A compromised renderer that can
disable its own blocking is a compromised renderer that can fetch anything.

**Hosts-file / DNS-level blocking only.** Much simpler and much weaker: no per-resource-type rules,
no exceptions, no first-party carve-outs, and it breaks sites that serve content and ads from one
hostname. Worth supporting as an additional source of rules; not worth having as the mechanism.

## Open

**How aggressive the default configuration is.** Which lists are on out of the box, and whether
"block third-party frames by default" is a default or an option. This is a compatibility question
that needs real browsing to answer, and answering it now would be guessing.

**Whether cosmetic filtering needs a DOM-mutation path** to handle sites that inject ads after load.
It does, in practice, and it is also where the per-page CPU goes. Sequence it after the style engine
exists and measure before choosing a strategy.
