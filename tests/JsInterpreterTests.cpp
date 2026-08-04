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
    // An ordinary function called plainly gets no receiver, so the outer
    // `this` does not reach it -- which is the half of the difference the two
    // lines above do not show.
    ExpectEval("const o = { n: 7, get(){ return (function(){ return this })() } }; typeof o.get()",
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

  AddTest(tests, "JsInterpreter/AScriptThatRecursesWhileAllocatingIsCollectedThrough", [] {
    // Found by the fuzzer, and the clearest single measure of what the machine
    // bought. This used to be "throw RangeError: out of memory": the collector
    // could not run during evaluation, because a tree-walker keeps live values
    // in C++ frames it cannot scan, so a script that recursed while allocating
    // grew the heap with nothing able to shrink it.
    //
    // Every one of those values is on the machine's value stack now, and the
    // safepoint at each call and each loop back edge collects them. So the
    // script no longer runs out of heap -- it runs out of *time*, which is the
    // bound that was supposed to catch it, and reports the reason it actually
    // stopped.
    //
    // This is the one test in the suite that is expected to fail under
    // MICROBROWSER_JS_TREEWALK=1, and that is what makes it worth having: the
    // two engines agree on all nine hundred and seventy two others, and differ
    // here because here is where they are supposed to.
    ExpectEval("function f(n){ return n < 6 ? n : f(n-1) * f(n-2) } f(73)",
               "throw RangeError: script ran too long");
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
         {"con^s={o}>funcn&", "tag`x`", "super", "...x"}) {
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
    ExpectEval("const o = {}; delete o.missing", "true");
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

  AddTest(tests, "JsInterpreter/SparseArrayHolesAreNotElements", [] {
    ExpectEval("0 in [, 1]", "false");
    ExpectEval("1 in [, 1]", "true");
    ExpectEval("'length' in []", "true");
    ExpectEval("const a = [1,2]; delete a.length", "false");
    ExpectEval("const a = [1,2]; delete a.length; a.length", "2");
    ExpectEval("const a = [1,2]; delete a[0]; a.length + ':' + (0 in a) + ':' + a.join(',')",
               "2:false:,2");
    ExpectEval("Object.keys([, 'x']).join(',')", "1");
    ExpectEval("Object.values([, 'x']).join(',')", "x");
    ExpectEval("JSON.stringify([, 1])", "[null,1]");
    ExpectEval("[, undefined].indexOf(undefined)", "1");
    ExpectEval("[,].includes(undefined)", "true");
    ExpectEval("[1,,3].map(x => x * 2).join(',')", "2,,6");
    ExpectEval("[1,,3].filter(x => true).join(',')", "1,3");
    ExpectEval("[1,,3].reduce((a, b) => a + b)", "4");
    ExpectEval("[1].map(x => { throw 'map' })", "throw map");
    ExpectEval("[1].filter(x => { throw 'filter' })", "throw filter");
    ExpectEval("[1,2].reduce((a, b) => { throw 'reduce' })", "throw reduce");
    ExpectEval("let s = ''; for (const k in [, 'x']) s += k; s", "1");
    ExpectEval("let s = ''; for (const v of [, 'x']) s += String(v) + ','; s",
               "undefined,x,");
  });

  AddTest(tests, "JsInterpreter/AnArrayGrowsWhenWrittenPastItsEnd", [] {
    ExpectEval("const a = []; a[2] = 'x'; a.length", "3");
    ExpectEval("const a = [1,2,3]; a.length = 1; a.join('')", "1");
    ExpectEval("const a = []; a[4294967295] = 7; a.length + ':' + a[4294967295]", "0:7");
    // A page can write `a.length = 4294967295`, and honouring it would be a
    // 34-gigabyte allocation.
    ExpectEval("const a = []; a.length = 4294967295; 'ok'",
               "throw RangeError: array length is too large");
    ExpectEval("const a = [1,2,3]; a.length = 1.5; 'ok'",
               "throw RangeError: array length is too large");
    ExpectEval("const a = [1,2,3]; a.length = NaN; 'ok'",
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

  AddTest(tests, "JsInterpreter/AdjacentTemplateSubstitutionsAllRun", [] {
    // With no literal text between them. The parser and the interpreter used to
    // scan the raw text separately and resume one character apart after a
    // substitution, so the `$` of the next one fell into the gap and every
    // substitution but the first was dropped.
    ExpectEval("`${1}${2}`", "12");
    ExpectEval("`${1}${2}${3}`", "123");
    ExpectEval("`x${1}${2}y`", "x12y");
    ExpectEval("const x = 5; `${x}${x}${x}`", "555");
    ExpectEval("`${}` + '|'", "|");   // an empty substitution contributes nothing
    ExpectEval("`$ {1}`", "$ {1}");   // and `${` has to be adjacent to begin one
  });

  AddTest(tests, "JsInterpreter/TemplateEscapesMatchStringEscapes", [] {
    // The same escape decoder, because `\n` cannot mean a newline in one and
    // the letter n in the other. It did: the template path had its own
    // two-line version that pushed whatever followed the backslash.
    ExpectEval("`a\\nb`.charCodeAt(1)", "10");
    ExpectEval("'a\\nb'.charCodeAt(1)", "10");
    ExpectEval("`a\\tb`.charCodeAt(1)", "9");
    ExpectEval("`a\\u0041b`", "aAb");
    ExpectEval("`\\u{1F600}`.length", "4");  // four bytes of UTF-8
    ExpectEval("`\\``", "`");
    ExpectEval("`\\q`", "q");  // an unrecognised escape is the character itself
  });

  AddTest(tests, "JsInterpreter/ATemplateSubstitutionIsAnExpression", [] {
    // Not a statement. Parsed as one, a leading brace is a block rather than an
    // object literal, and this was a syntax error.
    ExpectEval("`${{ a: 1 }.a}`", "1");
    ExpectEval("`${{ a: 1, b: 2 }.b}`", "2");
    ExpectEval("`${(1, 2)}`", "2");  // the comma operator is still one expression
    ExpectEval("`${ [1, 2].join('-') }`", "1-2");
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
    ExpectEval("parseInt('0x10')", "16");
    ExpectEval("parseInt('-0x10')", "-16");
    ExpectEval("parseInt('10', 2)", "2");
    ExpectEval("parseInt('10', 1)", "NaN");
    ExpectEval("Number('12px')", "NaN");  // the difference between the two
    ExpectEval("Number('+1.5')", "1.5");
    ExpectEval("Number('Infinity')", "Infinity");
    ExpectEval("Number(' 12\\n')", "12");
    ExpectEval("parseFloat('1.5rem')", "1.5");
    ExpectEval("parseFloat(' +1.5rem')", "1.5");
    ExpectEval("parseFloat('1e+')", "1");
    ExpectEval("parseFloat('-Infinitypx')", "-Infinity");
    ExpectEval("parseFloat('px')", "NaN");
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
    ExpectEval("class A { get v(){ return 3 } } Object.values(A.prototype).includes(3)", "true");
  });

  AddTest(tests, "JsInterpreter/ObjectCreateSetsThePrototype", [] {
    ExpectEval("const proto = { a: 1 }; const o = Object.create(proto); o.a", "1");
    ExpectEval("const proto = { a: 1 }; const o = Object.create(proto); o.b = 2; "
               "Object.keys(o).join(',')",
               "b");
    ExpectEval("'a' in Object.create({ a: 1 })", "true");
    ExpectEval("const o = Object.create(null); o.x = 4; o.x", "4");
    ExpectEval("Object.create(5)",
               "throw TypeError: Object.create prototype must be an object or null");
  });

  // --- Classes --------------------------------------------------------------

  AddTest(tests, "JsInterpreter/ClassesAreFunctionsWithAPopulatedPrototype", [] {
    // Saying so in one place is what keeps `new Foo()` from needing to know
    // which of the two forms it was handed.
    ExpectEval("class A { constructor(n){ this.n = n } get(){ return this.n } } new A(5).get()",
               "5");
    ExpectEval("class A {} typeof A", "function");
    ExpectEval("class A { m(){} } typeof A.prototype.m", "function");
    ExpectEval("const C = class { v(){ return 42 } }; new C().v()", "42");
    ExpectEval("class A { static create(){ return new A() } } typeof A.create()", "object");
  });

  AddTest(tests, "JsInterpreter/FieldsInitializePerInstanceAndSeeThis", [] {
    ExpectEval("class A { n = 7 } new A().n", "7");
    ExpectEval("class A { a = 2; b = this.a * 3 } new A().b", "6");
    ExpectEval("class A { n = 1 } const x = new A(); x.n = 9; new A().n", "1");
    ExpectEval("class A { n } typeof new A().n", "undefined");
    ExpectEval("class A { #secret = 4; reveal(){ return this.#secret } } new A().reveal()", "4");
  });

  AddTest(tests, "JsInterpreter/StaticMembersLiveOnTheClass", [] {
    ExpectEval("class A { static k = 9 } A.k", "9");
    ExpectEval("class A { static k = 9 } typeof new A().k", "undefined");
    ExpectEval("class A { static m(){ return 's' } } A.m()", "s");
  });

  AddTest(tests, "JsInterpreter/AccessorsRunOnReadAndWrite", [] {
    ExpectEval("class A { #n = 4; get value(){ return this.#n } } new A().value", "4");
    ExpectEval("class A { set v(x){ this.stored = x * 2 } } const a = new A(); a.v = 5; a.stored",
               "10");
    // A getter with no setter: assigning is a silent no-op rather than an
    // overwrite that removes the accessor.
    ExpectEval("class A { get v(){ return 1 } } const a = new A(); a.v = 9; a.v", "1");
    // Two halves of the same property, declared separately.
    ExpectEval("class A { get v(){ return this._v } set v(x){ this._v = x + 1 } } "
               "const a = new A(); a.v = 1; a.v",
               "2");
    // An accessor is inherited, and runs with `this` bound to the receiver.
    ExpectEval("class A { get twice(){ return this.n * 2 } } class B extends A { n = 5 } "
               "new B().twice",
               "10");
    ExpectEval("class A { get value(){ return 1 } } 'value' in new A", "true");
    ExpectEval("class A { set value(v){ this.v = v } } 'value' in new A", "true");
  });

  AddTest(tests, "JsInterpreter/InheritanceAndSuper", [] {
    ExpectEval("class A { constructor(n){ this.n = n } } "
               "class B extends A { constructor(){ super(3) } } new B().n",
               "3");
    ExpectEval("class A { hi(){ return 'hi' } } class B extends A {} new B().hi()", "hi");
    ExpectEval("class A { hi(){ return 'A' } } class B extends A { hi(){ return super.hi() + 'B' } } "
               "new B().hi()",
               "AB");
    ExpectEval("class A{} class B extends A{} (new B()) instanceof A", "true");
    ExpectEval("class A{} class B extends A{} (new B()) instanceof B", "true");
  });

  AddTest(tests, "JsInterpreter/SuperResolvesAgainstTheDefiningClassNotTheReceiver", [] {
    // Three levels is the case that catches it: resolving against the
    // receiver's prototype makes the middle class call itself forever.
    ExpectEval("class A { m(){ return 'a' } } "
               "class B extends A { m(){ return super.m() + 'b' } } "
               "class C extends B { m(){ return super.m() + 'c' } } new C().m()",
               "abc");
  });

  AddTest(tests, "JsInterpreter/DerivedFieldsInitializeAfterTheSuperCall", [] {
    // Which is the ordering that lets a derived field read a base one. Doing
    // it before instead leaves the field undefined in the constructor.
    ExpectEval("class A { constructor(){ this.a = 1 } } "
               "class B extends A { b = this.a + 1; constructor(){ super() } } new B().b",
               "2");
  });

  AddTest(tests, "JsInterpreter/ExtendingSomethingThatIsNotAConstructorThrows", [] {
    ExpectEval("class A extends 5 {}", "throw TypeError: class can only extend a constructor or null");
    // `super.m` in a class with no `extends` resolves against Object.prototype,
    // finds nothing, and fails as a call rather than as syntax -- which is what
    // a real engine does too.
    ExpectEval("class A { m(){ return super.m() } } new A().m()",
               "throw TypeError: undefined is not a function");
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
        .map(p => p.name.toUpperCase())
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
    ExpectEqString(log.at(1), "ALAN, GRACE", "filter and map and join");
    ExpectEqString(log.at(2), R"({"count":3,"age":122})", "and reduce into an object");
  });

  // --- Function.prototype ---------------------------------------------------

  AddTest(tests, "JsInterpreter/CallAndApplyRedirectTheReceiver", [] {
    ExpectEval("function f(){ return this.n } f.call({ n: 7 })", "7");
    ExpectEval("function f(a, b){ return this.n + a + b } f.call({ n: 1 }, 2, 3)", "6");
    ExpectEval("function f(a, b){ return this.n + a + b } f.apply({ n: 1 }, [2, 3])", "6");
    // apply with nothing to spread is a call with no arguments, not an error.
    ExpectEval("function f(){ return arguments.length } f.apply(null)", "0");
    // A borrowed method is the whole point: it runs against what it is given.
    ExpectEval("String.prototype.toUpperCase.call('ab')", "AB");
    ExpectEval("String.prototype.slice.call(12345, 1, 3)", "23");
    ExpectEval("try { Function.prototype.call.call(7) } catch (e) { e.name }", "TypeError");
    // No path from a source string to running code: no `Function(...)`, and no
    // `eval` for it to hide behind either.
    ExpectEval("try { Function('return 1') } catch (e) { e.name }", "TypeError");
    ExpectEval("typeof eval", "undefined");
    ExpectEval("Function.prototype.call === (function(){}).call", "true");
    // A throw from inside travels out rather than being swallowed.
    ExpectEval("try { (function(){ throw 'boom' }).call({}) } catch (e) { e }", "boom");
  });

  AddTest(tests, "JsInterpreter/BindFixesTheReceiverAndPrependsArguments", [] {
    ExpectEval("function f(){ return this.n } f.bind({ n: 7 })()", "7");
    ExpectEval("function f(a, b){ return a + b } f.bind(null, 1)(2)", "3");
    ExpectEval("function f(a, b, c){ return '' + a + b + c } f.bind(null, 1, 2)(3)", "123");
    // Binding twice cannot re-point `this`: the second bind's receiver is the
    // first bound function's, which already ignores it.
    ExpectEval("function f(){ return this.n } f.bind({ n: 1 }).bind({ n: 2 })()", "1");
    ExpectEval("function f(){} f.bind(null).name", "bound f");
    ExpectEval("const o = { n: 5, get(){ return this.n } }; const g = o.get.bind(o); g()", "5");
  });

  AddTest(tests, "JsInterpreter/BoundFunctionsSurviveCollection", [] {
    // The reason bind keeps its state in properties rather than in the
    // std::function's captures: the collector marks properties and cannot see
    // captures. A bound function whose target was only captured would be a
    // dangling pointer the moment anything allocated enough, and calling it
    // would be a use-after-free rather than a wrong answer.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      const g = (function (a, b) { return this.n + a + b }).bind({ n: 100 }, 20);
      let sink = null;
      for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }
      g(3)
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "123",
                   "the target, receiver and bound arguments all outlived the collector");
  });

  // --- String.prototype -----------------------------------------------------

  AddTest(tests, "JsInterpreter/StringMethodsComeFromASharedPrototype", [] {
    // Not copied onto each value: the identity is what makes a method
    // borrowable, and what a page relies on when it caches one.
    ExpectEval("'a'.trim === 'b'.trim", "true");
    ExpectEval("'a'.trim === String.prototype.trim", "true");
    ExpectEval("String.prototype.constructor === String", "true");
    ExpectEval("typeof 'a'.nope", "undefined");
    // A page can add one, and it is found the same way.
    ExpectEval("String.prototype.shout = function() { return this + '!'; }; 'hi'.shout()", "hi!");
  });

  AddTest(tests, "JsInterpreter/StringCharacterAccess", [] {
    ExpectEval("'abc'.charAt(1)", "b");
    ExpectEval("'abc'.charAt(9)", "");        // out of range is empty
    ExpectEval("'abc'.charAt(-1)", "");       // charAt does not count from the end
    ExpectEval("'abc'.at(-1)", "c");          // at does
    ExpectEval("typeof 'abc'.at(9)", "undefined");  // and is undefined past the end
    ExpectEval("'abc'.charCodeAt(0)", "97");
    ExpectEval("'abc'.charCodeAt(9)", "NaN");  // not 0
    ExpectEval("String.fromCharCode(104, 105)", "hi");
    // fromCharCode inverts charCodeAt exactly, which is the property that has
    // to hold for the byte model to be self-consistent.
    ExpectEval("String.fromCharCode('x'.charCodeAt(0))", "x");
  });

  AddTest(tests, "JsInterpreter/StringSearching", [] {
    ExpectEval("'abcabc'.indexOf('b')", "1");
    ExpectEval("'abcabc'.indexOf('b', 2)", "4");
    ExpectEval("'abc'.indexOf('z')", "-1");
    ExpectEval("'abc'.indexOf('')", "0");
    ExpectEval("'abc'.indexOf('', 99)", "3");  // the position clamps to the length
    ExpectEval("'abcabc'.lastIndexOf('b')", "4");
    ExpectEval("'abcabc'.lastIndexOf('b', 3)", "1");
    ExpectEval("'abc'.includes('bc')", "true");
    ExpectEval("'abc'.startsWith('ab')", "true");
    ExpectEval("'abc'.startsWith('bc', 1)", "true");
    ExpectEval("'abcd'.endsWith('cd')", "true");
    // endsWith's argument is where the string is treated as ending, not where
    // to start looking.
    ExpectEval("'abcd'.endsWith('bc', 3)", "true");
    ExpectEval("'abcd'.endsWith('cd', 3)", "false");
  });

  AddTest(tests, "JsInterpreter/StringSliceAndSubstringDifferOnNegatives", [] {
    ExpectEval("'abcdef'.slice(1, 3)", "bc");
    ExpectEval("'abcdef'.slice(-2)", "ef");
    ExpectEval("'abcdef'.slice(3, 1)", "");  // reversed is empty
    ExpectEval("'abcdef'.substring(3, 1)", "bc");  // reversed swaps
    ExpectEval("'abcdef'.substring(-2)", "abcdef");  // negative clamps to zero
    // Saturating rather than wrapping: these are the values a fuzzer reaches
    // for, and narrowing them without the clamp is undefined behaviour.
    ExpectEval("'abc'.slice(1e300)", "");
    ExpectEval("'abc'.slice(-1e300)", "abc");
    ExpectEval("'abc'.slice(NaN, NaN)", "");
  });

  AddTest(tests, "JsInterpreter/StringCaseAndWhitespace", [] {
    ExpectEval("'Ab1'.toUpperCase()", "AB1");
    ExpectEval("'Ab1'.toLowerCase()", "ab1");
    ExpectEval("'  a b  '.trim()", "a b");
    ExpectEval("'  a  '.trimStart() + '|'", "a  |");
    ExpectEval("'  a  '.trimEnd() + '|'", "  a|");
    ExpectEval("'   '.trim() + '|'", "|");
  });

  AddTest(tests, "JsInterpreter/StringSplit", [] {
    ExpectEval("'a,b,c'.split(',').join('|')", "a|b|c");
    ExpectEval("'a,b,'.split(',').length", "3");  // the empty tail is a part
    ExpectEval("'abc'.split('').join('|')", "a|b|c");
    ExpectEval("''.split('').length", "0");  // but an empty string has none
    ExpectEval("''.split('x').length", "1");  // unless the separator misses
    ExpectEval("'a,b,c'.split(',', 2).join('|')", "a|b");
    ExpectEval("'a,b'.split(',', 0).length", "0");
    // No separator at all is one part, which is not the same as an empty one.
    ExpectEval("'a,b'.split().length", "1");
  });

  AddTest(tests, "JsInterpreter/StringReplace", [] {
    ExpectEval("'aaa'.replace('a', 'X')", "Xaa");   // first only
    ExpectEval("'aaa'.replaceAll('a', 'X')", "XXX");
    ExpectEval("'abc'.replace('z', 'X')", "abc");   // no match is unchanged
    ExpectEval("'a-b'.replace('-', '$&$&')", "a--b");
    ExpectEval("'a-b'.replace('-', '$$')", "a$b");
    ExpectEval("'a-b'.replace('-', \"[$`|$']\")", "a[a|b]b");
    // A function replacement receives the spec's (match, position, whole).
    ExpectEval("'a1b2'.replaceAll('1', (m, i, s) => m + ':' + i + ':' + s.length)", "a1:1:4b2");
    // An empty pattern matches at every position, including past the end. The
    // cursor has to advance anyway or this never terminates.
    ExpectEval("'ab'.replaceAll('', '-')", "-a-b-");
    ExpectEval("''.replaceAll('', '-')", "-");
    ExpectEval("'abc'.replace('', '-')", "-abc");
    // A throw from the callback propagates rather than being swallowed.
    ExpectEval("try { 'a'.replace('a', () => { throw 'boom'; }); } catch (e) { e }", "boom");
  });

  AddTest(tests, "JsInterpreter/StringBuilding", [] {
    ExpectEval("'ab'.repeat(3)", "ababab");
    ExpectEval("'ab'.repeat(0) + '|'", "|");
    ExpectEval("'5'.padStart(3, '0')", "005");
    ExpectEval("'5'.padEnd(3)", "5  ");        // the default pad is a space
    ExpectEval("'abc'.padStart(2)", "abc");    // already long enough
    ExpectEval("'ab'.padStart(7, '123')", "12312ab");  // the last repeat truncates
    ExpectEval("'ab'.padStart(7, '')", "ab");  // an empty pad cannot fill
    ExpectEval("'a'.concat('b', 1)", "ab1");
  });

  AddTest(tests, "JsInterpreter/StringAllocationIsBounded", [] {
    // Every one of these is a length a page chose, multiplied. Without the
    // bound the answer is gigabytes; with it, it is a value a script can catch.
    ExpectEval("try { 'x'.repeat(1e9) } catch (e) { e.name }", "RangeError");
    ExpectEval("try { 'x'.repeat(-1) } catch (e) { e.name }", "RangeError");
    ExpectEval("try { 'x'.repeat(Infinity) } catch (e) { e.name }", "RangeError");
    ExpectEval("try { 'x'.padStart(1e9) } catch (e) { e.name }", "RangeError");
  });

  AddTest(tests, "JsInterpreter/WellKnownPrototypesSurviveCollection", [] {
    // Every prototype the interpreter holds has to be in the collector's root
    // set. One that is not gets freed the first time a program allocates
    // enough, and every method read off it afterwards is a use-after-free.
    // Dropping the array prototype from that list turns this test into a
    // segfault, which is what makes it a guard rather than a description.
    //
    // String.prototype is reachable a second way, through `String.prototype`
    // on the constructor -- but a page can delete that property, so the
    // explicit root is what it actually rests on.
    //
    // Only the string and array prototypes carry methods so far, so those are
    // the two this can observe; the object and function prototypes are empty
    // and get covered here the moment anything is installed on them.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      let sink = null;
      for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }
      ['ab'.toUpperCase(), [3, 1, 2].join('-'), 'a,b'.split(',').length].join(' ')
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "AB 3-1-2 2",
                   "the string and array prototypes outlived the collector");
  });

  // --- Regular expressions, as the language sees them ------------------------

  AddTest(tests, "JsInterpreter/ARegExpLiteralIsAPatternRatherThanItsOwnText", [] {
    // The whole point of the engine landing: before it, every one of these
    // evaluated to the source text of the literal, and `test` did not exist.
    ExpectEval("typeof /a/", "object");
    ExpectEval("String(/ab+c/gi)", "/ab+c/gi");
    ExpectEval("/\\d+/.test('abc123')", "true");
    ExpectEval("/\\d+/.test('abc')", "false");
    ExpectEval("/a/.source + ' ' + /a/gi.flags", "a gi");
    ExpectEval("/(?:)/.source", "(?:)");
    ExpectEval("/a/g.global + ' ' + /a/.global", "true false");
  });

  AddTest(tests, "JsInterpreter/ExecReportsWhereEachGroupLanded", [] {
    ExpectEval("/(\\d)(\\d)/.exec('ab12')[2]", "2");
    ExpectEval("/(\\d)(\\d)/.exec('ab12').index", "2");
    ExpectEval("/(\\d)(\\d)/.exec('ab12').input", "ab12");
    ExpectEval("/x/.exec('ab') === null", "true");
    // A branch not taken leaves its group undefined, which is what a page
    // tests for.
    ExpectEval("typeof /(a)|(b)/.exec('b')[1]", "undefined");
    ExpectEval("/(?<y>\\d{4})-(?<m>\\d{2})/.exec('2026-08').groups.m", "08");
  });

  AddTest(tests, "JsInterpreter/AGlobalRegExpCarriesItsPositionInLastIndex", [] {
    // `g` makes a regex stateful, and the state is a property a page reads,
    // writes, and trips over.
    ExpectEval("const r = /a/g; [r.test('aa'), r.lastIndex, r.test('aa'), r.test('aa')].join(' ')",
               "true 1 true false");
    ExpectEval("const r = /a/g; r.test('aa'); r.lastIndex = 0; r.test('aa')", "true");
    ExpectEval("const r = /a/; [r.test('aa'), r.lastIndex].join(' ')", "true 0");
  });

  AddTest(tests, "JsInterpreter/StringMethodsTakeAPatternAsWellAsAString", [] {
    ExpectEval("'a1b2'.replace(/\\d/g, '#')", "a#b#");
    ExpectEval("'a1b2'.replace(/\\d/, '#')", "a#b2");
    ExpectEval("'2026-08-03'.replace(/(\\d+)-(\\d+)-(\\d+)/, '$3/$2/$1')", "03/08/2026");
    ExpectEval("'2026-08'.replace(/(?<y>\\d+)-(?<m>\\d+)/, '$<m> of $<y>')", "08 of 2026");
    ExpectEval("'aBcD'.replace(/[A-Z]/g, c => '_' + c.toLowerCase())", "a_bc_d");
    ExpectEval("'  a  b '.trim().split(/\\s+/).join('|')", "a|b");
    // A separator's captures join the result, which is what makes this five
    // pieces rather than three.
    ExpectEval("'a1b22c'.split(/(\\d+)/).join('|')", "a|1|b|22|c");
    ExpectEval("'axb'.search(/b/) + ' ' + 'axb'.search(/q/)", "2 -1");
    ExpectEval("'aaa'.match(/a/g).length", "3");
    ExpectEval("'ab12'.match(/(\\d)(\\d)/)[1]", "1");
    ExpectEval("[...'a1b2'.matchAll(/\\d/g)].length", "2");
    // A pattern handed to matchAll must be global, and the spec makes that a
    // TypeError rather than an implicit `g`: the two readings of
    // `s.matchAll(/x/)` differ by an infinite loop.
    ExpectEval("try { 'a'.matchAll(/a/) } catch (e) { e.name }", "TypeError");
  });

  AddTest(tests, "JsInterpreter/WhichMethodsConvertAStringToAPatternAndWhichDoNot", [] {
    // The two rules genuinely differ, and getting them the wrong way round is
    // silently wrong rather than an error. `match` converts, so its `.` means
    // any character; `replace` and `split` do not, so theirs means a dot.
    ExpectEval("'a.c'.match('.')[0]", "a");
    ExpectEval("'a.c'.replace('.', '-')", "a-c");
    ExpectEval("'a.c'.replace(/./, '-')", "-.c");
    ExpectEval("'a.b.c'.split('.').length", "3");
    ExpectEval("'axc'.search('x')", "1");
    // A converted pattern is global, because matchAll requires one.
    ExpectEval("[...'a1b2'.matchAll('\\\\d')].length", "2");
    // `match` with no argument matches the empty pattern at position 0 rather
    // than failing, which is what makes `''.match()` an empty match.
    ExpectEval("'abc'.match().index", "0");
  });

  AddTest(tests, "JsInterpreter/TheRegExpConstructorCompilesAtRuntime", [] {
    ExpectEval("new RegExp('a+', 'i').test('AAA')", "true");
    // Copying a pattern keeps its flags, unless new ones are given.
    ExpectEval("new RegExp(/b/g).flags", "g");
    ExpectEval("new RegExp(/b/g, 'i').flags", "i");
    ExpectEval("RegExp('x').test('x')", "true");
    ExpectEval("try { new RegExp('('); } catch (e) { e.name }", "SyntaxError");
    ExpectEval("try { new RegExp('a', 'q'); } catch (e) { e.name }", "SyntaxError");
    // And compiling a pattern at runtime is still not a path from a string to
    // running code -- there is no `eval`, and a test elsewhere says so.
    ExpectEval("try { eval; } catch (e) { e.name }", "ReferenceError");
  });

  AddTest(tests, "JsInterpreter/AMalformedLiteralIsRefusedRatherThanMatchingNothing", [] {
    // Two different refusals, at two different times. An unterminated literal
    // never reaches the pattern compiler: the lexer cannot find where it ends,
    // so the whole program is a syntax error -- which is what a real engine
    // does too, and why the `try` around it does not catch anything.
    ExpectEval("try { /[a/.test('a') } catch (e) { e.name }",
               "throw SyntaxError: unterminated regular expression (line 1)");
    // A literal whose extent *is* clear but whose pattern is not compiles at
    // evaluation, so this one is catchable. A real engine reports it earlier;
    // the difference is when, not whether.
    ExpectEval("try { /(/.test('a') } catch (e) { e.name }", "SyntaxError");
  });

  AddTest(tests, "JsInterpreter/ACompiledPatternSurvivesACollection", [] {
    // The compiled pattern lives beside the object, in a table the collector
    // prunes. If the sweep dropped an entry for a live object, every match
    // after the next collection would fail; if it kept one for a dead object,
    // a later allocation at the same address would inherit it.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      const keep = /(\d+)/;
      let sink = null;
      for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }
      keep.exec('n42')[1]
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "42", "the pattern outlived the collector");
  });

  // --- Symbols and the iteration protocol -----------------------------------

  AddTest(tests, "JsInterpreter/ASymbolIsAKeyThatCannotBeWrittenOut", [] {
    ExpectEval("typeof Symbol()", "symbol");
    ExpectEval("typeof Symbol.iterator", "symbol");
    // Identity, not description. This is the property everything else rests on.
    ExpectEval("Symbol('x') === Symbol('x')", "false");
    ExpectEval("const s = Symbol('x'); s === s", "true");
    ExpectEval("Symbol('x').description", "x");
    ExpectEval("String(Symbol('x'))", "Symbol(x)");
    ExpectEval("Symbol('x').toString()", "Symbol(x)");
    // A symbol-keyed property is invisible to everything that enumerates,
    // which is what makes a symbol safe to hang a protocol hook on.
    ExpectEval("const s = Symbol(); const o = { plain: 1 }; o[s] = 2; "
               "[o[s], Object.keys(o).join(''), JSON.stringify(o)].join(' ')",
               "2 plain {\"plain\":1}");
    // And it does not collide with the string of the same text.
    ExpectEval("const s = Symbol('k'); const o = {}; o[s] = 1; o['Symbol(k)'] = 2; o[s]", "1");
  });

  AddTest(tests, "JsInterpreter/TheSymbolRegistryIsTheOneWayTwoSymbolsAreOne", [] {
    ExpectEval("Symbol.for('k') === Symbol.for('k')", "true");
    ExpectEval("Symbol.for('k') === Symbol('k')", "false");
    ExpectEval("Symbol.keyFor(Symbol.for('k'))", "k");
    ExpectEval("typeof Symbol.keyFor(Symbol('k'))", "undefined");
  });

  AddTest(tests, "JsInterpreter/AnythingWithASymbolIteratorCanBeIterated", [] {
    const std::string range =
        "const range = { from: 1, to: 4, [Symbol.iterator]() { let n = this.from, to = this.to; "
        "return { next: () => n <= to ? { value: n++, done: false } "
        ": { value: undefined, done: true } }; } }; ";
    ExpectEval(range + "let s = ''; for (const v of range) s += v; s", "1234");
    ExpectEval(range + "[...range].join('-')", "1-2-3-4");
    ExpectEval(range + "const [a, b, ...rest] = range; a + '|' + b + '|' + rest.join(',')",
               "1|2|3,4");
    ExpectEval(range + "function f(...xs){ return xs.length } f(...range)", "4");
    ExpectEval("try { for (const x of {}) {} } catch (e) { e.name }", "TypeError");
  });

  AddTest(tests, "JsInterpreter/ForOfStopsAskingAtABreak", [] {
    // The reason `for...of` drives the protocol one value at a time instead of
    // draining it first. An iterator with side effects must not be stepped
    // past the break, and one that never ends must still be breakable.
    ExpectEval("let calls = 0; const counted = { [Symbol.iterator]() { "
               "return { next: () => ({ value: ++calls, done: false }) }; } }; "
               "for (const v of counted) { if (v === 3) break; } calls",
               "3");
  });

  AddTest(tests, "JsInterpreter/StringsAndArraysPublishTheSameProtocol", [] {
    ExpectEval("[...'abc'].join('-')", "a-b-c");
    ExpectEval("const [first] = 'xyz'; first", "x");
    ExpectEval("[...[1, 2, 3]].join('')", "123");
    // The iterator objects themselves, which a page can drive by hand.
    ExpectEval("const it = [10, 20][Symbol.iterator](); "
               "[it.next().value, it.next().value, it.next().done].join(' ')",
               "10 20 true");
    ExpectEval("typeof 'a'[Symbol.iterator]", "function");
    // An iterator is itself iterable, which is what lets one be re-fed to a
    // `for...of` or a spread.
    ExpectEval("[...'ab'.matchAll(/./g)].length", "2");
  });

  AddTest(tests, "JsInterpreter/ReplacingTheBuiltInIteratorIsObserved", [] {
    // The array fast path is only taken while the hook is still the built-in
    // one. A page that replaces it gets what it asked for, not the shortcut.
    ExpectEval("const xs = [1, 2, 3]; "
               "xs[Symbol.iterator] = function(){ let n = 0; "
               "return { next: () => n++ < 2 ? { value: 'x', done: false } : { done: true } }; }; "
               "[...xs].join('')",
               "xx");
  });

  AddTest(tests, "JsInterpreter/AHoleInADestructuringPatternStillConsumesAValue", [] {
    // `const [, b] = xs` can only bind the second by stepping past the first,
    // which is visible when the source is an iterator rather than an array.
    ExpectEval("const [, b] = [1, 2]; b", "2");
    ExpectEval("let n = 0; const source = { [Symbol.iterator]() { "
               "return { next: () => ({ value: ++n, done: n > 5 }) }; } }; "
               "const [, second] = source; second",
               "2");
  });

  AddTest(tests, "JsInterpreter/ASymbolKeyedMethodSurvivesACollection", [] {
    // A symbol whose only reference is that it is a property key has to be
    // marked through the key. Missing that frees the cell while the map still
    // points at it, and the next allocation lands on top of it.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      const key = Symbol('held');
      const holder = {};
      holder[key] = 'kept';
      let sink = null;
      for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }
      holder[key] + ' ' + [...[1, 2]].length
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "kept 2",
                   "the symbol and the iteration hooks outlived the collector");
  });

  // --- Map and Set -----------------------------------------------------------

  AddTest(tests, "JsInterpreter/AMapKeepsInsertionOrderAndOneEntryPerKey", [] {
    ExpectEval("const m = new Map(); m.set('a', 1).set('b', 2).set('a', 3); "
               "[m.size, m.get('a'), m.get('b'), typeof m.get('z')].join(' ')",
               "2 3 2 undefined");
    // Re-setting a key keeps its place rather than moving it to the end.
    ExpectEval("const m = new Map([['a', 1], ['b', 2]]); m.set('a', 9); [...m.keys()].join('')",
               "ab");
    ExpectEval("const m = new Map([['a', 1]]); [m.has('a'), m.has('b')].join(' ')", "true false");
    ExpectEval("JSON.stringify([...new Map([['a', 1]])])", "[[\"a\",1]]");
  });

  AddTest(tests, "JsInterpreter/MapKeysAreComparedByIdentityAndSameValueZero", [] {
    // Two empty objects are two keys, which is the whole reason a Map exists
    // rather than an object with string keys.
    ExpectEval("const a = {}, b = {}; const m = new Map([[a, 1], [b, 2]]); "
               "[m.get(a), m.get(b), typeof m.get({}), m.size].join(' ')",
               "1 2 undefined 2");
    // SameValueZero, which is `===` with two changes: NaN finds itself, and
    // the two zeros are one key.
    ExpectEval("const m = new Map([[NaN, 'n']]); m.get(NaN)", "n");
    ExpectEval("const m = new Map([[0, 'z']]); m.get(-0)", "z");
    ExpectEval("const m = new Map([['1', 's']]); typeof m.get(1)", "undefined");
  });

  AddTest(tests, "JsInterpreter/DeletingFromAMapDoesNotDisturbTheRest", [] {
    ExpectEval("const m = new Map([['a', 1], ['b', 2], ['c', 3]]); "
               "[m.delete('b'), m.delete('b'), m.size, [...m.keys()].join('')].join(' ')",
               "true false 2 ac");
    ExpectEval("const m = new Map([['a', 1]]); m.clear(); [m.size, m.has('a')].join(' ')",
               "0 false");
  });

  AddTest(tests, "JsInterpreter/ASetHoldsEachMemberOnce", [] {
    ExpectEval("const s = new Set([1, 2, 2, 3]); [s.size, [...s].join('')].join(' ')", "3 123");
    ExpectEval("const s = new Set(); s.add(1).add(1); s.size", "1");
    ExpectEval("[...new Set('hello')].join('')", "helo");
    // A Set's `entries` yields each member twice, which is what makes a
    // callback written for a Map work on one.
    ExpectEval("JSON.stringify([...new Set([1]).entries()])", "[[1,1]]");
    ExpectEval("let seen = ''; new Set(['a']).forEach((v, k) => seen = v + k); seen", "aa");
  });

  AddTest(tests, "JsInterpreter/BothTakeAnyIterableAndPublishOne", [] {
    // The reason these landed after the iteration protocol rather than before.
    ExpectEval("const src = { [Symbol.iterator]() { let n = 0; "
               "return { next: () => n < 3 ? { value: n++, done: false } : { done: true } }; } }; "
               "new Set(src).size",
               "3");
    ExpectEval("new Map(new Map([['a', 1]])).get('a')", "1");
    ExpectEval("new Set(new Set([7])).has(7)", "true");
    // `for...of` over a Map yields pairs, over a Set yields members.
    ExpectEval("let out = ''; for (const [k, v] of new Map([['a', 1]])) out = k + v; out", "a1");
    ExpectEval("let out = ''; for (const v of new Set(['x'])) out = v; out", "x");
    ExpectEval("function f(...xs){ return xs.length } f(...new Set([1, 2]))", "2");
  });

  AddTest(tests, "JsInterpreter/ALargeMapDoesNotDegradeToAScan", [] {
    // Not a timing assertion -- a wrong answer is what an index bug produces
    // first. Twenty thousand entries, every one still findable after deleting
    // all but the last, which is the case a hole-leaving delete gets wrong.
    ExpectEval("const m = new Map(); for (let i = 0; i < 20000; i++) m.set('k' + i, i); "
               "const before = m.get('k19999'); "
               "for (let i = 0; i < 19999; i++) m.delete('k' + i); "
               "[before, m.size, m.get('k19999'), typeof m.get('k0')].join(' ')",
               "19999 1 19999 undefined");
  });

  AddTest(tests, "JsInterpreter/AMapSurvivesACollectionWithItsIndex", [] {
    // The index lives beside the object in the heap and is dropped by the
    // sweep that frees it. A map still live must keep both.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      const key = {};
      const m = new Map([[key, 'kept'], ['s', 'also']]);
      let sink = null;
      for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }
      m.get(key) + ' ' + m.get('s') + ' ' + m.size
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "kept also 2", "the map outlived the collector");
  });

  // --- Errors ---------------------------------------------------------------

  AddTest(tests, "JsInterpreter/ErrorsHaveConstructorsAndTypes", [] {
    ExpectEval("new Error('boom').message", "boom");
    ExpectEval("String(new TypeError('t'))", "TypeError: t");
    ExpectEval("String(new Error())", "Error");
    // Callable with or without `new`, which is one of the few places the
    // language says the two are the same.
    ExpectEval("Error('no new').message", "no new");
    ExpectEval("new TypeError('t') instanceof TypeError", "true");
    ExpectEval("new TypeError('t') instanceof Error", "true");
    ExpectEval("new Error('e') instanceof TypeError", "false");
    // The one that matters: an error the *engine* threw is the same kind of
    // object as one a page made, so `catch (e) { if (e instanceof TypeError) }`
    // works on both.
    ExpectEval("try { null.x } catch (e) { [e.name, e instanceof TypeError].join(' ') }",
               "TypeError true");
    ExpectEval("new Error('x', { cause: 'y' }).cause", "y");
  });

  // --- Promises and the microtask queue -------------------------------------

  AddTest(tests, "JsInterpreter/APromiseSettlesAfterTheScriptThatMadeIt", [] {
    // The defining property, and the reason a microtask queue exists at all:
    // a handler never runs during the turn that attached it.
    const std::vector<std::string> lines = Log(
        "console.log('before');"
        "Promise.resolve('value').then(v => console.log('then ' + v));"
        "console.log('after');");
    ExpectEqInt(static_cast<long long>(lines.size()), 3, "three lines");
    ExpectEqString(lines[0], "before", "sync first");
    ExpectEqString(lines[1], "after", "the rest of the script before any handler");
    ExpectEqString(lines[2], "then value", "and the handler at the end of the turn");
  });

  AddTest(tests, "JsInterpreter/AnExecutorRunsImmediatelyButItsHandlersDoNot", [] {
    // The half of the design a page depends on: `new Promise(r => { save = r })`
    // only works because the executor is synchronous.
    const std::vector<std::string> lines = Log(
        "let settle; const p = new Promise(r => { console.log('executor'); settle = r; });"
        "p.then(v => console.log('got ' + v));"
        "settle(7);"
        "console.log('sync end');");
    ExpectEqInt(static_cast<long long>(lines.size()), 3, "three lines");
    ExpectEqString(lines[0], "executor", "the executor ran during the constructor");
    ExpectEqString(lines[1], "sync end", "and the handler still waited");
    ExpectEqString(lines[2], "got 7", "for the end of the turn");
  });

  AddTest(tests, "JsInterpreter/AChainPassesValuesAndRejectionsAlong", [] {
    ExpectEqString(Log("Promise.resolve(5).then(v => v * 2).then(v => console.log('chain ' + v))")
                       .at(0),
                   "chain 10", "each handler's return feeds the next");
    // A rejection travels down the chain until something catches it, which is
    // what a `then` with no rejection handler has to do.
    ExpectEqString(Log("Promise.reject('no').then(() => console.log('skipped'))"
                       ".catch(e => console.log('caught ' + e))")
                       .at(0),
                   "caught no", "a rejection skips the value handlers");
    // A handler that throws rejects the promise it returns.
    ExpectEqString(Log("Promise.resolve().then(() => { throw new Error('x') })"
                       ".catch(e => console.log('rethrown ' + e.message))")
                       .at(0),
                   "rethrown x", "a throwing handler rejects its promise");
    // `finally` observes without intercepting.
    const std::vector<std::string> lines =
        Log("Promise.reject('r').finally(() => console.log('finally'))"
            ".catch(e => console.log('still ' + e))");
    ExpectEqString(lines.at(0), "finally", "it ran");
    ExpectEqString(lines.at(1), "still r", "and the rejection passed through it");
  });

  // --- generators -----------------------------------------------------------

  AddTest(tests, "JsInterpreter/CallingAGeneratorRunsNoneOfItsBody", [] {
    // The whole contract: the call returns an iterator and the body has not
    // started. If it had, the log would say so before `next` was ever called.
    const std::vector<std::string> log = Log(
        "function* g(){ console.log('body'); yield 1; }\n"
        "const it = g();\n"
        "console.log('called');\n"
        "it.next();\n");
    ExpectEqInt(static_cast<long long>(log.size()), 2, "two lines");
    ExpectEqString(log.at(0), "called", "the call itself runs no line of the body");
    ExpectEqString(log.at(1), "body", "the first next is what starts it");
    // The parameters, though, are bound at the call -- so a default that
    // throws throws there rather than three lines later.
    ExpectEval("function boom(){ throw 'at the call' }\n"
               "function* g(a = boom()){ yield a }\n"
               "try { g(); 'no throw' } catch (e) { e }",
               "at the call");
  });

  AddTest(tests, "JsInterpreter/NextWalksTheYieldsAndThenTheReturn", [] {
    ExpectEval("function* g(){ yield 1; yield 2; return 3 }\n"
               "const it = g();\n"
               "[it.next(), it.next(), it.next(), it.next()]\n"
               "  .map(r => r.value + ':' + r.done).join(' ')",
               "1:false 2:false 3:true undefined:true");
    // A bare `yield` yields undefined rather than nothing.
    ExpectEval("function* g(){ yield }\n g().next().value === undefined", "true");
  });

  AddTest(tests, "JsInterpreter/NextSendsAValueBackToTheYieldThatIsWaiting", [] {
    // The half of the protocol that makes a generator two-way, and the half a
    // collected-up-front implementation cannot have.
    ExpectEval("function* g(){ const a = yield 'first'; const b = yield a * 2; return a + b }\n"
               "const it = g();\n"
               "it.next().value + ' ' + it.next(10).value + ' ' + it.next(5).value",
               "first 20 15");
    // The value sent to the *first* next has nothing waiting for it, so it is
    // dropped rather than becoming anything.
    ExpectEval("function* g(){ return yield 1 }\n"
               "const it = g();\n"
               "it.next('ignored');\n"
               "it.next('kept').value",
               "kept");
  });

  AddTest(tests, "JsInterpreter/AGeneratorIsItsOwnIterableEverywhereOneIsTaken", [] {
    ExpectEval("function* g(){ yield 1; yield 2; yield 3 }\n"
               "let total = 0; for (const n of g()) total += n; total",
               "6");
    ExpectEval("function* g(){ yield 1; yield 2 }\n[...g()].join(',')", "1,2");
    ExpectEval("function* g(){ yield 'a'; yield 'b'; yield 'c' }\n"
               "const [first, ...rest] = g(); first + '/' + rest.join('')",
               "a/bc");
    // The identity `for...of` relies on: a generator's Symbol.iterator gives
    // back the generator, so a half-walked one carries on where it stopped.
    ExpectEval("function* g(){ yield 1; yield 2; yield 3 }\n"
               "const it = g(); it.next();\n"
               "[...it].join(',')",
               "2,3");
  });

  AddTest(tests, "JsInterpreter/AGeneratorSuspendsWhereverItWasWritten", [] {
    // A yield half way through an expression, inside two blocks, with a
    // `for...of` cursor open -- the case the filed frame exists to survive.
    ExpectEval("function* g(){\n"
               "  for (const n of [1, 2]) {\n"
               "    if (n) { const doubled = 2 * (yield n) ; yield doubled }\n"
               "  }\n"
               "}\n"
               "const it = g();\n"
               "[it.next().value, it.next(10).value, it.next().value, it.next(3).value].join(',')",
               "1,20,2,6");
    // And a yield inside a try, so the handler table's depths survive too.
    ExpectEval("function* g(){ try { yield 1; yield 2 } finally { } }\n"
               "[...g()].join(',')",
               "1,2");
  });

  AddTest(tests, "JsInterpreter/ThrowArrivesAtTheYieldThatIsWaiting", [] {
    ExpectEval("function* g(){ try { yield 1 } catch (e) { yield 'caught ' + e } yield 'after' }\n"
               "const it = g();\n"
               "it.next();\n"
               "it.throw('boom').value + ' ' + it.next().value",
               "caught boom after");
    // Uncaught inside the body, so it comes out of `throw` at the caller and
    // the generator is finished.
    ExpectEval("function* g(){ yield 1; yield 2 }\n"
               "const it = g(); it.next();\n"
               "try { it.throw('boom') } catch (e) { e + ' ' + it.next().done }",
               "boom true");
    // Before the first `next` the body has not been entered, so nothing in it
    // can catch -- not even a `try` written around the first yield.
    ExpectEval("function* g(){ try { yield 1 } catch (e) { yield 'caught' } }\n"
               "try { g().throw('boom') } catch (e) { 'thrown at the caller: ' + e }",
               "thrown at the caller: boom");
  });

  AddTest(tests, "JsInterpreter/ReturnFinishesAGeneratorWhereverItIs", [] {
    ExpectEval("function* g(){ yield 1; yield 2 }\n"
               "const it = g(); it.next();\n"
               "const closed = it.return('done here');\n"
               "closed.value + ' ' + closed.done + ' ' + it.next().done",
               "done here true true");
    // A finished generator keeps answering, and keeps answering the same
    // thing. This is what stops a `for...of` over a spent one from hanging.
    ExpectEval("function* g(){ yield 1 }\n"
               "const it = g(); it.next(); it.next();\n"
               "const again = it.next();\n"
               "(again.value === undefined) + ' ' + again.done",
               "true true");
  });

  AddTest(tests, "JsInterpreter/AGeneratorCannotBeResumedWhileItIsRunning", [] {
    // Its frame is on the machine, so putting it back would be the same frame
    // in two places. A TypeError is what the spec says and what this can do.
    ExpectEval("let it;\n"
               "function* g(){ it.next(); yield 1 }\n"
               "it = g();\n"
               "try { it.next() } catch (e) { e.message }",
               "this generator is already running");
  });

  AddTest(tests, "JsInterpreter/YieldStarWalksTheDelegateAndTakesItsReturnValue", [] {
    ExpectEval("function* inner(){ yield 1; yield 2; return 'inner done' }\n"
               "function* outer(){ const got = yield* inner(); yield got }\n"
               "[...outer()].join(',')",
               "1,2,inner done");
    // Any iterable, not only a generator -- `yield*` is defined on the
    // protocol rather than on generators.
    ExpectEval("function* g(){ yield* [1, 2]; yield* 'ab' }\n[...g()].join(',')", "1,2,a,b");
    ExpectEval("function* g(){ yield* [] ; yield 'after' }\n[...g()].join(',')", "after");
  });

  AddTest(tests, "JsInterpreter/GeneratorsAreWrittenInEveryFormAFunctionTakes", [] {
    ExpectEval("const g = function*(){ yield 'expression' };\n g().next().value", "expression");
    ExpectEval("const o = { *g(){ yield 'method' } };\n o.g().next().value", "method");
    ExpectEval("class C { *g(){ yield 'class method' } }\n new C().g().next().value",
               "class method");
    ExpectEval("class C { static *g(){ yield 'static' } }\n C.g().next().value", "static");
    // A generator method sees the receiver it was called on, which is the one
    // thing a filed frame could plausibly lose.
    ExpectEval("const o = { n: 7, *g(){ yield this.n } };\n o.g().next().value", "7");
  });

  AddTest(tests, "JsInterpreter/YieldOutsideAGeneratorIsRejected", [] {
    ExpectEval("function f(){ return yield 1 }\n f()", "throw SyntaxError: yield is only valid "
                                                      "inside a generator");
    // An async generator is refused rather than run as one or the other.
    ExpectEval("async function* g(){ yield 1 }\n typeof g",
               "function");
  });

  // --- async and await ------------------------------------------------------

  AddTest(tests, "JsInterpreter/AnAsyncFunctionReturnsAPromiseAndRunsUntilItWaits", [] {
    // The two halves of the contract. The body runs synchronously up to its
    // first `await` -- so 'a' is logged before the call returns -- and the
    // caller gets a promise the moment it stops, so 'b' beats 'c'.
    const std::vector<std::string> lines =
        Log("async function f(){ console.log('a'); await 0; console.log('c') }"
            "console.log(typeof f().then); f(); console.log('b')");
    ExpectEqString(lines.at(0), "a", "the body starts before the call returns");
    ExpectEqString(lines.at(1), "function", "and the call returns a thenable");
    ExpectEqString(lines.at(2), "a", "the second call starts synchronously too");
    ExpectEqString(lines.at(3), "b", "the caller carries on at the first await");
    ExpectEqString(lines.at(4), "c", "and the rest runs in a later turn");
  });

  AddTest(tests, "JsInterpreter/AnAsyncFunctionSettlesWithWhatItReturnsOrThrows", [] {
    ExpectEqString(Log("async function f(){ return 42 } f().then(v => console.log(v))").at(0),
                   "42", "a return with no await at all");
    ExpectEqString(Log("async function f(){ await 0; return 42 } f().then(v => console.log(v))")
                       .at(0),
                   "42", "and one after waiting");
    ExpectEqString(Log("async function f(){ throw 'boom' } f().catch(e => console.log(e))").at(0),
                   "boom", "a throw is a rejection rather than a throw at the caller");
    ExpectEqString(
        Log("async function f(){ await 0; throw 'boom' } f().catch(e => console.log(e))").at(0),
        "boom", "including one from after it waited");
    // Returning a promise flattens, the same way it does out of a `then`.
    ExpectEqString(
        Log("async function f(){ return Promise.resolve('flat') } f().then(v => console.log(v))")
            .at(0),
        "flat", "an async function that returns a promise does not nest it");
  });

  AddTest(tests, "JsInterpreter/AwaitResumesInsideWhateverItWasWrittenIn", [] {
    // What suspending has to preserve. Each of these has the `await` inside
    // something the frame was holding when it stopped -- a try, a loop with a
    // live iterator, a block with its own bindings, a call under construction.
    ExpectEqString(Log("async function f(){ try { await Promise.reject('x') }"
                       "catch (e) { return 'caught ' + e } } f().then(v => console.log(v))")
                       .at(0),
                   "caught x", "a rejection throws at the await");
    ExpectEqString(Log("async function f(){ let t = 0; for (const x of [1, 2, 3]) { t += await x }"
                       "return t } f().then(v => console.log(v))")
                       .at(0),
                   "6", "an open for...of cursor survives the wait");
    ExpectEqString(Log("async function f(){ let out = '';"
                       "try { for (const x of [1, 2]) { let seen = await x; out += seen } }"
                       "finally { out += '!' } return out } f().then(v => console.log(v))")
                       .at(0),
                   "12!", "and so do the blocks and the finalizer");
    ExpectEqString(Log("async function f(){ return [await 1, await 2, await 3].join('-') }"
                       "f().then(v => console.log(v))")
                       .at(0),
                   "1-2-3", "an array half built when the wait began");
    ExpectEqString(Log("function add(a, b){ return a + b }"
                       "async function f(){ return add(await 1, await 2) }"
                       "f().then(v => console.log(v))")
                       .at(0),
                   "3", "and a call whose arguments were half pushed");
  });

  AddTest(tests, "JsInterpreter/AwaitWorksThroughEveryFormAFunctionTakes", [] {
    ExpectEqString(Log("const f = async () => { await 0; return 'arrow' };"
                       "f().then(v => console.log(v))")
                       .at(0),
                   "arrow", "an async arrow");
    ExpectEqString(Log("const f = async x => x * 2; f(21).then(v => console.log(v))").at(0),
                   "42", "one with a bare parameter and an expression body");
    ExpectEqString(Log("const o = { n: 5, async m(){ await 0; return this.n } };"
                       "o.m().then(v => console.log(v))")
                       .at(0),
                   "5", "an object method, with its receiver still right after waiting");
    ExpectEqString(Log("class A { async m(){ await 0; return 'a' } }"
                       "class B extends A { async m(){ return (await super.m()) + 'b' } }"
                       "new B().m().then(v => console.log(v))")
                       .at(0),
                   "ab", "a class method, including through super");
    ExpectEqString(Log("async function g(){ await 0; return 7 }"
                       "async function f(){ return (await g()) + 1 }"
                       "f().then(v => console.log(v))")
                       .at(0),
                   "8", "and one async function awaiting another");
  });

  AddTest(tests, "JsInterpreter/AsyncIsAKeywordOnlyWhereItModifiesAFunction", [] {
    // It is a contextual keyword, so every one of these has to keep working --
    // and the lookahead that decides is the thing being tested.
    ExpectEval("function async(n){ return n + 1 } async(1)", "2");
    ExpectEval("let async = 1; async += 1; async", "2");
    ExpectEval("const o = { async: 1 }; o.async", "1");
    ExpectEval("const o = { async m(){ return 1 } }; typeof o.m", "function");
    ExpectEval("class C { async(){ return 'method named async' } } new C().async()",
               "method named async");
    // `await` outside an async function is not a keyword either, and using it
    // as an operator there is the error.
    Expect(Eval("function f(){ await 1 } f()").rfind("throw", 0) == 0,
           "await outside an async function is rejected");
  });

  AddTest(tests, "JsInterpreter/APromiseOfAPromiseFlattens", [] {
    // The resolve procedure, which is not just "fulfill". Without it a handler
    // returning a promise hands the next one a promise instead of its value,
    // and every chain that awaits something breaks.
    ExpectEqString(Log("Promise.resolve(Promise.resolve('flat'))"
                       ".then(v => console.log(v))")
                       .at(0),
                   "flat", "a promise resolved with a promise adopts it");
    ExpectEqString(Log("Promise.resolve().then(() => Promise.resolve('inner'))"
                       ".then(v => console.log(v))")
                       .at(0),
                   "inner", "and so does one a handler returned");
    // Any thenable, not only a real promise.
    ExpectEqString(Log("Promise.resolve({ then(res) { res('adopted') } })"
                       ".then(v => console.log(v))")
                       .at(0),
                   "adopted", "a page's own thenable is adopted too");
    // Resolving a promise with itself would wait forever, so it is a rejection.
    ExpectEqString(Log("let settle; const p = new Promise(r => { settle = r }); settle(p);"
                       "p.catch(e => console.log(e.name))")
                       .at(0),
                   "TypeError", "a promise cannot resolve to itself");
  });

  AddTest(tests, "JsInterpreter/TheCombinatorsDifferInWhichAnswerWins", [] {
    ExpectEqString(Log("Promise.all([1, Promise.resolve(2), 3])"
                       ".then(vs => console.log(vs.join('')))").at(0),
                   "123", "all waits for every one, in order");
    ExpectEqString(Log("Promise.all([1, Promise.reject('no')])"
                       ".catch(e => console.log('rejected ' + e))").at(0),
                   "rejected no", "and one rejection ends it");
    ExpectEqString(Log("Promise.race([new Promise(() => {}), Promise.resolve('fast')])"
                       ".then(v => console.log(v))").at(0),
                   "fast", "race takes the first answer");
    ExpectEqString(Log("Promise.allSettled([Promise.resolve(1), Promise.reject('e')])"
                       ".then(rs => console.log(rs.map(r => r.status).join(',')))").at(0),
                   "fulfilled,rejected", "allSettled reports both outcomes");
    ExpectEqString(Log("Promise.any([Promise.reject('a'), Promise.resolve('b')])"
                       ".then(v => console.log(v))").at(0),
                   "b", "any takes the first success");
    ExpectEqString(Log("Promise.any([]).catch(e => console.log(e.name))").at(0),
                   "AggregateError", "and rejects when there is nothing to succeed");
    ExpectEqString(Log("Promise.all([]).then(vs => console.log('empty ' + vs.length))").at(0),
                   "empty 0", "an empty all fulfils at once");
    // Any iterable, not only an array.
    ExpectEqString(Log("Promise.all(new Set([1, 2])).then(vs => console.log(vs.join('')))").at(0),
                   "12", "the combinators take any iterable");
  });

  AddTest(tests, "JsInterpreter/TheQueueIsFirstInFirstOut", [] {
    const std::vector<std::string> lines = Log(
        "queueMicrotask(() => console.log('one'));"
        "Promise.resolve().then(() => console.log('two'));"
        "queueMicrotask(() => console.log('three'));");
    ExpectEqInt(static_cast<long long>(lines.size()), 3, "all three ran");
    ExpectEqString(lines[0] + lines[1] + lines[2], "onetwothree", "in the order queued");
  });

  AddTest(tests, "JsInterpreter/AnEndlessMicrotaskLoopStopsInsteadOfHanging", [] {
    // Every browser hangs on this, because the queue is drained to empty
    // before anything else runs. Here the drain gives up and leaves the rest
    // queued, so the window survives -- see the note on DrainMicrotasks.
    Interpreter interpreter;
    const Result result = interpreter.Run(
        "let n = 0; const loop = () => { n++; Promise.resolve().then(loop) }; loop();"
        "'returned'");
    Expect(!result.IsAbrupt(), "it returned at all: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "returned", "with the program's value");
    Expect(interpreter.HasPendingMicrotasks(),
           "and left the rest of the work queued rather than dropping it");
  });

  // --- Array.prototype -------------------------------------------------------

  AddTest(tests, "JsInterpreter/ArraysSearchAndTest", [] {
    ExpectEval("[5, 1, 4].find(x => x > 3)", "5");
    ExpectEval("[5, 1, 4].findIndex(x => x > 3)", "0");
    ExpectEval("[5, 1, 4].findLast(x => x > 3)", "4");
    ExpectEval("[5, 1, 4].findLastIndex(x => x > 3)", "2");
    ExpectEval("typeof [1].find(x => false)", "undefined");
    ExpectEval("[1].findIndex(x => false)", "-1");
    ExpectEval("[[1, 2].some(x => x > 1), [1, 2].every(x => x > 1)].join(' ')", "true false");
    // Vacuously true, which is the answer that surprises people.
    ExpectEval("[].every(x => false)", "true");
    ExpectEval("[].some(x => true)", "false");
    // `includes` finds NaN where `indexOf` does not, which is why it exists.
    ExpectEval("[[NaN].includes(NaN), [NaN].indexOf(NaN)].join(' ')", "true -1");
    ExpectEval("[[1, 2, 3].at(-1), [1, 2, 3].at(0)].join(' ')", "3 1");
  });

  AddTest(tests, "JsInterpreter/SortIsStableAndComparesStringsByDefault", [] {
    // The default comparator is a *string* comparison, which is why this is
    // the canonical JavaScript surprise rather than a bug here.
    ExpectEval("[1, 10, 2].sort().join(',')", "1,10,2");
    ExpectEval("[1, 10, 2].sort((a, b) => a - b).join(',')", "1,2,10");
    ExpectEval("[5, 1, 4, 2].sort((a, b) => b - a).join(',')", "5,4,2,1");
    // Stability: equal keys keep the order they were in.
    ExpectEval("[{k:1,n:'a'},{k:0,n:'b'},{k:1,n:'c'}].sort((x, y) => x.k - y.k)"
               ".map(x => x.n).join('')",
               "bac");
    // A comparator that throws stops the sort rather than corrupting it.
    ExpectEval("try { [3, 1, 2].sort(() => { throw new Error('x') }) } catch (e) { e.message }",
               "x");
    ExpectEval("try { [1].sort(7) } catch (e) { e.name }", "TypeError");
  });

  AddTest(tests, "JsInterpreter/ArraysRearrangeInPlaceOrCopy", [] {
    ExpectEval("const a = [1, 2, 3, 4, 5]; const cut = a.splice(1, 2); "
               "cut.join(',') + ' | ' + a.join(',')",
               "2,3 | 1,4,5");
    ExpectEval("const a = [1]; a.splice(1, 0, 9); a.join(',')", "1,9");
    ExpectEval("const a = [1, 2]; a.unshift(0); a.join(',')", "0,1,2");
    ExpectEval("const a = [1, 2]; [a.shift(), a.join(',')].join(' ')", "1 2");
    ExpectEval("[1, 2, 3].reverse().join('')", "321");
    ExpectEval("[1, 2].concat([3, 4], 5).join('')", "12345");
    ExpectEval("[1, 2, 3].fill(0, 1).join('')", "100");
    ExpectEval("[1, [2, [3]]].flat().length", "3");
    ExpectEval("[1, [2, [3]]].flat(2).join(',')", "1,2,3");
    ExpectEval("[1, 2].flatMap(x => [x, x]).join('')", "1122");
  });

  AddTest(tests, "JsInterpreter/AHoleIsNotAnUndefinedElement", [] {
    // The rule that is invisible until a page uses a sparse array, and then is
    // wrong everywhere at once.
    // map preserves holes, which JSON writes as null.
    ExpectEval("JSON.stringify([1, , 3].map(x => x * 2))", "[2,null,6]");
    ExpectEval("[1, , 3].filter(() => true).length", "2");
    ExpectEval("let n = 0; [1, , 3].forEach(() => n++); n", "2");
    ExpectEval("[1, , 3].join('-')", "1--3");
    ExpectEval("JSON.stringify([1, , 3].slice(0, 2))", "[1,null]");
  });

  AddTest(tests, "JsInterpreter/TheArrayConstructorHasTwoMeanings", [] {
    // One numeric argument is a length; anything else is the elements. The
    // language's oldest wart, and pages depend on both halves.
    ExpectEval("new Array(3).length", "3");
    ExpectEval("new Array(1, 2).length", "2");
    ExpectEval("try { new Array(-1) } catch (e) { e.name }", "RangeError");
    ExpectEval("[Array.isArray([]), Array.isArray({})].join(' ')", "true false");
    ExpectEval("Array.of(1, 2).join('')", "12");
    ExpectEval("Array.from('abc').join('')", "abc");
    ExpectEval("Array.from(new Set([1, 2])).join('')", "12");
    // An array-like with a length, which is the other half of `from`.
    ExpectEval("Array.from({ length: 3 }, (_, i) => i).join('')", "012");
  });

  AddTest(tests, "JsInterpreter/ReduceNeedsSomethingToStartFrom", [] {
    ExpectEval("[1, 2, 3].reduce((s, x) => s + x)", "6");
    ExpectEval("[1, 2, 3].reduce((s, x) => s + x, 10)", "16");
    ExpectEval("[1, 2, 3].reduceRight((s, x) => s + x, '')", "321");
    ExpectEval("[].reduce((s, x) => s + x, 0)", "0");
    // The case that catches `[].reduce(f)`: a TypeError, not undefined.
    ExpectEval("try { [].reduce((s, x) => s + x) } catch (e) { e.name }", "TypeError");
  });

  // --- Object statics --------------------------------------------------------

  AddTest(tests, "JsInterpreter/ObjectStaticsAgreeOnWhatOwnKeysAre", [] {
    ExpectEval("JSON.stringify(Object.entries({ a: 1, b: 2 }))", "[[\"a\",1],[\"b\",2]]");
    ExpectEval("Object.keys({ a: 1, b: 2 }).join('')", "ab");
    ExpectEval("Object.values({ a: 1, b: 2 }).join('')", "12");
    ExpectEval("JSON.stringify(Object.fromEntries([['x', 1]]))", "{\"x\":1}");
    // The call this method mostly exists for.
    ExpectEval("JSON.stringify(Object.fromEntries(new Map([['m', 9]])))", "{\"m\":9}");
    ExpectEval("JSON.stringify(Object.assign({}, { a: 1 }, { b: 2 }))", "{\"a\":1,\"b\":2}");
    ExpectEval("[Object.hasOwn({ a: 1 }, 'a'), Object.hasOwn({}, 'a')].join(' ')", "true false");
    ExpectEval("Object.getPrototypeOf([]) === Array.prototype", "true");
  });

  AddTest(tests, "JsInterpreter/FreezingActuallyFreezes", [] {
    // A freeze that reported success and changed nothing would be worse than
    // not having one: a page uses it to protect state it then assumes is
    // unchanged.
    ExpectEval("const o = Object.freeze({ v: 1 }); o.v = 99; o.v", "1");
    ExpectEval("const o = Object.freeze({ v: 1 }); o.w = 2; typeof o.w", "undefined");
    ExpectEval("const o = Object.freeze({ v: 1 }); delete o.v; o.v", "1");
    // Through a builtin too, which does not go via property assignment.
    ExpectEval("const a = Object.freeze([1, 2]); a.push(3); a.length", "2");
    ExpectEval("[Object.isFrozen(Object.freeze({})), Object.isFrozen({})].join(' ')",
               "true false");
    // A primitive is frozen by definition.
    ExpectEval("Object.isFrozen(1)", "true");
  });

  // --- Syntax a real page uses -----------------------------------------------

  AddTest(tests, "JsInterpreter/AnObjectLiteralCanDefineAccessors", [] {
    ExpectEval("({ get v(){ return 7 } }).v", "7");
    ExpectEval("const o = { set v(x){ this.w = x } }; o.v = 3; o.w", "3");
    // Both halves of one property, not the second replacing the first.
    ExpectEval("const o = { get v(){ return this.n }, set v(x){ this.n = x * 2 } }; "
               "o.v = 4; o.v",
               "8");
    ExpectEval("const k = 'dyn'; ({ get [k](){ return 'c' } }).dyn", "c");
    // `get` and `set` are ordinary identifiers, so a property named either
    // still has to work -- which is the only reason detecting an accessor
    // needs a token of lookahead.
    ExpectEval("const o = { get: 1, set: 2 }; o.get + o.set", "3");
    ExpectEval("const o = { get(){ return 5 } }; o.get()", "5");
  });

  AddTest(tests, "JsInterpreter/NewTakesASpreadLikeAnyOtherCall", [] {
    ExpectEval("class P { constructor(a, b){ this.s = a + b } } new P(...[1, 2]).s", "3");
    // Any iterable, since the spread runs the protocol.
    ExpectEval("class P { constructor(a, b){ this.s = a + b } } new P(...new Set([3, 4])).s", "7");
    ExpectEval("class P { constructor(a, b, c){ this.s = '' + a + b + c } } "
               "new P(0, ...[1, 2]).s",
               "012");
  });

  AddTest(tests, "JsInterpreter/ExponentAssignsLikeEveryOtherOperator", [] {
    ExpectEval("let n = 2; n **= 3; n", "8");
    ExpectEval("let n = 2; n **= 0; n", "1");
  });

  // --- The builtins a probe found missing ------------------------------------

  AddTest(tests, "JsInterpreter/JsonParsesWhatItStringifies", [] {
    ExpectEval("JSON.parse('{\"a\":[1,2,{\"b\":\"c\"}]}').a[2].b", "c");
    ExpectEval("JSON.stringify(JSON.parse('{\"x\":1,\"y\":[true,null]}'))",
               "{\"x\":1,\"y\":[true,null]}");
    ExpectEval("typeof JSON.parse('null')", "object");
    ExpectEval("JSON.parse('-1.5e2')", "-150");
    ExpectEval("JSON.parse('[]').length", "0");
  });

  AddTest(tests, "JsInterpreter/JsonRefusesWhatIsNotJson", [] {
    // Every one of these is accepted by a lenient parser and is a different
    // document from what was sent. Trailing content matters most: accepting
    // the prefix is how a truncated response gets treated as a whole one.
    const auto refuses = [](const char* text) {
      return "SyntaxError" ==
             Eval(std::string("try { JSON.parse('") + text + "') } catch (e) { e.name }");
    };
    Expect(refuses("{"), "an unterminated object");
    Expect(refuses("{\"a\":1}x"), "trailing content");
    Expect(refuses("[1,]"), "a trailing comma");
    Expect(refuses("+1"), "a leading plus");
    // Depth is bounded, so a wall of brackets is an error rather than a
    // stack overflow.
    ExpectEval("let deep = '1'; for (let i = 0; i < 500; i++) deep = '[' + deep + ']'; "
               "try { JSON.parse(deep) } catch (e) { e.name }",
               "SyntaxError");
  });

  AddTest(tests, "JsInterpreter/TheUriFunctionsDifferInWhatTheyKeep", [] {
    // The whole reason both exist: encodeURI keeps the punctuation that
    // separates the parts of a URL, encodeURIComponent escapes it so a value
    // cannot become a separator.
    ExpectEval("encodeURIComponent('a b&c=d/e')", "a%20b%26c%3Dd%2Fe");
    ExpectEval("encodeURI('http://x/a b?c=d')", "http://x/a%20b?c=d");
    ExpectEval("decodeURIComponent('a%20b%26c')", "a b&c");
    ExpectEval("decodeURI('a%20b%2Fc')", "a b%2Fc");
    ExpectEval("try { decodeURIComponent('%zz') } catch (e) { e.name }", "URIError");
  });

  AddTest(tests, "JsInterpreter/NumbersHaveMethodsWithoutBeingBoxed", [] {
    ExpectEval("(1.5).toFixed(2)", "1.50");
    ExpectEval("(255).toString(16)", "ff");
    ExpectEval("(255).toString(2)", "11111111");
    ExpectEval("(-10).toString(36)", "-a");
    ExpectEval("try { (1).toString(1) } catch (e) { e.name }", "RangeError");
    // The static predicates do not convert, which is the whole difference from
    // the global ones: `isNaN('x')` is true and `Number.isNaN('x')` is false.
    ExpectEval("[Number.isNaN('x'), isNaN('x')].join(' ')", "false true");
    ExpectEval("[Number.isInteger(4), Number.isInteger(4.5)].join(' ')", "true false");
    ExpectEval("Number.MAX_SAFE_INTEGER", "9007199254740991");
  });

  AddTest(tests, "JsInterpreter/MathAndTheClock", [] {
    ExpectEval("[Math.trunc(4.7), Math.sign(-3), Math.log2(8), Math.hypot(3, 4)].join(' ')",
               "4 -1 3 5");
    // NaN has no sign and both zeros keep theirs, which `v > 0 ? 1 : -1` gets
    // wrong for all three.
    ExpectEval("[Math.sign(0), Math.sign(NaN)].join(' ')", "0 NaN");
    ExpectEval("const r = Math.random(); typeof r === 'number' && r >= 0 && r < 1", "true");
    // Different draws, which is the property a shuffle depends on. Two hundred
    // is far past any collision a working generator would produce.
    ExpectEval("const seen = new Set(); for (let i = 0; i < 200; i++) seen.add(Math.random()); "
               "seen.size",
               "200");
    ExpectEval("new Date(0).toISOString()", "1970-01-01T00:00:00Z");
    ExpectEval("new Date(0).getTime()", "0");
    // An unparsed date is an honest NaN rather than a wrong instant.
    ExpectEval("new Date('the third of never').getTime()", "NaN");
    ExpectEval("typeof Date.now()", "number");
  });

  AddTest(tests, "JsInterpreter/PropertiesCanBeDefinedRatherThanAssigned", [] {
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', { value: 1 }); o.x", "1");
    ExpectEval("const o = {}; Object.defineProperty(o, 'g', { get(){ return 4 } }); o.g", "4");
    ExpectEval("let taken = 0; const o = {}; "
               "Object.defineProperty(o, 's', { set(v){ taken = v } }); o.s = 9; taken",
               "9");
    ExpectEval("const o = Object.defineProperties({}, { a: { value: 1 }, b: { value: 2 } }); "
               "o.a + o.b",
               "3");
    ExpectEval("Object.getOwnPropertyDescriptor({ a: 1 }, 'a').value", "1");
    ExpectEval("typeof Object.getOwnPropertyDescriptor({}, 'a')", "undefined");
    ExpectEval("Object.getOwnPropertyNames({ a: 1, b: 2 }).join(',')", "a,b");
    // An array's indices live in element storage rather than the property map,
    // so they have to be listed separately or they go missing.
    ExpectEval("Object.getOwnPropertyNames([7, 8]).join(',')", "0,1,length");
  });

  AddTest(tests, "JsInterpreter/ObjectPrototypeHasItsOwnMethods", [] {
    ExpectEval("[({ a: 1 }).hasOwnProperty('a'), ({}).hasOwnProperty('a')].join(' ')",
               "true false");
    // The `[object Kind]` form, which is how a page tells an array from a
    // plain object without trusting `instanceof`.
    ExpectEval("Object.prototype.toString.call([])", "[object Array]");
    ExpectEval("Object.prototype.toString.call({})", "[object Object]");
    ExpectEval("Object.prototype.toString.call(/x/)", "[object RegExp]");
    ExpectEval("Object.prototype.toString.call(() => 1)", "[object Function]");
    // Every built-in prototype chains to Object.prototype, which is what makes
    // any of this resolve on an array or a number at all.
    ExpectEval("Array.prototype.isPrototypeOf([])", "true");
    ExpectEval("Object.prototype.isPrototypeOf([])", "true");
    ExpectEval("[].hasOwnProperty(0)", "false");
  });

  AddTest(tests, "JsInterpreter/AnIteratorIsItselfIterable", [] {
    // The bug this test was written after finding: the built-in iterator had a
    // comment saying it was iterable and no code that made it so, which is
    // invisible until something spreads the result of calling one.
    ExpectEval("[...['a', 'b'].values()].join(',')", "a,b");
    ExpectEval("[...['a', 'b'].keys()].join(',')", "0,1");
    ExpectEval("JSON.stringify([...['a'].entries()])", "[[0,\"a\"]]");
    ExpectEval("[...[1, 2][Symbol.iterator]()].length", "2");
    ExpectEval("let out = ''; for (const c of 'ab'[Symbol.iterator]()) out += c; out", "ab");
    // `values` is the same function the protocol hook is, so the two cannot
    // disagree about what iterating an array means.
    ExpectEval("[].values === [][Symbol.iterator]", "true");
  });

  AddTest(tests, "JsInterpreter/ATaggedTemplateSeesItsPiecesApart", [] {
    // The whole point of the form: the tag receives the literal chunks and the
    // substituted *values* separately, so a library can escape an
    // interpolation it did not write.
    ExpectEval("const t = (s, ...v) => s.join('|') + ' :: ' + v.join(','); t`a${1}b${2}c`",
               "a|b|c :: 1,2");
    ExpectEval("const t = (s, ...v) => s.length + ' ' + v.length; t`plain`", "1 0");
    ExpectEval("const raw = (s) => s.raw[0]; raw`hello`", "hello");
    // A method tag keeps its receiver.
    ExpectEval("const o = { n: 'o', tag(s){ return this.n + s[0] } }; o.tag`X`", "oX");
    ExpectEval("try { const bad = 5; bad`x` } catch (e) { e.name }", "TypeError");
  });

  AddTest(tests, "JsInterpreter/ReflectNamesWhatTheLanguageDoesImplicitly", [] {
    ExpectEval("Reflect.get({ a: 1 }, 'a')", "1");
    ExpectEval("const o = {}; Reflect.set(o, 'b', 2); o.b", "2");
    // `has` is `in`, which walks the prototype chain -- unlike
    // hasOwnProperty, and keeping the two straight is why both exist.
    ExpectEval("[Reflect.has({ a: 1 }, 'a'), Reflect.has({}, 'toString')].join(' ')",
               "true true");
    ExpectEval("const o = { a: 1 }; Reflect.deleteProperty(o, 'a'); typeof o.a", "undefined");
    ExpectEval("Reflect.apply(function(x, y){ return this.n + x + y }, { n: 10 }, [1, 2])", "13");
    ExpectEval("Reflect.ownKeys({ x: 1, y: 2 }).join(',')", "x,y");
    ExpectEval("Reflect.getPrototypeOf([]) === Array.prototype", "true");
  });

  AddTest(tests, "JsInterpreter/WeakCollectionsHoldKeysThatAreObjects", [] {
    ExpectEval("const m = new WeakMap(); const k = {}; m.set(k, 'v'); m.get(k)", "v");
    ExpectEval("const m = new WeakMap(); const k = {}; m.set(k, 1); "
               "[m.has(k), m.has({})].join(' ')",
               "true false");
    ExpectEval("const m = new WeakMap(); const k = {}; m.set(k, 1); "
               "[m.delete(k), m.has(k)].join(' ')",
               "true false");
    ExpectEval("const k = {}; new WeakMap([[k, 1]]).get(k)", "1");
    ExpectEval("const s = new WeakSet(); const k = {}; s.add(k); [s.has(k), s.has({})].join(' ')",
               "true false");
    // A primitive has no identity to be weak about -- there is nothing for the
    // collector to notice the death of -- so it is a TypeError rather than a
    // key that never collects.
    ExpectEval("try { new WeakMap().set(1, 2) } catch (e) { e.name }", "TypeError");
    // No size, no iteration, no clear. Every one of those would let a page
    // observe *when* a collection ran, which is the observation weak
    // references exist to withhold.
    ExpectEval("[typeof new WeakMap().size, typeof new WeakMap().forEach].join(' ')",
               "undefined undefined");
  });

  AddTest(tests, "JsInterpreter/AWeakEntryGoesWhenItsKeyDoes", [] {
    // The property the whole design is for, and the only one a test can tell
    // apart from a Map: an entry whose key is unreachable stops holding its
    // value alive. Measured on the heap rather than through the language,
    // because the language deliberately offers no way to ask.
    Interpreter interpreter;
    const Result result = interpreter.Run(R"(
      const table = new WeakMap();
      const held = {};
      table.set(held, { kept: true });
      for (let i = 0; i < 5000; i++) {
        // The key goes out of scope on the next iteration, so nothing but the
        // table refers to it -- and the table must not be what keeps it.
        table.set({}, { i, padding: [1, 2, 3, 4] });
      }
      table.get(held).kept
    )");
    Expect(!result.IsAbrupt(), "the program ran: " + js::ToString(result.value));
    ExpectEqString(js::ToString(result.value), "true", "the held entry survived");
    // Five thousand dead keys, each with a value holding an array. If the
    // table held them, the heap would still have tens of thousands of objects.
    Expect(interpreter.GetHeap().ObjectCount() < 5000,
           "the dead entries were collected, leaving " +
               std::to_string(interpreter.GetHeap().ObjectCount()) + " objects");
  });

  AddTest(tests, "JsInterpreter/AProxyWithNoTrapsIsItsTarget", [] {
    // The property that makes every other trap optional: a handler that
    // defines nothing has to be invisible.
    ExpectEval("const t = { a: 1 }; const p = new Proxy(t, {}); p.a", "1");
    ExpectEval("const t = { a: 1 }; const p = new Proxy(t, {}); 'a' in p", "true");
    ExpectEval("const t = { a: 1 }; const p = new Proxy(t, {}); typeof p.missing", "undefined");
    // A write goes through to the target, which is what makes a pass-through
    // proxy usable as the object itself.
    ExpectEval("const t = {}; const p = new Proxy(t, {}); p.b = 2; t.b", "2");
    ExpectEval("try { new Proxy(1, {}) } catch (e) { e.name }", "TypeError");
  });

  AddTest(tests, "JsInterpreter/AProxysTrapsSeeEveryOperation", [] {
    // What reactive frameworks are built on: every read and write is
    // observable, without the object having to be rewritten to announce them.
    ExpectEval("const reads = []; "
               "const p = new Proxy({ x: 5 }, { get(t, k){ reads.push(k); return t[k] } }); "
               "p.x; p.x; reads.join(',')",
               "x,x");
    ExpectEval("const p = new Proxy({}, { set(t, k, v){ t[k] = v * 10; return true } }); "
               "p.y = 3; p.y",
               "30");
    ExpectEval("const p = new Proxy({ x: 1 }, { has(t, k){ return k === 'magic' || k in t } }); "
               "['magic' in p, 'x' in p, 'nope' in p].join(' ')",
               "true true false");
    // The key reaches the trap as what it is. A symbol key arriving as text
    // would be a name the page could write out, which is what symbols exist
    // not to be.
    ExpectEval("const p = new Proxy({}, { get(t, k){ return typeof k } }); "
               "[p[Symbol('s')], p.name].join(' ')",
               "symbol string");
    // The receiver is the proxy, so a getter reached through it sees the proxy
    // as `this` -- which is how a computed property's read is noticed.
    ExpectEval("const p = new Proxy({}, { get(t, k, r){ return r === p } }); p.anything", "true");
  });

  AddTest(tests, "JsInterpreter/AFunctionOutlivesTheScriptThatDefinedIt", [] {
    // A function object points at its parameters and its body in the parse
    // tree -- the tree *is* the code -- so the interpreter has to keep every
    // program it has run. Without that, a callback registered by one script
    // and called after it finished reads freed memory, and *every* callback is
    // one of those: an event listener, a promise reaction, a timer.
    //
    // Nothing reached it until something invoked a listener after its script
    // was over, which is why this test exists at the language level rather
    // than only where it was found.
    Interpreter interpreter;
    const Result defined = interpreter.Run("globalThis.later = (a, b) => a + b + 'ok';");
    Expect(!defined.IsAbrupt(), "the first script ran: " + js::ToString(defined.value));
    const Result called = interpreter.Run("later(1, 2)");
    Expect(!called.IsAbrupt(), "the second script ran: " + js::ToString(called.value));
    ExpectEqString(js::ToString(called.value), "3ok",
                   "the function still had its body a script later");

    // And through a collection, which is the other way a body could go.
    const Result survived = interpreter.Run(
        "let sink = null;"
        "for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }"
        "later(4, 5)");
    Expect(!survived.IsAbrupt(), "and after a collection: " + js::ToString(survived.value));
    ExpectEqString(js::ToString(survived.value), "9ok", "still callable");
  });

  AddTest(tests, "JsInterpreter/StringMethodsCoexistWithLengthAndIndexing", [] {
    // `length` and `[i]` predate the prototype and still win over it, which is
    // the ordering GetProperty has to preserve.
    ExpectEval("'abc'.length", "3");
    ExpectEval("'abc'[1]", "b");
    ExpectEval("typeof 'abc'[9]", "undefined");
    ExpectEval("'abc'.slice(1).length", "2");
    // A number is converted before the method runs -- template literals and
    // `+` produce strings from anything, so methods have to accept what they
    // produce.
    ExpectEval("`${12345}`.slice(1, 3)", "23");
    ExpectEval("(1 + '').padStart(3, '0')", "001");
  });
}

}  // namespace microbrowser::tests
