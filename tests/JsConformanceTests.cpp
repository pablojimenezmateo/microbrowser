#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Interpreter.h"

// The language as a page uses it, rather than as the engine grew it.
//
// JsInterpreterTests is organised the way the engine was built -- one group per
// feature as it landed. This file is organised the other way round: it is the
// audit in `docs/js-conformance-roadmap.md` turned into assertions, so that
// each line is a thing a real page does and the file as a whole says how much
// of the language is actually there.
//
// Everything here runs on whichever engine took the program, like the rest of
// the suite. What makes these worth having separately is that they were each
// written from an observed *wrong answer*: `[] + {}` really was NaN, `'é'
// .length` really was 2, and a `for (let i ...)` loop really did close over one
// binding. A test that was written before the bug is a guess; these are not.

namespace microbrowser::tests {

using js::Completion;
using js::Interpreter;
using js::Result;

namespace {

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

}  // namespace

void RegisterJsConformanceTests(std::vector<TestCase>& tests) {
  // --- ToPrimitive ----------------------------------------------------------
  //
  // The conversions that can run script. Every line here answered NaN or
  // "[object Object]" before, because the conversions were pure functions with
  // no interpreter to call `valueOf` with.

  AddTest(tests, "JsConformance/AnObjectInArithmeticRunsItsOwnConversion", [] {
    ExpectEval("[] + {}", "[object Object]");
    ExpectEval("+[]", "0");
    ExpectEval("+[5]", "5");
    ExpectEval("1 + []", "1");
    ExpectEval("'x' + [1,2]", "x1,2");
    ExpectEval("({valueOf(){return 7}}) * 2", "14");
    ExpectEval("({toString(){return '3'}}) - 1", "2");
  });

  AddTest(tests, "JsConformance/TheHintDecidesWhichConversionIsTriedFirst", [] {
    // valueOf first for arithmetic, toString first for a string context. An
    // object with both is the only way to tell, which is why the test has one.
    ExpectEval("const o = {valueOf(){return 1}, toString(){return 'two'}}; `${o}`", "two");
    ExpectEval("const o = {valueOf(){return 1}, toString(){return 'two'}}; o - 0", "1");
    ExpectEval("const o = {valueOf(){return 1}, toString(){return 'two'}}; o + ''", "1");
  });

  AddTest(tests, "JsConformance/SymbolToPrimitiveOverridesBoth", [] {
    ExpectEval(
        "const o = {[Symbol.toPrimitive](h){ return h }};"
        "`${o}` + '|' + (o - 0 === o - 0) + '|' + (o + '')",
        "string|false|default");
  });

  AddTest(tests, "JsConformance/AConversionThatThrowsPropagates", [] {
    ExpectEval("try { ({valueOf(){ throw 'x' }}) + 1 } catch (e) { e }", "x");
    // Nothing primitive to reach: a null prototype means no valueOf and no
    // toString, which is the spec's TypeError rather than a crash.
    ExpectEval(
        "try { Object.create(null) + 1 } catch (e) { e instanceof TypeError }", "true");
  });

  AddTest(tests, "JsConformance/LooseEqualityCoercesAnObject", [] {
    ExpectEval("[1] == 1", "true");
    ExpectEval("[] == false", "true");
    ExpectEval("({}) == '[object Object]'", "true");
    // Two objects still compare by identity: the type test settles it before
    // any conversion runs.
    ExpectEval("({}) == ({})", "false");
    ExpectEval("null == 0", "false");
  });

  AddTest(tests, "JsConformance/RelationalOperatorsConvertWithTheNumberHint", [] {
    // Both become strings, so this compares as text -- which is why it is
    // false rather than true.
    ExpectEval("[2] < [11]", "false");
    ExpectEval("2 < 11", "true");
    ExpectEval("({valueOf(){return 5}}) > 4", "true");
  });

  AddTest(tests, "JsConformance/ArrayToStringIsJoinRatherThanTheObjectForm", [] {
    ExpectEval("[1,2].toString()", "1,2");
    ExpectEval("[].toString()", "");
    ExpectEval("[null, 1].toString()", ",1");
    ExpectEval("String([1,[2,3]])", "1,2,3");
  });

  AddTest(tests, "JsConformance/ABooleanHasAPrototype", [] {
    ExpectEval("true.toString()", "true");
    ExpectEval("false.valueOf()", "false");
    ExpectEval("`${true}`", "true");
  });

  AddTest(tests, "JsConformance/ObjectToStringNamesNullAndReadsTheTag", [] {
    ExpectEval("Object.prototype.toString.call(null)", "[object Null]");
    ExpectEval("Object.prototype.toString.call(undefined)", "[object Undefined]");
    ExpectEval("Object.prototype.toString.call(1)", "[object Number]");
    ExpectEval("Object.prototype.toString.call('a')", "[object String]");
    ExpectEval("Object.prototype.toString.call([])", "[object Array]");
    ExpectEval("Object.prototype.toString.call({[Symbol.toStringTag]:'X'})", "[object X]");
  });

  AddTest(tests, "JsConformance/InstanceofConsultsSymbolHasInstance", [] {
    ExpectEval("class C { static [Symbol.hasInstance](x){ return x === 42 } } 42 instanceof C",
               "true");
    // The trap wins over the prototype walk, so an actual instance says no.
    ExpectEval("class C { static [Symbol.hasInstance](){ return false } } new C() instanceof C",
               "false");
  });

  AddTest(tests, "JsConformance/AComputedKeyConvertsThroughToPrimitive", [] {
    ExpectEval("const o = {a:1}; o[{toString(){return 'a'}}]", "1");
    ExpectEval("const o = {}; o[[1,2]] = 5; o['1,2']", "5");
  });

  AddTest(tests, "JsConformance/AUnaryOperatorRunsTheConversionToo", [] {
    ExpectEval("-({valueOf(){return 3}})", "-3");
    ExpectEval("~({valueOf(){return 0}})", "-1");
    ExpectEval("let o = {valueOf(){return 4}}; o++; o", "5");
  });
}

}  // namespace microbrowser::tests
