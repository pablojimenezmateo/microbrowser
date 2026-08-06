#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BenchSupport.h"
#include "js/Bytecode.h"
#include "js/Interpreter.h"
#include "js/Parser.h"

// JavaScript, on both engines, on the same programs.
//
// The pair is the point. A bytecode machine is supposed to be faster than a
// tree-walker, and "supposed to" is exactly the kind of claim this repository
// does not accept without a number -- see docs/performance/m1-rasterizer.md,
// where the thing everybody knew was hot was not. Registering each workload
// twice makes the ratio readable off one table instead of inferred from two
// runs on two days.
//
// Both sides skip parsing and skip constructing an interpreter, because neither
// is what changed: the timed body is evaluation and nothing else. The
// interpreter is shared across iterations for the same reason, so what is
// measured is the second run of a program rather than the cost of installing
// the global object.

namespace microbrowser::bench {

namespace {

using microbrowser::js::CompiledFunction;
using microbrowser::js::Interpreter;

struct Workload {
  std::string_view name;
  // Units of work the program performs, so the table can say what one call,
  // one iteration or one property read costs rather than only what the whole
  // program costs.
  std::size_t units;
  std::string_view unit_name;
  std::string_view source;
};

// Each is wrapped in a function so that the work happens in a call rather than
// at the top level. That is where a page does its work, it is where the two
// engines differ most, and it keeps the top-level scope from growing a binding
// per iteration.
constexpr Workload kWorkloads[] = {
    {"js/fib", 150049, "call",
     "(function(){ function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2) } return fib(24) })()"},
    {"js/loop-arithmetic", 200000, "iteration",
     "(function(){ let t = 0; for (let i = 0; i < 200000; i++) { t += i * 3 - 1 } return t })()"},
    {"js/property-reads", 100000, "read",
     "(function(){ const o = { a: 1, b: 2, c: 3 }; let t = 0;"
     "for (let i = 0; i < 100000; i++) { t += o.a + o.b + o.c } return t })()"},
    {"js/array-index", 100000, "element",
     "(function(){ const a = []; for (let i = 0; i < 1000; i++) a.push(i); let t = 0;"
     "for (let i = 0; i < 100000; i++) { t += a[i % 1000] } return t })()"},
    {"js/method-calls", 100000, "call",
     "(function(){ const o = { n: 0, step(){ return ++this.n } };"
     "for (let i = 0; i < 100000; i++) o.step(); return o.n })()"},
    // A class method and a `super` chain, which until class bodies were
    // compiled ran entirely on the tree-walker however the caller got there.
    {"js/class-methods", 100000, "call",
     "(function(){ class Counter { constructor(){ this.n = 0 } step(){ return ++this.n } }"
     "const c = new Counter(); for (let i = 0; i < 100000; i++) c.step(); return c.n })()"},
    {"js/class-super", 50000, "call",
     "(function(){ class A { m(v){ return v + 1 } }"
     "class B extends A { m(v){ return super.m(v) + 1 } }"
     "const b = new B(); let t = 0; for (let i = 0; i < 50000; i++) t = b.m(t); return t })()"},
    {"js/closures", 50000, "closure",
     "(function(){ let t = 0; for (let i = 0; i < 50000; i++) { const f = () => i + 1; t += f() }"
     "return t })()"},
    {"js/string-build", 20000, "append",
     "(function(){ let s = ''; for (let i = 0; i < 20000; i++) { s += 'x' } return s.length })()"},
    // Three loops that differ only in how many names they read, so the
    // marginal cost of one name lookup falls out of the differences. That
    // number is what decides whether resolving names to slots is worth the
    // change it costs -- and `perf` is not available on every machine this
    // gets run on, so the measurement is built rather than sampled.
    {"js/name-0-reads", 200000, "iteration",
     "(function(){ for (let i = 0; i < 200000; i++) {} return 1 })()"},
    {"js/name-4-reads-near", 200000, "iteration",
     "(function(){ const x = 1; let t = 0;"
     "for (let i = 0; i < 200000; i++) { t = x; t = x; t = x; t = x } return t })()"},
    {"js/name-4-reads-far", 200000, "iteration",
     "const far = 1;"
     "(function(){ return (function(){ return (function(){ let t = 0;"
     "for (let i = 0; i < 200000; i++) { t = far; t = far; t = far; t = far } return t })() })() })()"},
    {"js/try-catch", 50000, "throw",
     "(function(){ let n = 0; for (let i = 0; i < 50000; i++) { try { throw i } catch (e) { n += 1 } }"
     "return n })()"},
};

// Everything one workload needs to stay alive between iterations: the tree,
// because a function object points into it; the chunk, for the same reason; and
// the interpreter that holds both.
struct Prepared {
  std::shared_ptr<Interpreter> interpreter;
  js::NodePtr program;
  std::unique_ptr<CompiledFunction> compiled;
};

std::shared_ptr<Prepared> Prepare(std::string_view source, bool compile) {
  auto prepared = std::make_shared<Prepared>();
  prepared->interpreter = std::make_shared<Interpreter>();
  js::ParseResult parsed = js::Parse(source);
  prepared->program = std::move(parsed.program);
  if (compile && prepared->program != nullptr) {
    prepared->compiled = js::Compile(*prepared->program, source.size());
  }
  return prepared;
}

}  // namespace

void RegisterJsBenchmarks(std::vector<Benchmark>& benchmarks) {
  for (const Workload& workload : kWorkloads) {
    const std::string_view source = workload.source;

    auto machine = Prepare(source, true);
    if (machine->compiled != nullptr) {
      AddBenchmark(benchmarks, std::string(workload.name) + " [machine]", workload.units,
                   workload.unit_name,
                   [machine] { machine->interpreter->RunCompiled(*machine->compiled); });
    } else {
      // Not silently absent. A workload that stops compiling would otherwise
      // vanish from the table and read as one the machine never had.
      AddBenchmark(benchmarks, std::string(workload.name) + " [machine: DID NOT COMPILE]", 0,
                   "call", [] {});
    }

    auto walker = Prepare(source, false);
    AddBenchmark(benchmarks, std::string(workload.name) + " [tree-walker]", workload.units,
                 workload.unit_name,
                 [walker] { walker->interpreter->RunProgram(*walker->program); });
  }
}

}  // namespace microbrowser::bench
