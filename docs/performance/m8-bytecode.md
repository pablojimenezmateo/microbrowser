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

## Read one benchmark at a time

**A number from a run of the whole table is not comparable with a number from
another run of the whole table**, and the tree-walker column is the half that
shows it. The benchmarks share a process and therefore share an allocator, and a
workload that allocates heavily leaves free lists warm for whatever runs next.
Two measurements of code that did not change between them:

- `js/fib [tree-walker]` reports 561 ns/call when the machine's `js/fib` ran
  before it in the same process, and 1760 ns/call when run on its own. The
  machine's `fib` used to allocate 150k environments and now allocates none, so
  it stopped warming the run after it.
- `js/class-super [tree-walker]` reports 1460 ns/call against one build of the
  harness and 4610 against another. Linked into a bare program that runs the
  same source through `RunProgram` and nothing else, the two builds are 0.67 and
  0.68 seconds — the same. What moved was the thirteen compiled chunks the
  harness builds at registration and never runs.

The harness warms each benchmark against itself (one untimed run, a pilot, then
best of five rounds). What it cannot warm is the process it was handed. So every
number below was taken one benchmark per process, best of five, on the perf
preset:

```
./build/microbrowser-perf/microbrowser/microbrowser_bench "js/fib [machine]"
```

Compare a machine number against a machine number. The ratio column is the
weakest thing in the table.

## The numbers

Per unit of work, lower is better. Ratio is tree-walker ÷ machine. Taken after
frames without scopes; the two middle columns are the machine's own numbers
before each of the two changes that moved them, so what each bought reads off
one row. The "before slots" column was taken by the older whole-table protocol
and is comparable in shape rather than digit for digit.

| Workload | Machine | Before frames | Before slots | Tree-walker | Ratio |
|---|---|---|---|---|---|
| `js/fib` — `fib(24)`, 150k calls | 116 ns/call | 252 ns/call | 255 ns/call | 1760 ns/call | **15×** |
| `js/class-super` — `super.m(v)` through one level | 279 ns/call | 599 ns/call | — | 4610 ns/call | **17×** |
| `js/class-methods` — `c.step()` 100k times | 203 ns/call | 336 ns/call | — | 1205 ns/call | **6×** |
| `js/method-calls` — `o.step()` 100k times | 202 ns/call | 313 ns/call | 330 ns/call | 722 ns/call | **3.6×** |
| `js/try-catch` — throw and catch per iteration | 115 ns | 193 ns | 252 ns | 291 ns | **2.5×** |
| `js/closures` — an arrow made and called per iteration | 460 ns | 612 ns | 611 ns | 3605 ns | **7.8×** |
| `js/array-index` — `a[i % 1000]` | 203 ns | 211 ns | 265 ns | 294 ns | 1.4× |
| `js/loop-arithmetic` — `t += i * 3 - 1` | 110 ns | 111 ns | 158 ns | 178 ns | 1.6× |
| `js/property-reads` — `o.a + o.b + o.c` | 157 ns | 157 ns | 239 ns | 339 ns | 2.2× |
| `js/string-build` — `s += 'x'` | 265 ns | 264 ns | 332 ns | 342 ns | 1.3× |

**Calls stopped allocating, and that is the whole of the "before frames"
column.** `js/fib` is calls and nothing else and it halved — the row slot
resolution did not move. `js/class-super` and `js/class-methods` halved for the
same reason. `js/try-catch` nearly halved because the block scope a `try` sits
in went with the call's own. The rows that are arithmetic in a loop did not
move at all, because they were never allocating.

`js/closures` is the row that says what is left: an arrow is a closure, so the
function that makes one still allocates a scope, and 612 → 460 ns is the arrow's
*own* call being flattened rather than the maker's.

Class bodies were the last construct handed back to the tree-walker, so until
they were compiled a page written in classes ran its method bodies on the old
engine however the caller got there — the machine's number for `js/class-methods`
was 555 ns against the tree-walker's 579 ns, which is to say it was not doing
anything. Both of those were taken by the whole-table protocol and are here for
the shape of the change rather than for their digits.

| Workload | Machine, then | Before class bodies | Tree-walker, then |
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

## How a frame without scopes works

A scope has to outlive its call only when something can still reach it afterwards, and the only
thing that can reach one is a closure made inside it. So a function that makes none cannot leak a
scope, and none of its scopes needs to be on the heap: its bindings go in a slice of a locals stack
the machine owns, and the call allocates nothing at all.

Three things make it work:

- **The question is syntactic, and is answered before anything is emitted.** "Can this leak a scope"
  is "is there a function node in the body" — a nested function, an arrow, a class, an object
  literal with a method. There is no other way to make a function here: `eval` and `Function(source)`
  do not exist, and a test says so. It has to be answered first because every name in the body
  resolves against the answer.
- **Every block in such a function is flattened into the one slice.** Slot indices are function-wide
  rather than per-block, so `hops = 0` means the frame however many blocks deep the reference is,
  and `hops = 1` means the scope the function was defined in. No `PushScope` is emitted at all.
- **A block still has to be undeclared again each time it is entered.** That is the one thing a
  fresh Environment gave for free. `ClearLocals` puts a block's run of slots back in their reserved
  state, so `x` before its own `let x` is a ReferenceError on the second time round a loop as much
  as on the first.

It follows that a flattened function never has a nested one, so a flattened scope is never something
an inner function resolves *through* — which is what keeps the hop counting to one rule rather than
two.

Two limits fall out. The slice is a fixed-capacity stack for the reason the value stack is —
an instruction holds a `Binding*` into it while it runs — so overflowing it is the RangeError a
page gets for recursing too far. And a flattened function has no name-based fallback: a name form
walks Environments and there is no Environment holding this, so a function with four thousand
block-scoped names abandons the compile and the tree-walker takes the program.

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

**A call that makes a closure still allocates.** `js/closures` is the row: an arrow is a function, so
the scope of whatever makes one has to be able to outlive the call, and it goes on the heap. The
present rule is all-or-nothing per function — one arrow anywhere in a body puts every block in that
body back on the heap — where what is actually required is that the scopes the closure can *reach*
survive. Narrowing it means asking which scopes a given `Closure` op is inside rather than whether
there is one, and it is worth doing only against a page that spends its time there.

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

## What suspending costs

`await` is not on the table above, because a benchmark of it would be measuring the microtask queue
rather than the machine. What is worth writing down is the shape of the cost, since it is the one
place the machine does something a call does not.

A suspension copies five slices out of the machine's stacks and back: the frame, its region of the
value stack, the block scopes it pushed, the `for...of` cursors it left open, and its bindings when
they are in the frame rather than on the heap. So an `await` costs one heap allocation for the
promise, one for the reaction, a queue entry, and a copy proportional to how much of the frame is
live at the point it waits — which for the shape a page writes (`const x = await f()` at statement
level, nothing half-built) is the four prologue slots and whatever the body has declared.

The copy is what a fixed-capacity register file would avoid, and it is the wrong thing to optimise
first: a page that awaits is a page that is waiting on the network, and the turn boundary either
side of it costs more than the copy does.

## What is not measured here

Collection. The machine collects at every loop back edge and every call, and the tree-walker cannot
collect during evaluation at all — so a benchmark comparing them on a program that allocates is
comparing two different behaviours, not two speeds. The behavioural difference is asserted in
`JsInterpreterTests` instead: a script that recurses while allocating used to exhaust the heap and
now runs to its step budget.
