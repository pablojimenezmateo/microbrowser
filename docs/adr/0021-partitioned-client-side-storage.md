# ADR 0021 — Client-side storage, partitioned, and what is allowed to survive

**Status:** accepted · **Date:** 2026-08-04

## Context

There is no client-side storage of any kind. ADR 0012 listed the whole category under "what is
refused, and why it is refused rather than deferred", with one sentence:

> **Storage APIs without a partition key.** `localStorage`, `sessionStorage`, IndexedDB and the
> Cache API are all per-site state, so every one of them is keyed by ADR 0005's key or does not
> exist. This is the row people forget, so it is written here as well as there.

That settles *how* they must be keyed and leaves *whether they exist* open. The survey answers that:

| | occurrences |
|---|---|
| `sessionStorage` | 51 |
| `localStorage` | 44 |
| `caches` / `CacheStorage` | 14 |
| `indexedDB` | 7 |

And Plex's very first inline script, before any bundle loads, is:

```js
const SESSION_STORAGE_KEY = "splashScreenViewed";
try { … window.sessionStorage.getItem(SESSION_STORAGE_KEY) … } catch {}
```

That one is defensive and degrades. The sign-in token is not, and this is where the ADR gets its
tension. `AGENTS.md` says:

> Nothing persists to disk unless the user opted in. The HTTP cache is memory-only by default.

Plex requires an account. A browser that discards `localStorage` on exit is a browser that makes the
user sign in to Plex every launch. Those two statements are both true, and pretending otherwise —
either by quietly writing the token to disk or by declaring the sign-in loop acceptable — is the
failure mode this ADR exists to prevent.

## Decision

### 1. Storage exists, and the key is not optional anywhere

`localStorage`, `sessionStorage`, IndexedDB and the Cache API are implemented, in that order.

**Every store takes an ADR 0005 `PartitionKey` — `(container, top-level site, origin)` — and there
is no overload without one.** The architecture lint already enforces this for `CookieJar` and
`HttpCache` and explicitly says it applies "on each per-site store as it lands". Each of these is
one of those, and each extends the lint on the commit it lands rather than afterwards.

The consequence is the one ADR 0005 was written to produce and it is worth stating in terms a page
would notice: `example.com` embedded in `a.com` and `example.com` embedded in `b.com` have
**different** `localStorage`. That is Total Cookie Protection applied to storage, it breaks
third-party single-sign-on flows that assume shared state, and it is the correct behaviour under
this project's priority order. Where a page breaks because of it, the breakage is a decision.

### 2. Persistence is opt-in, per site, and the default is memory

Three tiers, and the middle one is the point:

| Store | Lifetime by default |
|---|---|
| `sessionStorage` | the tab, always. Never on disk, in any configuration. |
| `localStorage`, IndexedDB, Cache API | **memory, for the life of the browser session** |
| any of the above, for a site the user has marked persistent | on disk, encrypted at rest |

So the default is that storage *works* — a page can write and read within a session, which is what
the overwhelming majority of the 116 sites in the table need — and nothing outlives the process. No
prompt, no dialog, no decision asked of the user for the common case.

Persistence is a **user act on a site**, expressed the way this browser expresses site-level
decisions already (HTTPS downgrade is per-site and explicit; this is the same shelf). It is not
`navigator.storage.persist()`: a page asking to be remembered is not the user choosing to remember
it, and that API's honest answer here is to resolve `false` unless the user has already marked the
site.

**This is the answer to the Plex problem, and it is not a compromise of the rule.** "Nothing persists
unless the user opted in" is satisfied by a user who marked Plex persistent; it would be violated by
a browser that decided sign-in tokens are important enough to keep. The difference is who chose.

### 3. Quota is per key, enforced, and small enough to be honest

Each partition key gets a byte budget across all its stores. Exceeding it throws
`QuotaExceededError` — which is the specified failure and one every real page handles, because
Safari's quotas have trained the web to handle it.

The number is chosen the way ADR 0009's parse bound was: measured against the target sites, with the
margin written down. A quota is also a security bound — storage is memory, and unbounded storage
from a page is a denial of service against the process — so it is enforced at the store rather than
checked by a caller.

Eviction, when it happens, is **whole partitions, least recently used**, never individual keys. Half
a site's state is worse than none: a page that finds its schema version but not its data behaves in
ways nobody tested.

### 4. `localStorage` is synchronous, and that is a real cost to accept

The API is synchronous by specification, so a `getItem` blocks script. With a memory-backed store
that is a hash lookup and costs nothing. With a disk-backed store for a persistent site it is I/O on
the main loop — the one place this project has been careful never to block.

The resolution: **a persistent site's store is loaded into memory when the document is created and
written back asynchronously**, so the synchronous API is always served from memory and the disk
never appears in a `getItem`. The write-back is a task, and it rides the existing loop rather than a
timer.

That has a bounded consequence — a crash can lose the last unflushed writes — and it is the right
trade for a store the specification limited to a few megabytes. IndexedDB, which is asynchronous by
design and can be large, does not need the trick and does not get it.

### 5. Ordering, and why `sessionStorage` is first

1. **`sessionStorage`** — 51 occurrences, the simplest lifetime, no persistence question at all, and
   it is what Plex touches before anything else runs.
2. **`localStorage`** — the same store with a longer lifetime and the persistence tier above.
3. **Cache API** — 14 occurrences, and every one of them is inside a service worker path that
   ADR 0022 refuses. It lands *late* and possibly never; it is listed so that "we skipped it" is a
   decision.
4. **IndexedDB** — 7 occurrences, and by far the largest implementation on this list: object stores,
   indices, cursors, transactions with their own commit semantics, and the structured clone
   algorithm underneath. It is a poor return on the count, and it is on the list because "any page"
   eventually means a page that uses it.

**The `storage` event is part of `localStorage`, not an extra.** It fires in other documents of the
same origin *and partition* when a value changes. With one tab it fires nowhere and is trivially
correct; the reason to build it early is that it is the mechanism a page uses to notice a sign-out
in another tab, and retrofitting it after tabs exist means auditing every write.

### 6. What is refused

- **`navigator.storage.persist()` granting persistence.** It resolves `false` for a site the user
  has not marked. That is an honest answer, not a stub: the browser genuinely will not persist, and
  the page is told so in the way the API is defined to tell it.
- **`document.cookie` without the partition key.** It reads and writes the same partitioned jar as
  the network stack; there is no second path.
- **Any store that is not keyed.** Including whatever comes next — this is the row that gets
  forgotten, and the lint is what makes forgetting fail the build.

## Consequences

- **The architecture lint grows a store per landing**, and the rule "every storage-like lookup takes
  a `PartitionKey`" stops being a two-class rule and becomes a real invariant.
- **`src/net`'s partition key gets used by modules that are not `net`.** Where the stores live is a
  `MODULE.deps` question — most likely a new `src/storage` that `engine` and `bindings` reach through
  the same shape ADR 0015 uses for geometry, so that `bindings` never holds a store directly.
- **Encryption at rest becomes a requirement the first time anything is written.** A sign-in token in
  a plaintext file on disk is the worst outcome available here, and it is the default outcome if
  persistence ships before the key management does. They land together or persistence does not land.
- **Some sites break, and the breakage is the feature.** Partitioned storage breaks federated login
  and cross-site personalisation. That is ADR 0005 working, and the user-visible consequence should
  be documented somewhere a user reads, not only here.
- **A browser session becomes a thing with a lifetime**, which it was not before. What clears on
  navigation, on tab close, and on exit is now three different answers, and getting them wrong leaks
  state between sites.

## Alternatives considered

**Keep refusing storage entirely, as ADR 0012 does today.** Rejected on the measurement: 116 call
sites, and Plex cannot sign in. It also fails ADR 0012's own rule in an unusual direction — absence
is honest, but a page that feature-detects `localStorage` and finds it missing usually does not have
a fallback, because no browser has been without it since 2009.

**Persist everything by default, like every other browser.** Rejected on the privacy contract. It is
the single largest source of durable cross-session tracking state, and "the web expects it" is
exactly the kind of compatibility argument `AGENTS.md` ranks last.

**Prompt the user the first time a site writes.** Rejected as prompt fatigue, which is a privacy
failure with a consent-shaped costume. Every page writes storage; a prompt on every site trains the
user to accept, and a trained accept is worth nothing.

**Give `localStorage` a per-site "remember me" checkbox on the page's request.** Rejected as the same
thing at a smaller scale, and worse for being page-initiated. The persistence decision belongs on the
same shelf as the other per-site decisions the user makes about a site, reached the same way.

**Implement IndexedDB first, since it is what serious applications use.** Rejected on cost against
count: 7 occurrences and the largest implementation on the list. `sessionStorage` is 51 occurrences
and an afternoon.
