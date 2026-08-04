# ADR 0008 — The DOM Binding Layer

**Status:** accepted · **Date:** 2026-08-04

## Context

`src/js` runs JavaScript and knows nothing about the document. `src/dom` holds the document and
knows nothing about JavaScript. `src/js/MODULE.deps` forbids the first from including the second,
with a comment saying why: *"A JS engine that could reach into the DOM directly would make the
binding layer optional, and the binding layer is where every same-origin check will live."*

That forbidding is a promise about a layer that does not exist yet. This ADR decides what it is,
before it is written, because two of its properties cannot be retrofitted.

The engine reached the point where this is the next thing worth building. It has a regular
expression engine, symbols and the iteration protocol, `Map`/`Set`/`WeakMap`/`WeakSet`, Promises
with a microtask queue, `Proxy` and `Reflect`, and most of the standard library. What it does not
have is any way to affect a page: it is a complete calculator wired to nothing.

## Decision

### The bindings are their own module, not part of either side

`src/bindings`, depending on `util`, `js` and `dom` — the only module in the tree allowed to see
both of the last two.

The alternative, putting the bindings in `src/js` behind a callback interface, was rejected for
the reason the `MODULE.deps` comment already gives: it makes the layer optional. A callback that
can be left unset is a check that can be left unset, and the check this layer performs is the
same-origin one. A module boundary the architecture lint enforces is a promise the compiler keeps.

`src/engine` owns an instance of it, because the engine owns both the document and the script that
runs against it.

### A wrapper is a JavaScript object holding a raw node pointer, and that is safe only while
### nodes are never freed

Every DOM node reachable from script needs a JavaScript object standing for it. The wrapper holds
a `dom::Node*`, which is a pointer into a tree the bindings layer does not own.

Today that is safe, and the reason is narrower than it looks. `dom::Node::Remove` exists and its
comment says it "detaches and destroys" — but **nothing in the tree calls it.** It is unused code,
so in practice no node is ever freed before its document, and a raw pointer cannot dangle.

That is a thin thing to rest on. The first caller makes a wrapper able to outlive its node, and a
raw pointer in a garbage-collected object is a use-after-free reachable from a page — which is to
say, an RCE primitive rather than a crash. Binding `removeChild` would be that first caller, which
is one of the reasons it is not in the first slice below.

So the rule is written here rather than discovered later: **whoever gives `Node::Remove` a caller
owns fixing this at the same time.** The fix is not a detail of that patch; it is the reason that
patch is not a small one. Two shapes work, and either is acceptable:

- The wrapper holds a `std::shared_ptr<dom::Node>` and removal detaches rather than frees, so a
  removed node stays alive exactly as long as script still refers to it. This is what the DOM
  specification's own object model implies and what every real engine does.
- The bindings layer keeps a graveyard of removed nodes for the lifetime of the document. Simpler,
  and it leaks a removed subtree until navigation — which for a browser that navigates away from a
  page is a bounded leak rather than an unbounded one.

The first is correct; the second is defensible. Keeping the raw pointer is neither.

### Wrapper identity is preserved, and the cache lives on the JavaScript side

`document.body === document.body` has to be true, and so does `element.parentNode === document
.body`. Script uses object identity as a set key, a map key and a cache key, and a binding layer
that hands out a fresh wrapper per access breaks all three quietly.

So there is a cache from node to wrapper. It is held **in the JavaScript heap**, reachable from the
`document` wrapper, rather than in a C++ table beside it. The reason is the collector: a C++ table
of `Object*` would have to be registered as a GC root, and the interpreter has no API for a third
party to add roots — adding one would let any module keep any object alive forever, which is the
kind of capability that gets used by accident. A cache the collector can already see needs no new
capability.

The consequence is that the cache keeps every wrapped node's wrapper alive for as long as the
document is. That is a real cost and the right one for now: it is bounded by the number of nodes
script has touched, and the alternative is a weak cache, which is exactly the ephemeron machinery
the heap grew for `WeakMap` and which can be moved to when the cost shows up in a measurement.

### The security check has one place to be, and it is here

Every property read, every method call and every navigation that crosses from one document to
another passes through this module. There is no other path: `src/js` cannot see `src/dom` and
`src/dom` cannot see `src/js`.

That is the whole point of the layering, and it is what makes "did we check the origin" a question
with one answer rather than one per call site. When frames exist, the check goes here. When
`postMessage` exists, the check goes here. A binding that reaches a node belonging to another
document without asking is a bug in one file.

### What the first slice is

Deliberately small, and complete rather than broad:

- `document`, with `getElementById`, `querySelector`, `getElementsByTagName`, `createElement` and
  `createTextNode`.
- On an element: `tagName`, `id`, `className`, `textContent` (both directions),
  `getAttribute`/`setAttribute`/`hasAttribute`, `children`, `parentNode`, `appendChild`.

No events, because an event loop that dispatches them has to be checked against the zero-idle-CPU
invariant first, the same way the microtask queue was. No `innerHTML`, because it means running the
HTML parser on a string from script into a live tree, which is the single most dangerous binding in
a browser and deserves to be added on purpose rather than in a first slice.

## Consequences

- The architecture lint gains a module whose whole existence is a security boundary. It should be
  read as one: a change to `src/bindings/MODULE.deps` that widens `allow:` is a change to the
  browser's security model.
- `src/dom` gains a constraint it did not have: adding node removal is now a change with a named
  consequence in another module, recorded here rather than in someone's memory.
- The engine gains the ability to run a page's script against its own document, which is the last
  structural thing standing between "renders a page" and "runs a page".

## Alternatives considered

**Bind through `src/engine` rather than a module of its own.** Rejected: `src/engine` already
depends on everything, so putting the bindings there would make the layer invisible to the lint and
its boundary unenforceable. The point of a separate module is that the compiler can tell when
someone crosses it.

**Expose the DOM to `src/js` and let script reach it directly.** Rejected for the reason the
existing manifest comment gives, and worth restating: the binding layer is where the same-origin
check lives, and a layer that can be bypassed is not a check.
