# The JavaScript engine: what it has, and what it still does not

This began as an audit of `src/js` against the language, taken by running the engine rather than
by reading it: a scratch shell that runs a snippet and prints its console output, and ~180 probes
over the syntax and the standard library. The probes went from **110 passing to 173**, and what
follows is the state after that work — what is done, what is deliberately approximate, and what
is genuinely left.

The ordering below is the repository's own: **correctness first**. A feature that is silently
wrong outranks a feature that is missing, because a missing one throws at the call and a wrong one
returns a plausible answer three lines later.

## What was wrong and is now right

Every one of these was a wrong answer the engine gave, found by running it.

| What | Was | Is |
|---|---|---|
| `ToPrimitive` — `valueOf`, `toString`, `Symbol.toPrimitive` | `[] + {}` → `NaN`; `[1] == 1` → false | the spec's, with the hints written out |
| `let` in a loop head | one binding: `3,3,3` | one per iteration: `0,1,2` |
| An optional chain | guarded one link, so `(null)?.a.b` threw | short-circuits as a chain |
| `a?.[k]` | a SyntaxError | computed *and* optional are bits now |
| String indexing | UTF-8 bytes: `'é'.length` was 2 | UTF-16 code units, over UTF-8 storage |
| Property attributes | none, so `defineProperty` was enumerable | enumerable, writable, configurable |
| Enumeration order | insertion | integer keys first, ascending |
| `for...in` | own properties only | up the prototype chain, built-ins hidden |
| Call depth | 200 frames, C++-bound | 4096, bounded by the machine |
| `class X extends Error` | an error with no message | the base allocates the right kind |
| `new Proxy(fn, {})` | not callable at all | callable, with `apply` and `construct` |
| `yield*` | a loop: `throw` and `return` stopped outside | forwards to the delegate |
| A throw past a `for...of` | dropped the cursor | closes the iterator |
| `Date` | 12 methods, through `std::tm` | a computed calendar, ~45 methods, a parser |
| `JSON` | no replacer, no indent, no reviver, no `toJSON` | all four, and a named cycle error |
| `.` under `/u` | one byte | one code point |

## What arrived that was simply absent

- **Modules.** `import`/`export` in every form, `import.meta`, `import()`. The host supplies a
  resolver; the engine loads depth-first, keys by resolved name, and evaluates post-order. A
  module's body runs on the machine, which is what lets an `async function` inside one work.
- **BigInt.** The last type the language has. Arbitrary precision in base 2^32, in its own
  translation unit that knows nothing about JavaScript values; the digits live beside the heap
  keyed by the cell in the `Value`, so `Value` gained no member. Mixing a bigint and a number is a
  TypeError, enforced before any conversion runs.
- **`ArrayBuffer`, the nine typed arrays, `DataView`.** One `BufferView` record shared by all
  three, and `Object::ElementCount`/`GetElement`/`SetElement` consult it — so every generic
  `Array.prototype` method already works on a typed array.
- **`new.target`**, class **static blocks**, the private **brand check** (`#x in o`), object
  **rest** in a pattern, **computed keys** in a pattern, nested patterns with defaults.
- **`\p{...}`** and code-point `.` under `/u`.
- **The pattern protocol** — `Symbol.replace` and its four siblings, so a library object can stand
  in for a RegExp.
- **Named evaluation** — `const f = () => {}` gives the arrow the name `f`.
- **`WeakRef`, `FinalizationRegistry`**, `Symbol.species`, `escape`/`unescape`, `Error.stack`,
  `AggregateError`, `Reflect.construct`, `Function.prototype.toString`, `globalThis` as the real
  global, and roughly sixty missing methods across `Object`, `Array`, `String`, `Number`, `Math`
  and `Boolean` — which had no prototype at all.

## The deviations, each deliberate and each written where it lives

These are not gaps to be closed by accident. Each is a decision with a reason at the site.

| What | Why |
|---|---|
| A module import binds **by value**, not as a live view | Live bindings need every use of an imported name to compile to an indirection, which is a scope analysis this engine does not have. Binding by value is what every bundler's output behaves like. In `Modules.cpp`. |
| `String.prototype.normalize` returns its input | The canonical forms need the Unicode decomposition tables — a megabyte and a build step. Inventing a normalisation is worse than not having one. |
| `\p{...}` covers a subset of properties | The full data is generated from the standard. What is here is the blocks a page's text is in; an unknown property is a **SyntaxError**, not a silently empty set. |
| Case conversion covers Latin, Greek and Cyrillic | The ranges where case is arithmetic. Everything else is left as itself, which is the answer that cannot be wrong. |
| `WeakRef` holds its target strongly | A correct one needs a per-turn keepalive so that collection timing is unobservable. A cache that never evicts is a memory cost, not a wrong answer. |
| `FinalizationRegistry` never calls back | Conforming: the spec says one may never be called, precisely so an engine can collect on its own schedule. |
| `localeCompare` compares by code point | A real collation needs locale data. A page sorting a list gets a stable, predictable order rather than a fabricated one. |
| `toLocaleString` is the plain form everywhere | No locale data. A fabricated format is a lie a page cannot detect. |
| The tree-walker refuses `async`/generators | A wrong answer three lines later is worse than a refusal at the call. It is the differential engine now, not a fallback. |

## What is genuinely left

1. **Annex B block-function hoisting.** `if (x) { function f(){} }` puts `f` in the enclosing
   function scope in a browser. Small, and only matters for old code.
2. **`Intl`.** Not the core language, and a real one needs CLDR.
3. **`Atomics` / `SharedArrayBuffer`.** Needs the process model first — see ADR 0004.
4. **`BigInt64Array` / `BigUint64Array`.** Now that BigInt exists these are two more entries in a
   table; they were left out with the rest of the typed arrays and are the smallest thing here.
5. **Full Unicode tables** for `normalize`, the case mappings, and the rest of `\p{...}`. One
   dependency decision (ADR 0001) and one build step, not one piece of code.
6. **Live module bindings**, if a real page is ever found that needs them.
7. **Wiring the module resolver to the loader** — an import is a fetch, and a fetch has to pass
   the privacy layer. Engine work rather than language work.

## What a real site's script found next

The audit above was probes. The pass after it was youtube.com — fourteen scripts, loaded and run
with their errors surfaced — and it is worth recording separately because the two methods found
different *kinds* of bug. Probes find what you thought to ask about. A real bundle finds what
everybody's minifier emits.

Every one of these was a wrong answer, not a missing feature, and none of them was on the list
above:

| What | Was | Is |
|---|---|---|
| **`var` scope** | block-scoped: `{ var n = 1 }` was invisible outside the block, and `for (var i…) {} return i` was a ReferenceError | function-scoped |
| **`var` hoisting** | absent: a read before the declaration's line was a *TDZ error* | the binding exists from function entry, holding undefined |
| Top-level `this` | undefined, in scripts and modules alike | the global object in a script; undefined only in a module |
| `for (k in o)` | `in` parsed as a relational operator, so the head never matched — seven of the fourteen scripts died here | the grammar's `[~In]` parameter |
| `for (k in a = a \|\| {}, o)` | a for-in's right side parsed as an `AssignmentExpression` | an `Expression`, as the grammar says; for-of keeps the narrower form |
| `ɵprov` | a lexer error | an identifier; escaped and unescaped spellings are one name |
| `super()`'s return | discarded: a base constructor returning an object did not become the derived `this` | it does, which is the rule an element is upgraded in place by |
| Parse depth | 256 `Depth` units, ~85 levels of nesting — a round number | 1024, measured at a 12x margin from where the stack actually overflows (ADR 0009) |

The `var` pair is the one to take a lesson from. Both engines were wrong **identically**, so the
differential — the tool that has found every other engine disagreement in this project — could not
say a word about it. Two engines agreeing is evidence, and it is not proof.

The tool that did find them is `tools/jsshell`: it runs one file, and its `-p` mode reports a
syntax error by *offset* with sixty characters of source either side. A minified bundle is one line
of 200KB, so a line number locates nothing. With it, the whole 10.7MB application bundle now parses.

## How to check any of this

The probes are not in the repository — they were a scratch harness, and the ones worth keeping
became tests. `tests/JsConformanceTests.cpp` is the audit turned into assertions: every case there
was written from an *observed wrong answer* rather than from reading the spec, which is what makes
it worth having beside the feature-by-feature suite in `JsInterpreterTests.cpp`.

`MICROBROWSER_JS_TREEWALK=1` runs everything through the tree-walker. Forty tests are expected to
fail and the list is at the top of `tests/JsVmTests.cpp`; anything else appearing there is a
difference nobody decided on.
