# The JavaScript engine: what is missing, and in what order

This is an audit of `src/js` against the language, taken by running the engine rather than by
reading it. The method: a scratch shell that runs a snippet and prints its console output, and
~180 probes over the syntax and the standard library. Everything below was observed, not guessed,
and each line says what the engine actually did.

The ordering is the repository's own: **correctness first**. A feature that is silently wrong
outranks a feature that is missing, because a missing one throws at the call and a wrong one
returns a plausible answer three lines later. Within that, syntax the parser rejects outranks
builtins that are absent, because a rejected token kills the whole script and an absent builtin
kills one call.

## Tier 1 — things that exist and are wrong

These are the expensive ones. Each is a feature the engine advertises and answers incorrectly.

| # | What | Observed | Should be |
|---|---|---|---|
| 1.1 | Strings are UTF-8 bytes, not UTF-16 code units | `'é'.length` → `2`; `'a😀b'.length` → `6`; `String.fromCharCode(233)` → a broken byte | `1`, `4`, `'é'` |
| 1.2 | `ToPrimitive` does not exist — no `valueOf`, no `toString`, no `Symbol.toPrimitive` | `[] + {}` → `NaN`; `+[]` → `NaN`; `[1] == 1` → `false`; `date - date` → `NaN` | `'[object Object]'`, `0`, `true`, a number |
| 1.3 | `let` in a `for` head is one binding, not one per iteration | `for(let i=0;i<3;i++)fs.push(()=>i)` → `3,3,3` | `0,1,2` |
| 1.4 | An optional chain guards one link, not the chain | `(null)?.a.b.c` → TypeError | `undefined` |
| 1.5 | Destructuring with a computed key is parsed and ignored | `const {[k]:v} = {q:8}` → `v` is `undefined` | `8` |
| 1.6 | Integer keys are not ordered first | `Object.keys({b:1,a:2,1:3,0:4})` → `b,a,1,0` | `0,1,b,a` |
| 1.7 | `Object.prototype.toString` ignores `null` and `Symbol.toStringTag` | `.call(null)` → `[object Undefined]` | `[object Null]` |
| 1.8 | `Array.prototype.toString` is inherited from `Object` | `[1].toString()` → `'[object Array]'` | `'1'` |
| 1.9 | The call stack is 256 frames | `f(2000)` tail-free recursion → RangeError | a page recurses further than that |
| 1.10 | `globalThis` is a bare object, not the global | `globalThis.Math` → `undefined` | `Math` |
| 1.11 | `instanceof` ignores `Symbol.hasInstance` | a class with the trap → `false` | the trap's answer |
| 1.12 | Relational operators compare strings by UTF-8 bytes | works by accident for ASCII | code-unit order |

## Tier 2 — syntax the parser rejects

A page that contains any of these does not run at all.

| # | What | Observed |
|---|---|---|
| 2.1 | `a?.[k]` | `SyntaxError: expected a property name after '.'` |
| 2.2 | `new.target` | `SyntaxError: unexpected token '.'` |
| 2.3 | Class static blocks — `class C { static { ... } }` | `SyntaxError: expected ';'` |
| 2.4 | The brand check — `#x in o` | `ReferenceError: #x is not defined` |
| 2.5 | Nested destructuring with a default — `const {b:{c}={c:9}} = {}` | `SyntaxError: invalid assignment target` |
| 2.6 | BigInt literals — `10n` | `SyntaxError: invalid token` |

Private fields, private methods, static private fields, class fields, computed method names,
optional call (`f?.()`), logical assignment, numeric separators, optional catch binding and
labelled statements all work.

## Tier 3 — builtins a real page calls

Enumerated with `Object.getOwnPropertyNames` over every constructor and prototype the engine has.

- **`Object`** — missing `is`, `seal`, `isSealed`, `preventExtensions`, `isExtensible`,
  `getOwnPropertySymbols`, `getOwnPropertyDescriptors`, `groupBy`. `Object.prototype` missing the
  `__proto__` accessor and `toLocaleString`.
- **`Array`** — missing `copyWithin`, `toSorted`, `toReversed`, `toSpliced`, `with`, `toString`,
  `toLocaleString`, `Array.prototype[Symbol.unscopables]`.
- **`String`** — missing `String.raw`, `String.fromCodePoint`, `codePointAt`, `normalize`,
  `localeCompare`, `substr`, `toLocaleUpperCase`/`toLocaleLowerCase`, `trimLeft`/`trimRight`.
- **`Number`** — missing `toPrecision`, `toExponential`, `toLocaleString`.
- **`Boolean`** — has no prototype at all: `true.toString()` throws.
- **`Math`** — missing `clz32`, `fround`, `imul`, `expm1`, `log1p`, `sinh`, `cosh`, `tanh`,
  `asinh`, `acosh`, `atanh`, and the constants `LOG2E`, `LOG10E`, `SQRT1_2`.
- **`Date`** — twelve methods out of ~forty-five. Missing `Date.parse`, `Date.UTC`, every
  `getUTC*`, every setter, `getTimezoneOffset`, `toString`, `toDateString`, `toJSON`, the
  `toLocale*` family, and `Symbol.toPrimitive`.
- **`JSON`** — `stringify` takes neither an indent nor a replacer and ignores `toJSON`;
  `parse` takes no reviver.
- **`RegExp`** — has `source`, `flags`, `lastIndex`. Missing the `Symbol.match`/`replace`/
  `split`/`search` protocol, so a user object cannot stand in for a pattern. `/u` does not make
  `.` match an astral character, and `\p{...}` is unsupported.
- **`Reflect`** — missing `construct`, `getOwnPropertyDescriptor`, `isExtensible`,
  `preventExtensions`.
- **`Function.prototype`** — missing `toString`.
- **`Error`** — missing `cause`, `stack`, and `AggregateError`.
- **`Symbol`** — missing `species`, `match`, `replace`, `search`, `split`,
  `isConcatSpreadable`, `unscopables`, and `Symbol.prototype.description` as a getter.
- **Globals** — missing `isFinite`.

## Tier 4 — subsystems that are absent

| # | What | Why it matters |
|---|---|---|
| 4.1 | `Proxy` traps beyond `get`/`set`/`has` — no `deleteProperty`, `ownKeys`, `getOwnPropertyDescriptor`, `defineProperty`, `apply`, `construct` | `new Proxy(fn, {apply})` is how every framework wraps a function |
| 4.2 | `ArrayBuffer`, the typed arrays, `DataView` | binary data, canvas pixels, `fetch(...).arrayBuffer()` |
| 4.3 | `class X extends Array` / `extends Error` — subclassing a builtin | `class HttpError extends Error` is in every codebase |
| 4.4 | `WeakRef`, `FinalizationRegistry` | caches |
| 4.5 | BigInt as a value type | rarer, but it is a *type*, so `typeof` and every operator have a case |
| 4.6 | Modules | a loader question as much as a VM one — see `AGENTS.md` |

## Known gaps already written down in the source

- `yield*` does not forward `throw` and `return` to its delegate.
- A throw that unwinds past a `for...of` does not close the iterator.
- An unhandled rejection gets a console line and nothing more.

## The order of work

1. **Tier 1 minus strings** (1.2–1.12). Each is small, each is a wrong answer, and none of them
   depends on the others.
2. **Tier 2.** All six are parser work plus a compiler opcode or two.
3. **Tier 3.** A sweep through the builtin files. Mechanical, and the bulk of the surface a page
   touches.
4. **Tier 1.1, strings.** The deep one. Left until the surface around it is settled so it is
   changed once. The plan is not to replace `std::string` — the bindings seam, the HTML parser and
   the CSS parser all speak UTF-8 — but to make the *indexing* UTF-16, with an ASCII fast path
   that is the identity, so the common case costs a flag test.
5. **Tier 4.1 and 4.3**, then **4.2**.
6. **4.4, 4.5**, then modules as their own piece of work.
