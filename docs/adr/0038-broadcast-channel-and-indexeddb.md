# ADR 0038 — `BroadcastChannel`, and IndexedDB's memory tier

**Status:** accepted · **Date:** 2026-08-09

## Context

TD-0020's Woffle diagnosis narrowed to two names. youtube's offline entity store
feature-detects before it builds anything. The check that actually decides is
not the `typeof` list alone — `yPS` also requires:

```js
"IDBTransaction" in self && "objectStoreNames" in IDBTransaction.prototype
```

then opens a throwaway database. `plI` only constructs `W3O` (the PES encoder)
after `g.wU()` and a live `BroadcastChannel` both succeed — the channel is `X_`,
named `PERSISTENT_ENTITY_STORE_SYNC:…`, and it is how the offline store tells
another tab of the same site that an entity changed. Neither API existed in this
browser. ADR 0012's rule is the one that matters here: a stub that answers
`typeof indexedDB === 'object'` but has no real `IDBTransaction` behind it is
worse than an absence, because it sends `g.wU()` down the true branch into a wall.

ADR 0021 §5 already committed to IndexedDB — "the largest implementation on the storage list" —
and named its shape: databases, object stores with a `keyPath`, indexes, and the same
`PartitionKey` every other store in that ADR takes. It did not cover `BroadcastChannel`, which is
not a storage API at all; this ADR is where that gap gets a decision.

## Decision

### 1. `BroadcastChannel` is real, and document-scoped is a named gap, not a stub

A `BroadcastChannel` extends `EventTarget` (`InstallEventMethods`, over a prototype `MakeInterface`
builds), and `postMessage` structured-clones its argument through `js::StructuredSerialize` before
handing it to every *other* live channel of the same name — never the sender, per specification.
Delivery goes through `TimerQueue::QueueTask`, exactly like `MessagePort::postMessage`
(`MessageChannels.cpp`), for the same reason: a page reaches for a channel specifically to cross a
macrotask boundary, and a microtask here would make that promise false.

**"Every other live channel of the same name" means every channel `this document` has constructed
today.** This browser has one document per `DomBindings`, and `src/bindings` may not see a
`PartitionKey` (or `src/engine`, or `src/storage`) at all, so there is nowhere in this module to
fan a message out to a second page. That is enough for `postMessage` to a channel of the same name
in the same script and enough for `g.wU()`'s feature detect, but not enough for two tabs of
`youtube.com` to hear each other's `PERSISTENT_ENTITY_STORE_SYNC:…` traffic. Widening it to the
partition is `src/engine`'s to do, the same way ADR 0021 treats a `storage` event: fan-out across
documents happens on the far side of the seam, once there is a second document to fan out to. This
is written down as a gap rather than silently left for someone to discover, per ADR 0012.

### 2. IndexedDB gets ADR 0021's memory tier, not its persistent one

Every database lives in `storage::PartitionedIndexedDb`, in memory, for the life of the browser
session — ADR 0021 §2's *middle* tier, the one that needs no user act and outlives nothing. The
*persistent* tier (disk, encrypted, opt-in per site) is explicitly out of scope here: this ADR
closes the platform gap TD-0020 named, not the whole of ADR 0021 §5. A page that expects its
entity cache to survive a browser restart does not get that yet; one that expects it to survive
navigating away and back within a session does.

**Quota is 50MiB per partition**, enforced in `PartitionedIndexedDb` rather than trusted to a
caller — the same posture ADR 0021 §3 states for storage generally: a page's write is a denial of
service against the process until something bounds it, and the bound belongs at the store.

**Keys are number, string, or array of either** — enough for EntityStore's own two shapes
(`keyPath: "key"` and the compound `["parentEntityKey", "childEntityKey"]`) and nothing wider. No
`Date` key, no binary key, no `autoIncrement`. Each absence is deliberate under ADR 0012 rather
than a stub: a half-built cross-type key ordering (number < date < string < binary < array per
specification) that this browser has never needed is the shape that rule calls worse than not
having it, because a page that probes for it and finds something that silently orders wrong is
harder to diagnose than a page that finds nothing.

### 3. The seam is `bindings::IndexedDbSource`, the same inversion as `StorageSource`

`src/bindings` declares the interface; `src/engine` implements it (`EngineIndexedDb.cpp`) and is
the only place that ever holds a `PartitionKey` — a binding cannot name a partition even by
accident, because it has no type to hold one in. This is the identical shape ADR 0015 used for
geometry and ADR 0021 used for `sessionStorage`/`localStorage`, applied a third time rather than
invented again.

Everything behind the seam is a synchronous call: the store is in memory, so there is nothing to
wait for. **The only thing `TimerQueue::QueueTask` defers is the completion *event*, not the
work** — exactly the distinction `MessagePort::postMessage` makes between the send and the
delivery. That is what lets a promise (or a page's own generator-based scheduler) built on an
`IDBRequest` settle on a later turn rather than inside the call that made the request, which is
the specification's rule and is a property real pages rely on.

### 4. An `on<type>` handler property is an accessor over a hidden slot, never plain data

This is a correctness rule this session had to *discover* rather than one it started with, and it
generalises past this ADR's two features. `EventDispatch.cpp`'s `RunListenersOn` treats
`holder.Get("on" + type)` as an *implicit* listener on every dispatch — the specified behaviour for
an HTML `onclick`-style attribute. A handler installed as an ordinary data property (`req.onsuccess
= fn` stored as `Set("onsuccess", fn)`) is visible to that implicit read *and* to whatever explicit
check a feature's own delivery code performs, so it fires twice: once explicitly, once again from
`dispatchEvent`'s own attribute pass. `MessagePort::onmessage` already avoided this by defining an
accessor over a hidden `#onmessage` slot — `Object::Get` never returns a value for an accessor
property, which is what keeps the implicit read from seeing it — but nothing had named the rule, so
`BroadcastChannel.onmessage` and every `on<type>` property IndexedDB's request/transaction objects
expose were written as plain data and each delivered its handler twice, caught by this ADR's own
test suite (`BroadcastChannel/PostMessageDeliversAsATaskNotAMicrotask` failed with a duplicated
message before the fix).

`DomBindings::InstallOnEventAccessor` (`EventBindings.cpp`) is now the one place this pattern is
written, and every `on<type>` property this ADR adds — `BroadcastChannel.onmessage`/
`onmessageerror`; `IDBRequest.onsuccess`/`onerror`; `IDBOpenDBRequest.onupgradeneeded`/
`onblocked`; `IDBTransaction.oncomplete`/`onerror`/`onabort` — goes through it, defined once on
the shared prototype rather than per instance. A future `on<type>` property that is written as
plain data again is the bug this paragraph exists to prevent.

### 5. What is deliberately narrow, beyond the two absences above

- **No versionchange transaction distinct from a normal one.** `createObjectStore`/`createIndex`
  are callable on any `IDBDatabase` this binds rather than only inside `upgradeneeded`. Honest
  about what this engine actually checks rather than a half-enforced version of the restriction.
- **`IDBKeyRange` is `.only()` and nothing else.** No `.bound()`/`.lowerBound()`/`.upperBound()`,
  and no open/closed range on a cursor. EntityStore's own use is `IDBKeyRange.only(key)`; a
  half-built range comparison across key types this browser barely orders is the ADR 0012 stub.
- **An index's `keyPath` lives only in this document's own bindings-side metadata.** `src/storage`
  cannot see `js`, so it cannot extract a key from a value — only `src/bindings` can, and it
  remembers an index's `keyPath` in a table keyed by `(db, store, index)` rather than teaching
  `storage::IndexedDbIndexDef` one. **The consequence:** a second document of the same origin that
  reopens an existing database at a version it has already seen never calls `createIndex` again
  (nothing fires `upgradeneeded`), so a `put()` there cannot populate that index. Recorded rather
  than fixed — fixing it is a real widening of `storage`'s contract, not a one-line change.

## Consequences

- **Positive:** `g.wU()`'s feature-detect shape passes against real constructors — not `typeof
  === 'object'` stubs — and `plI`/`W3O`/`X_` construct in a page that only needs these two APIs.
- **Positive:** The accessor rule (§4) is now written down and mechanised in one function; the next
  `on<type>` property anyone adds inherits the fix rather than repeating the bug.
- **Negative:** `BroadcastChannel` does not cross documents yet (§1). A test that opens two tabs of
  the same site and expects them to hear each other will fail until `src/engine` widens the
  fan-out.
- **Negative:** IndexedDB does not survive a restart (§2), and an index's schema does not survive a
  reload without a version bump (§5). Both are named rather than silent.
- **Negative:** Quota, key shape, and `IDBKeyRange` are all narrower than the specification. Each
  narrowing is listed so a future session extending this ADR knows what was left out and why,
  rather than rediscovering it against a failing page.

## Alternatives considered

**Widen `BroadcastChannel` to the partition immediately.** Rejected for this ADR. It requires
`src/engine` to hold a registry of live channels across every document sharing a partition key and
a way to reach a *different* document's `DomBindings` to deliver into it — real work with its own
design questions (does a channel in a background tab still receive while unloaded?) that TD-0020
does not need answered to pass `g.wU()`. Named as the next step rather than attempted here.

**Persist IndexedDB by default.** Rejected outright under `AGENTS.md`: "nothing persists to disk
unless the user opted in" is not negotiable, and ADR 0021 §2 already settled that IndexedDB gets
the same three-tier treatment as `localStorage`. This ADR implements the middle tier because that
is what TD-0020 needs; the persistent tier is ADR 0021's to finish, for every store at once.

**Fix the double-delivery bug by removing the explicit handler call instead of adding an
accessor.** Considered and rejected: the explicit call is what lets delivery code run the handler
even when a page constructed the object without ever calling `addEventListener` — removing it
would make `dispatchEvent`'s implicit attribute read the *only* path, which is correct for a real
`on<type>` property but means every future feature must remember dispatchEvent's specific
behaviour rather than a rule at the property's definition site. Defining the property once, as an
accessor, keeps the guarantee at the one place a property is declared rather than at every call
site that delivers one.
