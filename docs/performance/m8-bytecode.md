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

Per unit of work, lower is better. Ratio is tree-walker ÷ machine.

| Workload | Machine | Tree-walker | Ratio |
|---|---|---|---|
| `js/closures` — an arrow made and called per iteration | 611 ns | 2921 ns | **4.8×** |
| `js/fib` — `fib(24)`, 150k calls | 255 ns/call | 1034 ns/call | **4.1×** |
| `js/method-calls` — `o.step()` 100k times | 330 ns/call | 830 ns/call | **2.5×** |
| `js/property-reads` — `o.a + o.b + o.c` | 239 ns | 344 ns | 1.4× |
| `js/try-catch` — throw and catch per iteration | 252 ns | 324 ns | 1.3× |
| `js/array-index` — `a[i % 1000]` | 265 ns | 314 ns | 1.2× |
| `js/loop-arithmetic` — `t += i * 3 - 1` | 158 ns | 184 ns | 1.2× |
| `js/string-build` — `s += 'x'` | 332 ns | 361 ns | 1.1× |

## What the shape of that table says

**Calls got much faster and loops barely did.** That is the whole finding, and it was not the
expected one. A call used to be C++ recursion through `Evaluate` with a `Result` returned by value at
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

Both help the tree-walker too, which is why the ratios above moved less than the absolute times did.

## Where the time still goes

**Bindings are still names in a hash map.** `LoadName` walks the scope chain doing a hash lookup per
level. That is the single largest remaining cost in a loop, and it is the deliberate first-cut
decision recorded at the top of `Bytecode.h`: keeping `Environment` meant closures, `this`,
`arguments` and every builtin worked unchanged, so the change was about the stack becoming data and
nothing else. Resolving a name to a (depth, slot) pair at compile time is the next step and is worth
its own measurement.

**A call still allocates an `Environment`.** `PushFrame` allocates one per call and declares `this`
and `__function__` into it. With slot resolution most calls would need no environment at all — only
the ones a closure captures — which is the same change viewed from the other end.

**`arguments` is built only where it is named.** The compiler scans the body for the identifier and
sets `needs_arguments`, so the array is allocated per call only where something can observe it. The
tree-walker builds one on every call, which is part of why `js/fib` is 4× rather than 2×.

## What is not measured here

Collection. The machine collects at every loop back edge and every call, and the tree-walker cannot
collect during evaluation at all — so a benchmark comparing them on a program that allocates is
comparing two different behaviours, not two speeds. The behavioural difference is asserted in
`JsInterpreterTests` instead: a script that recurses while allocating used to exhaust the heap and
now runs to its step budget.
