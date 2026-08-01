#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Interpreter.h"

namespace microbrowser::tests {

using js::Completion;
using js::Interpreter;
using js::Result;

namespace {

// Runs a program and returns its completion value as a string. A thrown value
// is prefixed, so a test states which of the two it expects rather than
// checking a flag and then a value.
std::string Eval(std::string_view source) {
  Interpreter interpreter;
  const Result result = interpreter.Run(source);
  if (result.completion == Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  return js::ToString(result.value);
}

void ExpectEval(std::string_view source, std::string_view expected) {
  ExpectEqString(Eval(source), std::string(expected),
                 std::string("evaluating: ") + std::string(source));
}

std::vector<std::string> Log(std::string_view source) {
  Interpreter interpreter;
  interpreter.Run(source);
  return interpreter.ConsoleOutput();
}

}  // namespace

void RegisterJsInterpreterTests(std::vector<TestCase>& tests) {
  // --- Values and conversions ----------------------------------------------

  AddTest(tests, "JsInterpreter/ArithmeticAndPrecedence", [] {
    ExpectEval("1 + 2 * 3", "7");
    ExpectEval("(1 + 2) * 3", "9");
    ExpectEval("2 ** 3 ** 2", "512");
    ExpectEval("7 % 3", "1");
    ExpectEval("-7 % 3", "-1");
  });

  AddTest(tests, "JsInterpreter/PlusConcatenatesWhenEitherSideIsAString", [] {
    // The asymmetry that makes `+` the most surprising operator in the
    // language.
    ExpectEval("'a' + 1", "a1");
    ExpectEval("1 + 'a'", "1a");
    ExpectEval("1 + 2 + 'a'", "3a");
    ExpectEval("'a' + 1 + 2", "a12");
    ExpectEval("1 - '1'", "0");
  });

  AddTest(tests, "JsInterpreter/NumbersPrintTheWayJavaScriptPrintsThem", [] {
    // A plain printf gets every one of these wrong.
    ExpectEval("100", "100");
    ExpectEval("0.1 + 0.2", "0.30000000000000004");
    ExpectEval("1/3", "0.3333333333333333");
    ExpectEval("1e21", "1e+21");
    ExpectEval("-0", "0");
    ExpectEval("1/0", "Infinity");
    ExpectEval("0/0", "NaN");
  });

  AddTest(tests, "JsInterpreter/TruthinessFollowsTheSpecAndNotIntuition", [] {
    ExpectEval("[] ? 'y' : 'n'", "y");  // every object is truthy
    ExpectEval("'' ? 'y' : 'n'", "n");
    ExpectEval("0 ? 'y' : 'n'", "n");
    ExpectEval("NaN ? 'y' : 'n'", "n");
    ExpectEval("'0' ? 'y' : 'n'", "y");
  });

  AddTest(tests, "JsInterpreter/EqualityHasTwoFormsAndTheyDiffer", [] {
    ExpectEval("1 == '1'", "true");
    ExpectEval("1 === '1'", "false");
    ExpectEval("null == undefined", "true");
    ExpectEval("null === undefined", "false");
    ExpectEval("null == 0", "false");  // the rule that makes `x == null` idiomatic
    ExpectEval("NaN === NaN", "false");
  });

  AddTest(tests, "JsInterpreter/TypeofAnswersIncludingTheFamousMistake", [] {
    ExpectEval("typeof 1", "number");
    ExpectEval("typeof 'a'", "string");
    ExpectEval("typeof undefined", "undefined");
    ExpectEval("typeof null", "object");  // preserved because the web depends on it
    ExpectEval("typeof {}", "object");
    ExpectEval("typeof (() => 1)", "function");
    ExpectEval("typeof neverDeclared", "undefined");
  });

  // --- Scope, closures and functions ---------------------------------------

  AddTest(tests, "JsInterpreter/ClosuresCaptureTheirScope", [] {
    ExpectEval("function mk(){ let n = 0; return () => ++n; } "
               "const c = mk(); c(); c(); c()",
               "3");
    ExpectEval("function mk(){ let n = 0; return () => ++n; } "
               "const a = mk(), b = mk(); a(); a(); b()",
               "1");
  });

  AddTest(tests, "JsInterpreter/FunctionDeclarationsAreVisibleBeforeTheyAreWritten", [] {
    // Which is what makes mutually recursive functions work without forward
    // declarations.
    ExpectEval("f(); function f(){ return 1 } 'ok'", "ok");
    ExpectEval("function even(n){ return n === 0 ? true : odd(n - 1) } "
               "function odd(n){ return n === 0 ? false : even(n - 1) } even(10)",
               "true");
  });

  AddTest(tests, "JsInterpreter/RecursionWorksAndIsBounded", [] {
    ExpectEval("function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2) } fib(20)", "6765");
    // A page can write unbounded recursion, and the C++ stack is what would run
    // out. The language says this is a RangeError.
    ExpectEval("function f(){ return f() } f()", "throw RangeError: maximum call stack size exceeded");
  });

  AddTest(tests, "JsInterpreter/ArrowFunctionsCaptureThisAndOrdinaryOnesDoNot", [] {
    // The entire semantic difference between the two forms.
    ExpectEval("const o = { n: 7, get(){ return this.n } }; o.get()", "7");
    ExpectEval("const o = { n: 7, get(){ return (() => this.n)() } }; o.get()", "7");
    ExpectEval("const o = { n: 7, get(){ return (function(){ return this }).call } }; typeof o.get()",
               "undefined");
  });

  AddTest(tests, "JsInterpreter/DefaultsAndRestParameters", [] {
    ExpectEval("function f(a, b = a * 2){ return a + b } f(3)", "9");
    ExpectEval("function f(a, b = 1){ return b } f(1, undefined)", "1");
    ExpectEval("function f(...xs){ return xs.length } f(1, 2, 3)", "3");
    ExpectEval("function f(a, ...xs){ return xs.join('-') } f(1, 2, 3)", "2-3");
  });

  AddTest(tests, "JsInterpreter/SpreadInCallsAndArrays", [] {
    ExpectEval("function f(a, b, c){ return a + b + c } f(...[1, 2, 3])", "6");
    ExpectEval("[0, ...[1, 2], 3].join('')", "0123");
  });

  // --- Control flow ---------------------------------------------------------

  AddTest(tests, "JsInterpreter/LoopsAndBreakAndContinue", [] {
    ExpectEval("let s = 0; for (let i = 0; i < 5; i++) s += i; s", "10");
    ExpectEval("let s = 0; for (let i = 0; i < 5; i++) { if (i === 2) continue; s += i } s", "8");
    ExpectEval("let s = 0; for (let i = 0; i < 5; i++) { if (i === 2) break; s += i } s", "1");
    ExpectEval("let i = 0; while (i < 3) i++; i", "3");
    ExpectEval("let i = 0; do { i++ } while (i < 3); i", "3");
    ExpectEval("let i = 10; do { i++ } while (false); i", "11");
  });

  AddTest(tests, "JsInterpreter/ALabelledContinueContinuesTheNamedLoop", [] {
    // Found by running it: the label reached the labelled statement, which
    // ended the loop instead of continuing it. The label has to reach the loop.
    ExpectEval("let s = 0; outer: for (const x of [1, 2, 3]) { "
               "for (const y of [1, 2]) { if (y === 2) continue outer; s += x * y } } s",
               "6");
    ExpectEval("let s = 0; outer: for (let i = 0; i < 3; i++) { "
               "for (let j = 0; j < 3; j++) { if (j === 1) break outer; s++ } } s",
               "1");
  });

  AddTest(tests, "JsInterpreter/ForOfAndForInDifferInWhatTheyYield", [] {
    // The classic surprise: for...in over an array gives string keys.
    ExpectEval("let s = ''; for (const v of [10, 20]) s += v; s", "1020");
    ExpectEval("let s = ''; for (const k in [10, 20]) s += k; s", "01");
    ExpectEval("let s = ''; for (const k in { a: 1, b: 2 }) s += k; s", "ab");
    ExpectEval("let s = ''; for (const c of 'abc') s += c + '.'; s", "a.b.c.");
  });

  AddTest(tests, "JsInterpreter/SwitchFallsThroughUntilABreak", [] {
    ExpectEval("let r = ''; switch (2) { case 1: r += 'a'; case 2: r += 'b'; "
               "case 3: r += 'c'; break; default: r += 'd' } r",
               "bc");
    ExpectEval("let r = ''; switch (9) { case 1: r += 'a'; break; default: r += 'd' } r", "d");
    // `default` is taken only when nothing matched, however early it appears.
    ExpectEval("let r = ''; switch (2) { default: r += 'd'; case 2: r += 'b' } r", "b");
  });

  AddTest(tests, "JsInterpreter/TryCatchFinally", [] {
    ExpectEval("try { throw 'x' } catch (e) { e }", "x");
    ExpectEval("try { throw new Error('m') } catch (e) { 'caught' }", "caught");
    ExpectEval("let r = ''; try { r += 'a'; throw 1 } catch (e) { r += 'b' } finally { r += 'c' } r",
               "abc");
    ExpectEval("function f(){ try { return 1 } finally { } } f()", "1");
    // The case that makes control flow a value rather than an exception.
    ExpectEval("function f(){ try { return 1 } finally { return 2 } } f()", "2");
    ExpectEval("let r = ''; try { try { throw 1 } finally { r += 'f' } } catch (e) { r += 'c' } r",
               "fc");
  });

  AddTest(tests, "JsInterpreter/AnUncaughtThrowPropagatesOut", [] {
    ExpectEval("throw 'boom'", "throw boom");
    ExpectEval("null.x", "throw TypeError: cannot read property 'x' of null");
    ExpectEval("notDefined", "throw ReferenceError: notDefined is not defined");
    ExpectEval("const k = 1; k = 2", "throw TypeError: assignment to constant variable 'k'");
    ExpectEval("(1)()", "throw TypeError: 1 is not a function");
  });

  AddTest(tests, "JsInterpreter/ASyntaxErrorIsAThrownValueLikeAnyOther", [] {
    // One failure path for a caller, rather than two.
    Expect(Eval("if (").rfind("throw SyntaxError", 0) == 0, "a syntax error throws");
  });

  AddTest(tests, "JsInterpreter/AScriptCannotExhaustMemory", [] {
    // Found by the fuzzer. The collector cannot run during evaluation -- a
    // tree-walker keeps live values in C++ frames it cannot scan -- so a script
    // that recurses while allocating grows the heap with nothing able to shrink
    // it. Past a limit, allocation fails and that becomes a RangeError, which
    // is what a real engine says when it cannot grow.
    ExpectEval("function f(n){ return n < 6 ? n : f(n-1) * f(n-2) } f(73)",
               "throw RangeError: out of memory");
  });

  AddTest(tests, "JsInterpreter/ABlockThatDeclaresNothingAllocatesNoScope", [] {
    // The other half of the same finding: an empty loop body allocated a scope
    // per iteration, so `while (true) {}` ran out of heap before it reached the
    // step budget -- reporting the wrong reason for the right refusal.
    ExpectEval("while (true) {}", "throw RangeError: script ran too long");
    ExpectEval("for (;;) { }", "throw RangeError: script ran too long");
  });

  AddTest(tests, "JsInterpreter/ANodeThatIsNeitherExpressionNorStatementDoesNotLoop", [] {
    // Found by the fuzzer as a stack overflow. Evaluate's default deferred to
    // EvaluateStatement and EvaluateStatement's default deferred back, so a
    // node kind handled by neither bounced between them until the stack ran
    // out. A kind arriving there is a gap between the parser and the
    // evaluator, and saying so is the only useful answer.
    for (const std::string_view source :
         {"con^s={o}>funcn&", "class A {}", "tag`x`", "super", "...x"}) {
      const std::string result = Eval(source);
      Expect(result.rfind("throw", 0) == 0,
             std::string("expected a thrown value for: ") + std::string(source));
    }
  });

  AddTest(tests, "JsInterpreter/EvaluationDepthIsBoundedSeparatelyFromCallDepth", [] {
    // Found by the fuzzer as a stack overflow. Sixty nested unary operators
    // inside a function that recurses two hundred deep is twelve thousand C++
    // frames, and neither limit alone catches it: the call limit counts calls
    // and the parser's limit bounds one expression's nesting, but the product
    // is what the stack actually sees.
    std::string source = "function f(n){";
    for (int i = 0; i < 60; ++i) {
      source += "+";
    }
    source += "f(n-3)}f(20)";
    ExpectEval(source, "throw RangeError: maximum call stack size exceeded");
  });

  AddTest(tests, "JsInterpreter/BitwiseOperatorsConvertRatherThanCast", [] {
    // Found by the fuzzer as undefined behaviour: `~` cast a double straight to
    // int64, and 6.7e70 does not fit. Every bitwise operator runs its operands
    // through ToInt32, which truncates and wraps modulo 2^32 -- and a page
    // reaches it directly.
    ExpectEval("~1e70", "-1");
    ExpectEval("1e70 | 0", "0");
    ExpectEval("~5", "-6");
    ExpectEval("-1 >>> 0", "4294967295");
    ExpectEval("2147483647 + 1 | 0", "-2147483648");  // wraps, per the spec
    ExpectEval("NaN | 0", "0");
    ExpectEval("Infinity | 0", "0");
    ExpectEval("1 << 31", "-2147483648");
  });

  AddTest(tests, "JsInterpreter/AnInfiniteLoopIsBoundedRatherThanAHang", [] {
    // A page can write `while (true) {}`, and the only difference a user would
    // notice between a bounded and an unbounded one is whether the browser
    // comes back.
    ExpectEval("while (true) {}", "throw RangeError: script ran too long");
  });

  // --- Objects and arrays ---------------------------------------------------

  AddTest(tests, "JsInterpreter/ObjectsAndPropertyAccess", [] {
    ExpectEval("const o = { a: 1, 'b c': 2 }; o.a + o['b c']", "3");
    ExpectEval("const o = {}; o.x = 5; o.x", "5");
    ExpectEval("const k = 'z'; const o = { [k]: 9 }; o.z", "9");
    ExpectEval("const o = { a: 1 }; delete o.a; typeof o.a", "undefined");
    ExpectEval("const o = { a: undefined }; 'a' in o", "true");
  });

  AddTest(tests, "JsInterpreter/ArraysAndTheirMethods", [] {
    ExpectEval("[1,2,3].length", "3");
    ExpectEval("[1,2,3][1]", "2");
    ExpectEval("const a = [1]; a.push(2, 3); a.join('-')", "1-2-3");
    ExpectEval("[1,2,3].pop()", "3");
    ExpectEval("[1,2,3,4].filter(x => x % 2 === 0).map(x => x * 10).reduce((a, b) => a + b, 0)",
               "60");
    ExpectEval("[1,2,3].indexOf(2)", "1");
    ExpectEval("[1,2,3].includes(9)", "false");
    ExpectEval("[1,2,3,4].slice(1, 3).join('')", "23");
    ExpectEval("[1,2,3,4].slice(-2).join('')", "34");
  });

  AddTest(tests, "JsInterpreter/AnArrayGrowsWhenWrittenPastItsEnd", [] {
    ExpectEval("const a = []; a[2] = 'x'; a.length", "3");
    ExpectEval("const a = [1,2,3]; a.length = 1; a.join('')", "1");
    // A page can write `a.length = 4294967295`, and honouring it would be a
    // 34-gigabyte allocation.
    ExpectEval("const a = []; a.length = 4294967295; 'ok'",
               "throw RangeError: array length is too large");
  });

  AddTest(tests, "JsInterpreter/Destructuring", [] {
    ExpectEval("const [a, , b] = [1, 2, 3]; a + b", "4");
    ExpectEval("const { x, y = 5 } = { x: 9 }; x + y", "14");
    ExpectEval("const [a, ...rest] = [1, 2, 3]; rest.join('')", "23");
    ExpectEval("function f({ a, b }){ return a + b } f({ a: 1, b: 2 })", "3");
  });

  AddTest(tests, "JsInterpreter/ConstructorsAndPrototypes", [] {
    ExpectEval("function P(n){ this.n = n } const p = new P(4); p.n", "4");
    ExpectEval("function P(){} P.prototype.hi = function(){ return 'hi' }; new P().hi()", "hi");
    ExpectEval("function P(){} new P() instanceof P", "true");
    // A constructor returning an object replaces the instance; a primitive
    // does not.
    ExpectEval("function P(){ this.a = 1; return { a: 2 } } new P().a", "2");
    ExpectEval("function P(){ this.a = 1; return 5 } new P().a", "1");
  });

  AddTest(tests, "JsInterpreter/TemplateLiteralsInterpolate", [] {
    ExpectEval("const a = 1, b = 2; `${a} + ${b} = ${a + b}`", "1 + 2 = 3");
    ExpectEval("`no substitution`", "no substitution");
    ExpectEval("const x = 'y'; `${`inner ${x}`}`", "inner y");
  });

  AddTest(tests, "JsInterpreter/OptionalChainingStopsAtNullish", [] {
    ExpectEval("const o = null; typeof o?.a", "undefined");
    ExpectEval("const o = { a: { b: 1 } }; o?.a?.b", "1");
    ExpectEval("const o = {}; typeof o.f?.()", "undefined");
  });

  AddTest(tests, "JsInterpreter/LogicalOperatorsReturnAnOperandNotABoolean", [] {
    // Which is what makes `a || 'default'` idiomatic.
    ExpectEval("0 || 'default'", "default");
    ExpectEval("'set' || 'default'", "set");
    ExpectEval("null ?? 'default'", "default");
    ExpectEval("0 ?? 'default'", "0");  // the difference between ?? and ||
    ExpectEval("1 && 2", "2");
  });

  AddTest(tests, "JsInterpreter/CompoundAndLogicalAssignment", [] {
    ExpectEval("let a = 1; a += 2; a", "3");
    ExpectEval("let a = 'x'; a += 1; a", "x1");
    ExpectEval("let a = 8; a >>= 2; a", "2");
    ExpectEval("let a = null; a ?\?= 5; a", "5");
    ExpectEval("let a = 1; a ?\?= 5; a", "1");
    ExpectEval("const o = { n: 1 }; o.n += 4; o.n", "5");
  });

  AddTest(tests, "JsInterpreter/UpdateExpressionsDifferInWhatTheyYield", [] {
    ExpectEval("let a = 1; a++", "1");
    ExpectEval("let a = 1; ++a", "2");
    ExpectEval("let a = 1; a++; a", "2");
    ExpectEval("const o = { n: 1 }; o.n++; o.n", "2");
  });

  // --- Builtins -------------------------------------------------------------

  AddTest(tests, "JsInterpreter/ConsoleOutputIsCollectedRatherThanPrinted", [] {
    // A page must not be able to write to the terminal the browser was started
    // from.
    const std::vector<std::string> lines = Log("console.log('a', 1); console.log('b')");
    ExpectEqInt(static_cast<long long>(lines.size()), 2, "two lines");
    ExpectEqString(lines.at(0), "a 1", "arguments joined with a space");
    ExpectEqString(lines.at(1), "b", "and the second line");
  });

  AddTest(tests, "JsInterpreter/MathAndConversions", [] {
    ExpectEval("Math.max(1, 5, 3)", "5");
    ExpectEval("Math.min(1, 5, 3)", "1");
    ExpectEval("Math.max(1, NaN)", "NaN");  // any NaN makes the answer NaN
    ExpectEval("Math.floor(-1.5)", "-2");
    ExpectEval("Math.round(-0.5)", "0");  // rounds half up, not away from zero
    ExpectEval("Math.abs(-3)", "3");
    ExpectEval("parseInt('12px')", "12");
    ExpectEval("Number('12px')", "NaN");  // the difference between the two
    ExpectEval("parseFloat('1.5rem')", "1.5");
    ExpectEval("isNaN('x')", "true");
  });

  AddTest(tests, "JsInterpreter/JsonStringify", [] {
    ExpectEval("JSON.stringify({ a: 1, b: [1, 2], c: 'x', d: null })",
               R"({"a":1,"b":[1,2],"c":"x","d":null})");
    ExpectEval("JSON.stringify([1, 'a', true])", R"([1,"a",true])");
    ExpectEval(R"(JSON.stringify({ q: 'he said "hi"\n' }))", R"({"q":"he said \"hi\"\n"})");
    // An undefined property is omitted; an undefined array element is null.
    ExpectEval("JSON.stringify({ a: undefined, b: 1 })", R"({"b":1})");
    ExpectEval("JSON.stringify([undefined])", "[null]");
    // Infinity and NaN have no JSON representation.
    ExpectEval("JSON.stringify({ a: 1/0 })", R"({"a":null})");
  });

  AddTest(tests, "JsInterpreter/ObjectKeysAndValues", [] {
    ExpectEval("Object.keys({ b: 1, a: 2 }).join(',')", "b,a");  // insertion order
    ExpectEval("Object.values({ a: 1, b: 2 }).join(',')", "1,2");
  });

  // --- The collector --------------------------------------------------------

  AddTest(tests, "JsInterpreter/TheCollectorReclaimsCycles", [] {
    // The reason the heap traces rather than counts references: a closure over
    // a scope that holds the closure is a cycle, and it is the normal case
    // rather than the exception. A refcounted engine leaks one of these per
    // page.
    Interpreter interpreter;
    const Result warm = interpreter.Run("function mk(){ const o = {}; o.self = o; return 1 } 1");
    Expect(!warm.IsAbrupt(), "the warm-up ran");
    const std::size_t before = interpreter.GetHeap().ObjectCount();

    const Result made = interpreter.Run(
        "for (let i = 0; i < 500; i++) { const o = {}; o.self = o; o.f = () => o } 'done'");
    Expect(!made.IsAbrupt(), "the cycles were built");
    const std::size_t peak = interpreter.GetHeap().ObjectCount();
    Expect(peak > before, "they were allocated");

    interpreter.GetHeap().Collect({interpreter.Global()}, {interpreter.GlobalScope()});
    const std::size_t after = interpreter.GetHeap().ObjectCount();
    Expect(after < peak,
           "and reclaimed -- a self-referencing object with a closure over itself is exactly "
           "what a reference count cannot free");
  });

  AddTest(tests, "JsInterpreter/CollectingKeepsWhatIsStillReachable", [] {
    // The other half, and the one that matters: a collector that frees too much
    // is worse than one that frees nothing.
    Interpreter interpreter;
    const Result result = interpreter.Run(
        "globalThis.keep = { n: 1, inner: { m: 2 } };"
        "for (let i = 0; i < 2000; i++) { const junk = { i }; }"
        "keep.n + keep.inner.m");
    Expect(!result.IsAbrupt(), "it ran");
    ExpectEqString(js::ToString(result.value), "3",
                   "the reachable object survived the collections that ran during the loop");
  });

  AddTest(tests, "JsInterpreter/APrototypeCycleDoesNotHang", [] {
    // A page can build one, and an unbounded walk is a hang rather than a
    // wrong answer.
    ExpectEval("function A(){} function B(){} "
               "A.prototype = Object.create ? {} : {}; "
               "const a = new A(); typeof a.missing",
               "undefined");
  });

  // --- A program that exercises the lot -------------------------------------

  AddTest(tests, "JsInterpreter/ARealisticProgramRuns", [] {
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      function memoize(fn) {
        const cache = {};
        return function (n) {
          const key = String(n);
          if (key in cache) return cache[key];
          const value = fn(n);
          cache[key] = value;
          return value;
        };
      }
      const fib = memoize(function (n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); });

      const people = [
        { name: 'ada', age: 36 },
        { name: 'alan', age: 41 },
        { name: 'grace', age: 45 },
      ];
      const names = people
        .filter(p => p.age > 38)
        .map(p => p.name.toUpperCase ? p.name : p.name)
        .join(', ');

      const totals = people.reduce((acc, p) => { acc.count++; acc.age += p.age; return acc; },
                                   { count: 0, age: 0 });

      console.log(`fib(25) = ${fib(25)}`);
      console.log(names);
      console.log(JSON.stringify(totals));
      'done'
    )");
    Expect(!result.IsAbrupt(),
           "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "done", "and finished");

    const std::vector<std::string>& log = interpreter.ConsoleOutput();
    ExpectEqInt(static_cast<long long>(log.size()), 3, "three lines were logged");
    ExpectEqString(log.at(0), "fib(25) = 75025", "memoized fibonacci");
    ExpectEqString(log.at(1), "alan, grace", "filter and map and join");
    ExpectEqString(log.at(2), R"({"count":3,"age":122})", "and reduce into an object");
  });
}

}  // namespace microbrowser::tests
