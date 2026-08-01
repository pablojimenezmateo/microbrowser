# ADR 0005 — Contextual Identities and the Partition Key

**Status:** accepted · **Date:** 2026-08-01

## Context

Three separate things in this project all reduce to "which bucket does this state go in":

- **Total Cookie Protection**, already committed to: everything is partitioned by
  `(top-level site, origin)` so a third party embedded on two sites cannot correlate the visits.
- **Contextual identities** — Firefox's Multi-Account Containers. A *user-chosen* partition: a
  "Work" tab and a "Personal" tab are logged into the same site as different people, on purpose.
- **Site isolation**, ADR 0004: which WebContent process a document is assigned to.

They look like three features. They are one key, used three ways, and the expensive mistake is
discovering that after `net::Fetch`, the cookie jar, the cache, and the connection pool have each
grown their own idea of what identifies a bucket.

Firefox learned this the hard way and converged on `OriginAttributes` — a struct carrying
`userContextId`, `firstPartyDomain`, `privateBrowsingId`, and `partitionKey`, threaded through every
storage lookup in the browser. It works, and it took years to retrofit because the storage APIs were
written first.

There is also a layering problem that has to be solved at the same time. `guidelines/privacy.md`
says every request passes `src/privacy` before `src/net` sees it, and ADR 0004 says `net::Fetch`
takes the requesting identity. So `privacy` is above `net` in the call graph — but the identity type
is built out of URLs, origins, and registrable domains, which is `net`'s material. Put the key in
`net` and the modules depend on each other in both directions.

## Decision

### One key, in a module below both

A new bottom-of-stack module, **`src/url`**, owns `Url`, `Origin`, `Site`, the Public Suffix List,
and `PartitionKey`. `net`, `privacy`, `engine`, and `ui` all depend on it; it depends on `util` only.

This is Chromium's `//url` component, adopted for exactly the reason Chromium has it: URL and origin
types are needed by everything and must not drag the network stack in behind them. It is what breaks
the `privacy` ↔ `net` cycle, and the cycle is the proof that the module is real rather than a
convenience bucket.

### The key

```
PartitionKey = (ContainerId, Site top_level_site, Origin origin)
```

- **`ContainerId`** — the contextual identity. `ContainerId::Default()` is the ordinary browsing
  identity, and it is an ordinary value rather than an absent one, so there is no "no container"
  code path that skips the partitioning.
- **`top_level_site`** — the site of the top-level document, which is what makes third-party state
  uncorrelatable across sites. Already decided; unchanged.
- **`origin`** — the origin the state belongs to.

Every piece of per-site state is keyed by the whole thing. Not some of it, and not "the parts that
seemed to matter":

| State | Why it must carry the key |
|---|---|
| Cookies | the obvious one |
| `localStorage`, `sessionStorage`, IndexedDB, Cache API | same data, different API |
| HTTP cache entries | cache timing is a read oracle across partitions |
| Connection pool entries | a reused connection is a persistent identifier |
| **TLS session tickets and 0-RTT state** | a resumed session is a *server-assigned* identifier, which is worse than a cookie because nothing in the platform treats it as one |
| DNS cache entries | resolution timing is an oracle |
| HSTS and HPKP state | a per-site upgrade bit is a writable, readable, long-lived cookie |
| Permission grants | a grant is per identity, not per human |
| `Alt-Svc` / HTTP/3 advertisements | server-assigned routing state, same argument as session tickets |
| Favicon and preload caches | small, cached, and observable |

The two rows most often missed are TLS session tickets and HSTS. Both are state a *server* controls,
both survive a cookie clear, and both have been used for tracking in the wild.

### Containers refine the site instance, they do not sit beside it

This amends the definition in ADR 0004. A WebContent process serves one **`(site, ContainerId)`**
pair, not one site. Two containers on the same site get two processes.

Without this, containers are a storage-layer fiction: a memory-safety bug in the renderer would read
the other container's data straight out of the address space, and the entire point of the feature is
that those two identities are not supposed to meet. The process cap from ADR 0004 consolidates
same-site instances only, and it now means same-site *and* same-container. **Consolidation never
crosses a container boundary, for the same reason it never crosses a site boundary.**

The cost is real: containers multiply processes. That is the honest price of the feature, it is paid
only by users who create containers, and it is why the cap and the background-eviction policy exist.

### Ephemeral containers are the same mechanism

A container whose id is minted per tab and whose storage is memory-only, discarded when the tab
closes. Firefox users install an add-on for this; here it costs one constructor because the key
already exists. It is the right default for "open link in a throwaway context", and it is a
better-understood primitive than a private window, because it composes — you can have several at
once and they do not see each other.

`PrivateBrowsingId` is therefore **not** a separate field the way it is in Firefox. A private window
is an ephemeral container with persistence off, which removes a whole dimension from the key and a
whole class of "did we check both flags" bug.

### Persistence follows the existing rule

- Container **definitions** — name, colour, icon, and the site-assignment rules — are user
  configuration, in `AppDirectories::Config()`.
- Container **state** — cookies, storage — is in `AppDirectories::Data()` and only if the user opted
  into persistence at all. The default remains memory-only.
- An ephemeral container never writes either.

### A page cannot see which container it is in

Containers are a partitioning mechanism, not a signal. There is no API that exposes the container,
no header, no difference in `navigator`, and the container's name is never sent anywhere. If a page
could read it, it would be a fingerprinting bit that the *user* generated, which is the worst kind.

The residual risk is side channels — a page that can measure global resource exhaustion can infer
that other contexts exist. That is not container-specific and is not solved here; it is noted so
nobody later claims the isolation is stronger than it is.

## Consequences

- **`src/url` is a new module and lands in M2**, before `net` and `privacy`, because both need it.
  It carries the PSL, and the PSL is compiled in and never fetched — a list downloaded at runtime is
  both a request the user did not cause and a remote input to a security decision.
- **Every storage-like type takes a `PartitionKey` in its constructor or its lookup signature**, and
  there is no overload without one. Same technique as `net::Fetch` taking a `privacy::Verdict`: a
  signature cannot be bypassed by forgetting.
- **`PartitionKey` is on the hot path** — every request, every storage access — so it gets an
  object-size budget (`static_assert`) and a cheap hash. Interned site and origin strings, so the key
  is a few integers rather than three heap strings compared byte by byte.
- **The IPC vocabulary gains no container field.** The browser process knows a renderer's container
  from having created the process, exactly as it knows its site. A renderer that could name its own
  container could name a different one.
- **The UI work is M7**: container picker, per-tab indicator, "always open this site in…" rules.
  The mechanism lands in M2 and is usable from tests long before it has a picker.
- **Clearing data becomes per-container**, and so does the "forget this site" action.

## Alternatives Considered

**Separate OS-level profiles**, the way Chrome does. Simplest possible isolation, and it fails the
actual use case: profiles do not compose with tabs. Wanting a Work tab next to a Personal tab in the
same window is the entire feature, and a profile boundary is a window boundary.

**A `privateBrowsingId` field beside the container id**, as Firefox has. Rejected as redundant once
ephemeral containers exist: two mechanisms for "throwaway identity" means every lookup has to check
both, and the bug is the lookup that checks one.

**Container as a cookie-jar-only concept.** Cheaper, and it is the version that gets shipped when
containers are added late. Rejected because it is not a boundary — the cache, the connection pool,
and TLS session state all leak across it, and a renderer bug walks through it. A partition that
holds for cookies and not for connection reuse is a partition that holds until somebody measures it.

**Putting `Url`/`Origin` in `net` and giving `privacy` a callback interface** to avoid the cycle.
Rejected: it inverts a dependency to dodge a layering problem instead of fixing it, and it would put
a virtual call on the request path. The cycle was information — it said a module was missing.

## Open

**Whether the top-level site component should be the site or the whole partition key of the
top-level document**, for nested third-party frames (a.com → b.com → c.com). Firefox and Chrome
differ here and both have compatibility scars. Decide with a test case in hand, not in the abstract.

**Container-aware history.** Whether history entries carry a container, whether the omnibox surfaces
them across containers, and what "search history" means when the same site appears under two
identities. This is a UI design question and belongs with M7.
