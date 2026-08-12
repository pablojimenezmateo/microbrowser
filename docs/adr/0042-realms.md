# ADR 0042 — A realm is the unit of "one global", and `src/js` has to hold more than one

**Status:** accepted · **Date:** 2026-08-12 · **Supersedes:** nothing · **Amends:** ADR 0027 §1

## Context

`js::Interpreter` has exactly one global object, one global scope and one set of intrinsics. Every
consequence of that is written down in TD-0059, and the measurement that makes it the top of the
queue is written down three times over by three areas that reached it independently:

| area | what it costs there | recorded in |
|---|--:|---|
| the harness-silent bucket | **1,083 tests** use an `<iframe>` | TD-0059 |
| `url/` | 188 subtests, all `frame.contentWindow.location = badUrl` | TD-0057 |
| `dom/` | ~4,176 subtests behind two `<iframe>`s | session log, C6 |
| `dom/` harness failures | the cross-realm group: `Event-dispatch-throwing-multiple-globals`, `Event-timestamp-cross-realm-getter`, the seven `Range-*.html` | `docs/wpt-baseline.md` |

ADR 0027 landed the *lifecycle* of a nested browsing context on 2026-08-12: a child fetches, parses,
fires `load`, re-navigates on `src`, and answers `contentDocument`. It did not land the child's
*script*, and could not. `src/bindings/FrameBindings.cpp` says so at the point where it matters and
hands back a plain object with a `.document` on it, which is the stub ADR 0012 forbids, present only
because returning nothing broke more.

So this is not a new feature request. It is the one missing concept underneath a feature that is
otherwise built.

## Decision

**A realm is a first-class object in `src/js`, and an interpreter holds many.**

```
struct Realm {
  Object* global;              // the global object
  Environment* global_scope;   // where `var` and the builtins are declared
  Intrinsics intrinsics;       // one set of prototypes
};
```

`Interpreter` holds `std::vector<std::unique_ptr<Realm>>` and a `Realm* realm_` naming the one
currently running. `CreateRealm()` allocates a global, a global scope and a full set of intrinsics,
runs `InstallGlobals` against them, and answers a `RealmId`.

### 1. What is per-realm and what is shared, and why the line is where it is

**Per realm — the intrinsics.** `Object.prototype`, `Array.prototype`, `Function.prototype`, and the
prototypes of the primitives (string, number, boolean, bigint), `RegExp.prototype`,
`Promise.prototype`, `ArrayBuffer.prototype`, `%TypedArray%.prototype`, the generator and
async-generator prototypes. These are exactly the objects a page can reach and compare, and
`frames[0].Array === Array` answering *false* is the observable that tells a page it is in a second
realm. That comparison is the whole point: it is what a library uses to decide whether a value came
from somewhere else, and an engine that answered true would send every such library down the wrong
branch.

**Shared across realms — the well-known symbols.** `Symbol.iterator`, `Symbol.asyncIterator`,
`Symbol.toPrimitive`, `Symbol.hasInstance`, `Symbol.toStringTag`. The specification shares these
deliberately and every protocol that crosses a realm depends on it: an array from one realm iterated
by a `for...of` compiled in another has to be found through the *same* cell, or the protocol simply
does not connect. A per-realm `Symbol.iterator` would be the subtlest possible break — spreading a
cross-realm array would silently produce nothing.

**Shared — the engine's internal signals.** `return_signal` and `chain_signal` are not reachable
from any page: they are never a property of anything nameable, and every path that could return one
converts it first. They are one each because they are compared by identity by the engine and by
nobody else. Making them per-realm would be a cost with no observable attached.

**Shared — the heap.** One `Heap`, one collector, one set of roots that now walks every realm. This
is the decision with the most consequence and §3 is about it.

### 2. A callable carries its realm; the machine follows the callee

The rule the language needs is: **a builtin allocates from the realm of the function, not the realm
of the caller.** `frames[0].Array.prototype.map.call(x, f)` must produce an array whose prototype is
the *child's* `Array.prototype`, because that is the realm `map` came from.

So `Object` grows a `std::uint16_t realm_` — an index into `realms_` rather than a pointer, which
keeps it in padding the class already had and keeps it valid across a `realms_` growth. It is set at
creation from whichever realm is running, which means every native installed by
`CreateRealm`'s call to `InstallGlobals` gets that realm for free.

The current realm is then re-derived wherever the callee changes:

- **The machine**: `RunFrames` already reloads `frame` every iteration. The realm depends only on
  `frame->function`, so the sync is guarded by comparing that pointer against the last one synced —
  one load and one compare on the dispatch path, and the lookup only when the callee actually
  changed. **It is deliberately not keyed on the frame count**: a pop followed by a push inside one
  turn (a generator resume) leaves the count equal and the callee different, and keying on the count
  would silently run a function in its caller's realm.
- **A native, and the tree-walker's function path**: an RAII guard in `CallFunction`, because both
  hold values in C++ locals and neither goes through a frame.
- **`PushFrame`'s OrdinaryCallBindThis**: the global substituted for a null `this` is the *callee's*
  global, read from the callee's realm rather than from whatever was running.

### 3. Same-origin contexts share one interpreter; cross-origin ones get their own

This is the part that is a security decision rather than a language one.

**Same-origin**: one `Interpreter`, one `Heap`, N realms. They have to share a heap, because
same-origin frames can hand each other live objects — that is what same-origin *means* — and an
object from one heap in another is a use-after-free waiting for the first collection.

**Cross-origin**: a separate `Interpreter`, and therefore a separate heap, and therefore **no
pointer either side can hand the other, even by accident.** That is strictly stronger than the check
`Engine::OnFrameFetch` performs today, because it is not a check: there is no representable value
that crosses. It is also precisely the property ADR 0027 §5 needs for the process split to be an
extraction rather than a rewrite — a cross-origin realm already shares nothing, so moving it to
another process removes a heap rather than untangling one.

A `WindowProxy` for the cross-origin case is a *later* decision and is not in this ADR. What lands
here is the same-origin realm; cross-origin `contentWindow` stays absent, which remains a known
deviation from ADR 0027 §2.

### 4. Realms are bounded

A page creates a realm by appending an `<iframe>`, so the count is page-controlled and every
page-controlled count in this browser is bounded (ADR 0034 for the heap, ADR 0009 for parse depth).
Each realm costs a full set of intrinsics — on the order of a hundred objects and a global scope
holding every builtin. `kMaxRealms` is **64**, and exceeding it makes `CreateRealm` answer failure,
which a frame reports by not running script rather than by tearing anything down.

64 is chosen against what a page legitimately does rather than against what the index can hold: the
`std::uint16_t` could address 65,535, and a document with sixty-four *scripted* same-origin frames is
already far outside anything in ADR 0007's targets. A bound that is never reached by a real page and
is cheap to reach by a hostile one is the right shape; the alternative — one intrinsics set per
frame, unbounded — is a memory-exhaustion vector reachable from three lines of script.

### 5. The host half, which is where this gets dangerous

The language half landed on 2026-08-12 and is described above. The half that makes an `<iframe>` run
script is in `src/engine` and `src/bindings`, and it has one hazard that dominates the design.

**The hazard.** `engine::PageScript` owns `std::unique_ptr<js::Interpreter>` — one per `Page`, and a
child frame is a `Page`. For a same-origin child to share its parent's heap (§3), the child's
`PageScript` has to *borrow* the parent's interpreter and hold a `RealmId` alongside it. Every host
entry into that interpreter must then run in that realm: not only `Run`, but a timer firing, an event
dispatching, a fetch response arriving, a custom-element reaction, a microtask drain, an animation
frame. There are ~40 such uses across `PageScript.cpp` and `PageModules.cpp`.

**A missed one is not a bug, it is a same-origin escape**: the child's script would run with the
parent's global current, so `globalThis` in the frame would be the embedder's `window`. That is
strictly worse than today's stub, which merely returns too little.

**So the mechanism must make it impossible to forget rather than merely possible to get right.** The
shape to build is a realm-bound handle in `src/js` whose `operator->` returns a temporary proxy
holding a `RealmScope`:

```
class RealmHandle {                     // interpreter + realm, one value
  Access operator->();                  // Access holds a RealmScope for the full expression
};
// script_->interpreter()->Run(src)  enters the realm and leaves it, with nothing to remember
```

C++ gives this for free: the temporary returned by `operator->` lives until the end of the full
expression, which is exactly the extent a call needs. `PageScript` then holds a `RealmHandle` instead
of a pointer, and the ~40 sites that say `interpreter_->X` keep saying it. The handful that pass
`*interpreter_` as a `js::Interpreter&` to an installer are the ones to convert deliberately, because
those are the sites where a guard genuinely has to be written by hand — and after the conversion they
are the *only* ones, which is a list short enough to audit.

**Do not do this by adding `RealmScope` at 40 call sites.** It would work on the day it was written
and would be wrong at the first new entry point, and the failure is silent.

**And the 40 `interpreter_` sites are not the boundary anyway** — this was checked rather than
assumed, and it is the correction that matters most here. About a third of them are null tests that
run no script at all. Worse in the other direction, `PageScript` reaches script through `bindings_`
far more often than through `interpreter_`: `DispatchClick`, `DispatchPointerMouse`,
`DispatchSubmit`, every observer delivery and every reflected-attribute reaction call into the
interpreter *inside* `src/bindings`, where no realm is in scope and none can be. Guarding
`interpreter_->…` would therefore produce something that looks guarded, passes a test that runs a
child's `<script>`, and still runs the child's click handler in the parent's realm.

So **the boundary is `engine::PageScript`'s own public API**, not its uses of the interpreter. That is
the right seam for an independent reason: a `PageScript` *is* the script half of one document, so the
realm is a property of the object rather than of each call, and there is exactly one of these per
browsing context. What has to be decided is how each public method acquires the guard without a
future method being able to omit it — a private `Enter()` returning the scope that every method opens
with is the cheap version and is still omittable; making the public methods thin wrappers over a
guarded private implementation is the version that cannot be. Pick one deliberately, and say which in
the commit, because this is the decision the whole security property rests on.

The rest of the host half, in the order it has to happen:

1. `PageScript` borrows an interpreter and a realm rather than owning one, behind `RealmHandle`.
2. `FrameTree::SetDocument` asks the parent's interpreter for a realm when the child is same-origin,
   and does not when it is not — the same structural check ADR 0027 §2 already makes there, so
   `CreateRealm` returning nullopt and a cross-origin child take the same path.
3. `src/bindings` installs a second DOM surface into the child realm. The surface is already
   receiver-based (`Document.prototype` resolves against its receiver), which was the hard part and
   is done. What is not: `DomBindings` caches a wrapper per node, so two of them over one heap means
   a node reachable from both documents has two wrappers. `parent.document.body === parent.document.body`
   from the child must stay true, so the cache has to be shared or keyed by realm — decide which
   before writing either.
4. `contentWindow` returns the child realm's global, and the plain-object stub in
   `FrameBindings.cpp` is deleted.
5. `parent`, `top`, `frames`, `window.length`, `defaultView`, and `postMessage` between same-origin
   realms.

`dom/nodes/remove-from-shadow-host-and-adopt-into-iframe.html` is the smallest end-to-end check for
steps 3–4: it does `iframe.contentWindow.document.body.appendChild(adopted)` and then compares
pixels. It is **red on master today while the expectation file claims it passes**, which is worth
knowing before trusting that file about this area.

## Consequences

- **`contentWindow` becomes a real global object** for a same-origin frame, and the plain-object stub
  in `FrameBindings.cpp` is deleted rather than extended.
- **The binding layer installs its surface per realm.** The DOM's own constructors are intrinsics of
  a realm exactly as `Array` is, so `frames[0].HTMLDivElement !== HTMLDivElement` for the same reason
  and by the same mechanism. This is why the work in `src/bindings` is not additive: the installers
  have to take a realm, and the ones that memoise an interface object per interpreter are wrong.
- **`instanceof` across a frame boundary starts answering correctly**, which it cannot today.
- **The collector's root set grows a loop.** Every realm's global, global scope and intrinsics are
  roots. A missed realm is a use-after-free on a live page, so `Realm::Roots()` is one function next
  to the fields rather than a list maintained apart from them — the same rule `WellKnown::Roots` was
  already written under, and the reason `Intrinsics` keeps that shape.
- **Cost on the dispatch path**: one pointer compare per instruction, and a three-load realm lookup
  only when the running function changes. Measured before and after with `bench/JsBenchmarks.cpp`;
  the number goes in the commit message rather than here, because this file should not have to be
  re-dated when the machine gets faster.
- **What this does not do**: it does not give a cross-origin frame a `WindowProxy`, it does not make
  `postMessage` cross a process, and it does not run a worker. Workers are ADR 0022 and want a realm
  *and* a scheduler; this is the half they share.

## Alternatives rejected

**Proxy a few names from the child onto the parent's global.** Rejected in TD-0059 already and the
reason is worth keeping here: it makes `iframe.contentWindow.document === iframe.contentDocument`
true and `frames[0].Array === Array` true, and the second is the observable a page uses to detect
exactly this situation. The tests that would start passing are the ones checking the shallow answer;
the pages that would break are the ones that feature-detect and take the native path.

**One `Interpreter` per browsing context, with a value-copying bridge between them.** This is what
the code did before this ADR. It cannot express a same-origin frame at all: `parent.foo = childObj`
is a live reference in every other browser, and a copy is a different object that stops tracking.

**A realm as a `Heap`-level concept, with per-realm collection.** Attractive — it would make a realm
droppable in constant time. Rejected because same-origin realms hold references to each other by
design, so the collector cannot treat either as independently reachable; it would need cross-heap
remembered sets, which is a larger machine than the whole of the rest of this decision.
