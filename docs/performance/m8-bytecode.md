# The bytecode machine: what it bought, and where the time still goes

Measured with `bench/JsBenchmarks.cpp`, which registers every workload twice — once compiled, once
tree-walked — so the ratio is readable off one table instead of inferred from two runs on two days.
Both sides skip parsing and skip constructing an interpreter: the timed body is evaluation and
nothing else.

```
cmake --preset microbrowser-perf
cmake --build --preset microbrowser-perf --target microbrowser_bench
./build/microbrowser-perf/microbrowser/microbrowser_bench js/
```

## The numbers

Per unit of work, lower is better. Ratio is tree-walker ÷ machine. Taken after
slot resolution; the "before slots" column is the machine's own number when
bindings were still names in a hash map, which is what that change bought.

| Workload | Machine | Before slots | Tree-walker | Ratio |
|---|---|---|---|---|
| `js/closures` — an arrow made and called per iteration | 597 ns | 611 ns | 3900 ns | **6.5×** |
| `js/fib` — `fib(24)`, 150k calls | 259 ns/call | 255 ns/call | 573 ns/call | **2.2×** |
| `js/method-calls` — `o.step()` 100k times | 330 ns/call | 330 ns/call | 762 ns/call | **2.3×** |
| `js/property-reads` — `o.a + o.b + o.c` | 169 ns | 239 ns | 349 ns | **2.1×** |
| `js/array-index` — `a[i % 1000]` | 215 ns | 265 ns | 328 ns | 1.5× |
| `js/try-catch` — throw and catch per iteration | 216 ns | 252 ns | 336 ns | 1.6× |
| `js/loop-arithmetic` — `t += i * 3 - 1` | 121 ns | 158 ns | 181 ns | 1.5× |
| `js/string-build` — `s += 'x'` | 284 ns | 332 ns | 377 ns | 1.3× |

Class bodies were the last construct handed back to the tree-walker, so until
they were compiled a page written in classes ran its method bodies on the old
engine however the caller got there — the machine's number for `js/class-methods`
was 555 ns against the tree-walker's 579 ns, which is to say it was not doing
anything.

| Workload | Machine | Before class bodies | Tree-walker |
|---|---|---|---|
| `js/class-methods` — `c.step()` 100k times | 331 ns/call | 555 ns/call | 583 ns/call |
| `js/class-super` — `super.m(v)` through one level | 605 ns/call | 1098 ns/call | 1921 ns/call |

The three `js/name-*` rows are not workloads anybody writes; they exist to
isolate one number. They differ only in how many names a loop iteration reads,
so the marginal cost of a name operation falls out of the differences — and
`perf` is not available on every machine this gets run on, so the measurement is
built rather than sampled.

| | Machine, before slots | Machine, after |
|---|---|---|
| `js/name-0-reads` — `for (let i…) {}` | 83 ns/iteration | 62 ns |
| `js/name-4-reads-near` — plus 4 reads and 4 writes | 207 ns | 113 ns |
| **cost of one name operation** | **15.6 ns** | **4.3 ns** |
| cost of one extra scope crossed | 3.8 ns | 2.5 ns |

4.3 ns is the cost of *any* instruction here — `js/name-0-reads` runs twelve of
them in 62 ns, or 5.2 ns each. So the premium a name used to carry is gone
rather than reduced: a slot read is now an ordinary instruction, and what is
left is dispatch and copying a 40-byte `Value`.

## How slot resolution works

Bindings live in a vector on each `Environment`, and the names index into it. Compiled code resolved
its names while compiling and indexes straight in; the tree-walker, every builtin and every class
body still ask by name and pay a hash to reach the same slot. Values live in the vector alone, so
the two paths cannot disagree about what a binding holds.

Two things make the index knowable at compile time:

- **Slots are reserved before a block runs, not assigned as its declarations execute.** Control flow
  can skip a declaration — `switch (n) { case 1: let a; case 2: let b }` entered at the second case
  never runs the first — and `b` still has to be where the compiler put it.
- **A reserved slot is not a binding.** It has a third state between "absent" and "undefined", so
  reading one before its own `let` stays a ReferenceError. That is also what makes the machine give
  the language's answer where the tree-walker cannot: inside a block, a name means that block's
  binding from the first line, including the lines above its declaration.

The compiler's scope model has to move in lockstep with the `PushScope` instructions it emits, since
"walk two scopes out" is only true if the two agree about how many there are. The one place they
deliberately differ is a `finally` block, which is emitted at each exit path and runs after the
blocks between it and the jump have been popped — so `RunFinalizers` truncates the model for the
duration.

## What the shape of the table said

**Calls got much faster and loops barely did.** That was the finding after the machine landed, and
it was not the expected one. A call used to be C++ recursion through `Evaluate` with a `Result` returned by value at
every level; it is a frame pushed onto a vector now. A loop iteration was already cheap in the
tree-walker, and compiling it did not make the *work inside* it cheaper — because the work inside it
is name lookup and property lookup, and neither of those changed.

**The benchmark found two mallocs on the hot path that had nothing to do with either engine.**
Reading a table with calls at 3× and loops at 1.15× is what prompted looking, and both were in
code the tree-walker had used unchanged since M8 started:

- `Environment::Lookup(std::string_view)` built a `std::string` to hash — once per scope in the
  chain, on every read of every variable. Fixed with heterogeneous lookup (`NameHash`/`NameEqual`
  in `Heap.h`).
- A named property access passed the name to `GetProperty`, which takes a `PropertyKey`, so every
  `o.x` copied the name into a fresh key. The name is known at compile time; `CompiledFunction::keys`
  now holds it already built.

Both helped the tree-walker too. So did moving bindings into a vector: the map holds a four-byte
index now instead of a forty-byte binding, which makes it dense enough to matter — the tree-walker's
`js/fib` went from 1034 to 573 ns/call for a change made entirely for the machine's benefit. That is
worth stating plainly, because it means the ratios in the table understate what the machine did and
overstate nothing.

## Where the time still goes

**A call still allocates an `Environment`.** `PushFrame` allocates one per call and fills four
prologue slots. Most calls do not need one at all: only a function some closure captures has to have
its scope outlive the call, and the compiler already knows which those are, because it knows where
every `Closure` op is. That is the next thing worth measuring — `js/fib` and `js/method-calls` are
the two rows slot resolution did not move, and this is why.

**A class is still built by the tree-walking `EvaluateClass`.** Its method bodies are compiled and
that is where the time is, but the builder itself — computed keys, static field initializers, and
the per-instance field initializers `InitializeFields` runs — still walks the tree. Those run once
per class or once per instance rather than once per call, so they are the right things to have left.

**Dispatch and `Value` are the floor.** At 4-5 ns per instruction with a 40-byte `Value` carrying a
`shared_ptr`, a good share of every opcode is copying values around. Narrowing that is NaN-boxing or
a computed-goto dispatch loop, and both are large enough to want their own measurement first.

**`arguments` is built only where it is named.** The compiler scans the body for the identifier and
sets `needs_arguments`, so the array is allocated per call only where something can observe it. The
tree-walker builds one on every call, which is part of why `js/fib` is 4× rather than 2×.

## What is not measured here

Collection. The machine collects at every loop back edge and every call, and the tree-walker cannot
collect during evaluation at all — so a benchmark comparing them on a program that allocates is
comparing two different behaviours, not two speeds. The behavioural difference is asserted in
`JsInterpreterTests` instead: a script that recurses while allocating used to exhaust the heap and
now runs to its step budget.
