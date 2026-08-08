#include <string>
#include <string_view>
#include <vector>

#include <map>

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

// Runs a module graph given as a map from specifier to source, entered at
// "main". The resolver is the whole seam: the engine never looks at a file,
// and a test never needs one.
std::string EvalModules(const std::map<std::string, std::string>& files) {
  Interpreter interpreter;
  interpreter.SetModuleResolver(
      [&files](std::string_view specifier, std::string_view, std::string& resolved,
               std::string& source) {
        const auto found = files.find(std::string(specifier));
        if (found == files.end()) {
          return false;
        }
        resolved = found->first;
        source = found->second;
        return true;
      });
  const auto entry = files.find("main");
  if (entry == files.end()) {
    return "no entry";
  }
  const Result result = interpreter.RunModule(entry->second, "main");
  if (result.completion == Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  std::string out;
  for (const std::string& line : interpreter.ConsoleOutput()) {
    if (!out.empty()) {
      out += "|";
    }
    out += line;
  }
  return out;
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

  // --- Optional chaining ----------------------------------------------------
  //
  // Two things were wrong: `a?.[k]` was a syntax error, because computed and
  // optional were treated as alternatives rather than as bits; and only the
  // link short-circuited rather than the chain, so `(null)?.a.b` threw.

  AddTest(tests, "JsConformance/AnOptionalChainGivesUpAsAWhole", [] {
    ExpectEval("const o = {n: null}; String(o.n?.a.b.c)", "undefined");
    ExpectEval("String((null)?.a.b.c.d)", "undefined");
    ExpectEval("const o = {a:{b:{c:5}}}; o?.a.b.c", "5");
    // A nullish link that is not optional still throws -- `a?.b` being
    // undefined does not make `.c` on it legal.
    ExpectEval("const o = {}; try { o?.a.b } catch (e) { e instanceof TypeError }", "true");
  });

  AddTest(tests, "JsConformance/AnOptionalComputedAccessParses", [] {
    ExpectEval("const o = {}; String(o.z?.[0])", "undefined");
    ExpectEval("const o = {a:{b:{c:5}}}; o?.['a']?.['b']?.['c']", "5");
    ExpectEval("const o = {a:[1,2]}; o?.a?.[1]", "2");
  });

  AddTest(tests, "JsConformance/AnOptionalCallGivesUpTheChainToo", [] {
    ExpectEval("const o = {}; String(o.miss?.())", "undefined");
    ExpectEval("const o = {n:null}; String(o.n?.x(1,2).y)", "undefined");
    ExpectEval("const o = {a:{b:{c:5}}, f(){ return this.a }}; o.f?.().b.c", "5");
  });

  AddTest(tests, "JsConformance/ThePartsOfAChainAreEvaluatedOnce", [] {
    ExpectEval("let n = 0; function side(){ n++; return null }"
               "String(side()?.a.b.c) + ':' + n",
               "undefined:1");
  });

  AddTest(tests, "JsConformance/AChainInsideAnArgumentIsItsOwnChain", [] {
    // The inner chain's short-circuit belongs to the inner chain: the outer
    // call still happens, with undefined as its argument.
    ExpectEval("const o = {n:null}; function g(x){ return x === undefined ? 'inner' : 'outer' }"
               "g(o.n?.a.b)",
               "inner");
    ExpectEval("const o = {a:{}, n:null}; String(o.a?.[o.n?.k])", "undefined");
  });

  // --- Destructuring --------------------------------------------------------

  AddTest(tests, "JsConformance/AnObjectPatternTakesAComputedKey", [] {
    ExpectEval("const k = 'q'; const {[k]: v} = {q: 8}; v", "8");
    // Silently undefined before: the pattern read the *written* name, which
    // for a computed key is empty.
    ExpectEval("const k = 'a'; const {[k]: v = 5} = {}; v", "5");
  });

  AddTest(tests, "JsConformance/AnObjectPatternTakesARest", [] {
    ExpectEval("const {a, ...r} = {a:1, b:2, c:3}; a + ':' + JSON.stringify(r)",
               "1:{\"b\":2,\"c\":3}");
    ExpectEval("const k = 'z'; const {[k]: v, ...r} = {z:1, m:2}; v + ':' + JSON.stringify(r)",
               "1:{\"m\":2}");
    ExpectEval("let g, h; ({g, ...h} = {g:1, k:2}); g + ':' + JSON.stringify(h)",
               "1:{\"k\":2}");
    ExpectEval("function f({a = 1, ...o} = {}) { return a + ':' + JSON.stringify(o) } f({a:5,b:6})",
               "5:{\"b\":6}");
  });

  AddTest(tests, "JsConformance/ANestedPatternCanHaveADefault", [] {
    // `{c} = {c:9}` in a binding position is a default, not an assignment.
    // The parser reads a pattern with the expression grammar, so only the
    // consumer can tell -- and it used to answer "invalid assignment target".
    ExpectEval("const {b: {c} = {c: 9}} = {}; c", "9");
    ExpectEval("const {p: {q} = {q: 7}} = {p: {q: 3}}; q", "3");
    ExpectEval("const [x = 1, [y = 2] = []] = []; x + ':' + y", "1:2");
  });

  AddTest(tests, "JsConformance/DestructuringNullSaysSo", [] {
    ExpectEval("try { const {a} = null } catch (e) { e instanceof TypeError }", "true");
  });

  // --- new.target, static blocks, the brand check ---------------------------

  AddTest(tests, "JsConformance/NewTargetSaysWhetherTheCallWasAConstruction", [] {
    ExpectEval("function F(){ return new.target === F } String(F())", "false");
    ExpectEval("class G { constructor(){ if (!new.target) throw new TypeError('needs new') } }"
               "new G() instanceof G",
               "true");
    ExpectEval("class G { constructor(){ if (!new.target) throw new TypeError('x') } }"
               "try { G() } catch (e) { e.message }",
               "x");
  });

  AddTest(tests, "JsConformance/NewTargetIsTheMostDerivedConstructor", [] {
    ExpectEval("class A { constructor(){ this.t = new.target.name } } class B extends A {}"
               "new A().t + ':' + new B().t",
               "A:B");
  });

  AddTest(tests, "JsConformance/AnArrowReadsTheNewTargetAroundIt", [] {
    // Like `this`: an arrow declares none of its own, so the walk out finds
    // the enclosing function's.
    ExpectEval("function H(){ this.a = (() => new.target)() === H } new H().a", "true");
  });

  AddTest(tests, "JsConformance/AStaticBlockRunsInBodyOrder", [] {
    ExpectEval("class C { static x = 1; static { C.y = C.x + 1 } static z = 3 } "
               "C.x + ':' + C.y + ':' + C.z",
               "1:2:3");
    // `this` inside one is the class.
    ExpectEval("class C { static { C.n = this.name } } C.n", "C");
  });

  AddTest(tests, "JsConformance/ThePrivateBrandCheckIsAnOperator", [] {
    ExpectEval("class P { #v = 1; static has(o){ return #v in o } }"
               "P.has(new P()) + ':' + P.has({})",
               "true:false");
  });

  // --- Per-iteration bindings ----------------------------------------------
  //
  // `for (let i = ...)` makes one binding per pass, not one for the loop. The
  // engine answered 3,3,3 before, which is what `var` means -- and the whole
  // reason `let` was added to the language.

  AddTest(tests, "JsConformance/ALetInALoopHeadIsOneBindingPerIteration", [] {
    ExpectEval("const fs = []; for (let i = 0; i < 3; i++) fs.push(() => i);"
               "fs.map(f => f()).join()",
               "0,1,2");
    // `var` is the contrast, and it still behaves the old way -- one binding
    // for the whole loop, so every closure sees what it ended on.
    ExpectEval("const fs = []; for (var i = 0; i < 3; i++) fs.push(() => i);"
               "fs.map(f => f()).join()",
               "3,3,3");
  });

  AddTest(tests, "JsConformance/AContinueStillEndsTheIterationItIsIn", [] {
    ExpectEval("const fs = []; for (let i = 0; i < 4; i++) { if (i === 1) continue;"
               "fs.push(() => i) } fs.map(f => f()).join()",
               "0,2,3");
  });

  AddTest(tests, "JsConformance/ABindingInsideTheBodyIsPerIterationToo", [] {
    ExpectEval("const fs = []; for (let i = 0; i < 3; i++) { let j = i * 2;"
               "fs.push(() => `${i}:${j}`) } fs.map(f => f()).join()",
               "0:0,1:2,2:4");
    ExpectEval("const fs = []; for (const x of [1,2,3]) fs.push(() => x);"
               "fs.map(f => f()).join()",
               "1,2,3");
  });

  AddTest(tests, "JsConformance/TheLoopStillRunsTheSameNumberOfTimes", [] {
    // The copy must not disturb the loop itself: the increment writes to the
    // new binding and the test reads it, so an off-by-one here would be a
    // loop that never ends.
    ExpectEval("let s = 0; for (let i = 0; i < 5; i++) s += i; s", "10");
    ExpectEval("let n = 0; outer: for (let i = 0; i < 3; i++)"
               "{ for (let j = 0; j < 3; j++) { if (j === 1) continue outer; n++ } } n",
               "3");
  });

  // --- Date -----------------------------------------------------------------
  //
  // The calendar is computed rather than asked of the platform: `time_t`
  // cannot hold the range the language allows, and a page can name the far
  // end of it.

  AddTest(tests, "JsConformance/DateReadsAndWritesEveryUtcField", [] {
    ExpectEval("new Date(Date.UTC(2020,0,2,3,4,5,6)).toISOString()", "2020-01-02T03:04:05.006Z");
    ExpectEval("const d = new Date(Date.UTC(2020,0,2)); "
               "[d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay()].join()",
               "2020,0,2,4");
    ExpectEval("const d = new Date(0); d.setUTCFullYear(2000); d.toISOString()",
               "2000-01-01T00:00:00.000Z");
  });

  AddTest(tests, "JsConformance/DateParsesWhatItPrints", [] {
    ExpectEval("Date.parse('2020-01-02T00:00:00.000Z')", "1577923200000");
    ExpectEval("Date.parse('2020-01-02')", "1577923200000");
    ExpectEval("String(Date.parse('not a date'))", "NaN");
    // The round trips that matter: both of the forms this engine produces
    // have to come back as the same instant.
    ExpectEval("const d = new Date(Date.UTC(2020,5,15,12,0,0));"
               "new Date(d.toISOString()).getTime() === d.getTime()",
               "true");
    ExpectEval("const d = new Date(Date.UTC(2020,5,15,12,0,0));"
               "new Date(String(d)).getTime() === d.getTime()",
               "true");
  });

  AddTest(tests, "JsConformance/DateFieldsRollRatherThanFail", [] {
    // `new Date(2020, 0, 32)` is the first of February: pages add days that
    // way on purpose.
    ExpectEval("new Date(2020, 0, 32).getMonth()", "1");
    ExpectEval("new Date(2020, 12, 1).getFullYear()", "2021");
  });

  AddTest(tests, "JsConformance/AnInvalidDateIsAValueRatherThanAnError", [] {
    ExpectEval("new Date(NaN).toString()", "Invalid Date");
    ExpectEval("String(new Date(NaN).getTime())", "NaN");
    ExpectEval("JSON.stringify({d: new Date(NaN)})", "{\"d\":null}");
  });

  AddTest(tests, "JsConformance/ADatePrefersItsStringUnderTheDefaultHint", [] {
    // The one exotic conversion in the language, and the reason
    // Symbol.toPrimitive exists: `date + 1` concatenates, `date - 1` subtracts.
    ExpectEval("const d = new Date(0); typeof (d + 1)", "string");
    ExpectEval("const d = new Date(0); d - 0", "0");
  });

  AddTest(tests, "JsConformance/ADateSerializesAsItsIsoString", [] {
    ExpectEval("JSON.stringify({d: new Date(Date.UTC(2000,0,1))})",
               "{\"d\":\"2000-01-01T00:00:00.000Z\"}");
  });

  // --- Property attributes --------------------------------------------------

  AddTest(tests, "JsConformance/DefinePropertyDefaultsToNonEnumerable", [] {
    // The line every bundler emits. An engine that reported it from
    // Object.keys would put it in every loop over every module's exports.
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1});"
               "o.x + ':' + Object.keys(o).length",
               "1:0");
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1, enumerable: true});"
               "Object.keys(o).join()",
               "x");
    // getOwnPropertyNames reports it anyway, which is the whole difference
    // between the two.
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1});"
               "Object.getOwnPropertyNames(o).join()",
               "x");
  });

  AddTest(tests, "JsConformance/ANonWritablePropertyRefusesAWrite", [] {
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1}); o.x = 2; o.x", "1");
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1, writable: true});"
               "o.x = 2; o.x",
               "2");
  });

  AddTest(tests, "JsConformance/ADescriptorSaysWhatItActuallyIs", [] {
    ExpectEval("const d = Object.getOwnPropertyDescriptor({x:1}, 'x');"
               "[d.value, d.writable, d.enumerable, d.configurable].join()",
               "1,true,true,true");
    ExpectEval("const o = {}; Object.defineProperty(o, 'x', {value: 1});"
               "const d = Object.getOwnPropertyDescriptor(o, 'x');"
               "[d.writable, d.enumerable, d.configurable].join()",
               "false,false,false");
  });

  AddTest(tests, "JsConformance/ObjectCreateTakesADescriptorMap", [] {
    ExpectEval("const o = Object.create({x:1}, {y: {value: 2, enumerable: true}});"
               "o.x + ':' + o.y + ':' + Object.keys(o).join()",
               "1:2:y");
  });

  AddTest(tests, "JsConformance/SealAndPreventExtensionsAreNotFreeze", [] {
    ExpectEval("const o = Object.seal({a:1}); o.a = 2; delete o.a; o.b = 3;"
               "o.a + ':' + ('b' in o) + ':' + Object.isSealed(o)",
               "2:false:true");
    ExpectEval("const o = Object.preventExtensions({a:1}); o.b = 1; delete o.a;"
               "Object.isExtensible(o) + ':' + ('a' in o) + ':' + ('b' in o)",
               "false:false:false");
  });

  // --- Subclassing a builtin -------------------------------------------------

  AddTest(tests, "JsConformance/AClassCanExtendError", [] {
    ExpectEval("class E extends Error { constructor(m){ super(m); this.name = 'E' } }"
               "const e = new E('boom');"
               "[e instanceof Error, e instanceof E, e.message, String(e)].join('|')",
               "true|true|boom|E: boom");
  });

  AddTest(tests, "JsConformance/AClassCanExtendArray", [] {
    ExpectEval("class Stack extends Array {} const s = new Stack(); s.push(1);"
               "[s.length, s instanceof Array, Array.isArray(s)].join()",
               "1,true,true");
  });

  AddTest(tests, "JsConformance/AClassCanExtendMapAndSet", [] {
    ExpectEval("class C extends Map {} const c = new C(); c.set(1,2); c.get(1) + ':' + c.size",
               "2:1");
    ExpectEval("class S extends Set {} const s = new S([1,1,2]); s.size", "2");
  });

  // --- Proxy ----------------------------------------------------------------
  //
  // A proxy over a function is how every framework wraps one, and it was not
  // callable at all: only get, set and has existed.

  AddTest(tests, "JsConformance/AProxyOverAFunctionIsAFunction", [] {
    ExpectEval("const f = new Proxy(function(){}, {apply:(t,s,a)=>'applied:'+a.join()});"
               "typeof f + ':' + f(1,2)",
               "function:applied:1,2");
    ExpectEval("const f = new Proxy(function(){}, {construct:()=>({v:6})}); new f().v", "6");
    // No trap: the call goes straight through, which is what makes an empty
    // handler invisible.
    ExpectEval("const f = new Proxy((a,b)=>a+b, {}); f(1,2)", "3");
  });

  AddTest(tests, "JsConformance/AProxyTrapsDeletesAndEnumeration", [] {
    ExpectEval("let removed = ''; const p = new Proxy({a:1}, "
               "{deleteProperty:(t,k)=>{ removed = k; return true }});"
               "delete p.a; removed",
               "a");
    ExpectEval("const p = new Proxy({}, {ownKeys:()=>['a','b'],"
               "getOwnPropertyDescriptor:()=>({value:1,enumerable:true,configurable:true})});"
               "Object.keys(p).join()",
               "a,b");
    ExpectEval("const p = new Proxy({a:1,b:2}, {}); Object.keys(p).join()", "a,b");
    ExpectEval("const p = new Proxy({a:1,b:2}, {}); const seen = [];"
               "for (const k in p) seen.push(k); seen.join()",
               "a,b");
  });

  AddTest(tests, "JsConformance/AProxyReportsWhatItWraps", [] {
    // A feature test must not be what reveals a wrapper.
    ExpectEval("Array.isArray(new Proxy([], {}))", "true");
    ExpectEval("Object.prototype.toString.call(new Proxy([], {}))", "[object Array]");
    ExpectEval("JSON.stringify(new Proxy({m:1}, {}))", "{\"m\":1}");
  });

  // --- Typed arrays ---------------------------------------------------------

  AddTest(tests, "JsConformance/ATypedArrayStoresElementsAsBytes", [] {
    // 257 in a byte is 1: the write wraps, which is the whole point of a
    // typed array over an ordinary one.
    ExpectEval("const a = new Uint8Array(4); a[0] = 257; a[1] = 5; [...a].join()", "1,5,0,0");
    ExpectEval("new Int16Array([1,-1,32768]).join()", "1,-1,-32768");
    // The clamped kind saturates instead, which is what pixel data needs.
    ExpectEval("const c = new Uint8ClampedArray(2); c[0] = 300; c[1] = -5; c.join()", "255,0");
  });

  AddTest(tests, "JsConformance/TwoViewsOverOneBufferSeeEachOther", [] {
    ExpectEval("const b = new ArrayBuffer(8); const u1 = new Uint8Array(b);"
               "const u4 = new Uint32Array(b); u1[0] = 1; u4[0]",
               "1");
    ExpectEval("const b = new ArrayBuffer(8); const u = new Uint8Array(b);"
               "const s = u.subarray(2,4); s[0] = 9; u[2] + ':' + s.length",
               "9:2");
    ExpectEval("const b = new ArrayBuffer(4); const u = new Uint8Array(b);"
               "(u.buffer === b) + ':' + ArrayBuffer.isView(u) + ':' + ArrayBuffer.isView(b)",
               "true:true:false");
  });

  AddTest(tests, "JsConformance/ATypedArrayIsAnArrayLike", [] {
    // The generic Array.prototype methods work on one because ElementCount and
    // GetElement answer for a view -- which is what those methods are
    // specified over.
    ExpectEval("new Uint8Array([1,2,3]).map(x => x * 2).join()", "2,4,6");
    ExpectEval("new Uint8Array([1,2,3]).reduce((a,b) => a + b, 0)", "6");
    ExpectEval("new Uint8Array([1,2,3]).slice(1).join()", "2,3");
    ExpectEval("const out = []; for (const x of new Uint8Array([5,6])) out.push(x); out.join()",
               "5,6");
    ExpectEval("new Uint8Array(4).fill(3).join()", "3,3,3,3");
  });

  AddTest(tests, "JsConformance/ATypedArraySortsNumerically", [] {
    // An array sorts by string and a typed array by value, which is the one
    // place the generic method could not be shared.
    ExpectEval("new Uint8Array([10,9,1]).sort().join()", "1,9,10");
    ExpectEval("[10,9,1].sort().join()", "1,10,9");
  });

  AddTest(tests, "JsConformance/ADataViewChoosesItsByteOrder", [] {
    ExpectEval("const v = new DataView(new ArrayBuffer(8)); v.setInt32(0, 1, true);"
               "v.getInt32(0, true) + ':' + v.getInt32(0, false)",
               "1:16777216");
    ExpectEval("const v = new DataView(new ArrayBuffer(8)); v.setFloat64(0, 1.5);"
               "v.getFloat64(0)",
               "1.5");
  });

  AddTest(tests, "JsConformance/AViewIsCheckedAgainstItsBuffer", [] {
    ExpectEval("try { new Uint32Array(new ArrayBuffer(8), 0, 99) }"
               "catch (e) { e instanceof RangeError }",
               "true");
    ExpectEval("try { new Uint32Array(new ArrayBuffer(8), 3) }"
               "catch (e) { e instanceof RangeError }",
               "true");
    ExpectEval("try { new DataView(new ArrayBuffer(4)).getInt32(2) }"
               "catch (e) { e instanceof RangeError }",
               "true");
  });

  AddTest(tests, "JsConformance/ATypedArrayNamesItselfToAFeatureTest", [] {
    ExpectEval("Object.prototype.toString.call(new Uint8Array(1))", "[object Uint8Array]");
    ExpectEval("Object.prototype.toString.call(new ArrayBuffer(1))", "[object ArrayBuffer]");
    ExpectEval("Uint8Array.BYTES_PER_ELEMENT + ':' + Float64Array.BYTES_PER_ELEMENT", "1:8");
    ExpectEval("Uint8Array.from([1,2]).join() + ':' + Uint8Array.of(3,4).join()", "1,2:3,4");
  });

  // --- UTF-16 indexing ------------------------------------------------------
  //
  // A string is a sequence of UTF-16 code units and the storage is UTF-8, so
  // every index a method takes or returns crosses between the two. It used to
  // not cross at all: `'é'.length` was 2.

  AddTest(tests, "JsConformance/LengthCountsCodeUnitsRatherThanBytes", [] {
    ExpectEval("'é'.length", "1");
    ExpectEval("'日本語'.length", "3");
    // Two units for an astral character, which is what UTF-16 makes it.
    ExpectEval("'a\\u{1F600}b'.length", "4");
    ExpectEval("'abc'.length", "3");
  });

  AddTest(tests, "JsConformance/IndexingIsByCodeUnit", [] {
    ExpectEval("'héllo'[1]", "é");
    ExpectEval("'héllo'.charCodeAt(1)", "233");
    ExpectEval("'héllo'.slice(1,4)", "éll");
    ExpectEval("'héllo'.indexOf('llo')", "2");
    ExpectEval("'日本語'.at(-1)", "語");
    ExpectEval("'日本語'.split('').join('|')", "日|本|語");
  });

  AddTest(tests, "JsConformance/AnAstralCharacterIsASurrogatePair", [] {
    // The pair, split by index -- and put back together by fromCharCode,
    // which is the round trip every text-handling library performs.
    ExpectEval("const e = 'a\\u{1F600}b';"
               "[e.charCodeAt(1), e.charCodeAt(2), e.codePointAt(1), e.indexOf('b')].join()",
               "55357,56832,128512,3");
    ExpectEval("String.fromCharCode(0xD83D, 0xDE00) === '\\u{1F600}'", "true");
    // Iteration is by code *point*, which is the one place it differs from
    // indexing -- and why `[...s]` and `s.split('')` are not the same.
    ExpectEval("[...'a\\u{1F600}b'].join('|')", "a|\U0001F600|b");
    ExpectEval("[...'a\\u{1F600}b'].length + ':' + 'a\\u{1F600}b'.split('').length", "3:4");
  });

  AddTest(tests, "JsConformance/PaddingCountsCodeUnitsToo", [] {
    ExpectEval("'é'.padStart(3, 'x')", "xxé");
    ExpectEval("'é'.padEnd(3, 'x')", "éxx");
  });

  AddTest(tests, "JsConformance/AMatchIndexIsACodeUnitIndex", [] {
    ExpectEval("'日本語'.match(/本/).index", "1");
    ExpectEval("/語/.exec('日本語').index", "2");
    ExpectEval("'a\\u{1F600}b'.search(/b/)", "3");
    // `lastIndex` is read and written in the same measure.
    ExpectEval("const r = /./g; r.exec('日本語'); r.lastIndex", "1");
  });

  AddTest(tests, "JsConformance/CaseConversionReachesPastAscii", [] {
    ExpectEval("'héllo'.toUpperCase()", "HÉLLO");
    ExpectEval("'ÄÖÜ'.toLowerCase()", "äöü");
    ExpectEval("'привет'.toUpperCase()", "ПРИВЕТ");
    // A script with no case is left alone rather than mangled.
    ExpectEval("'日本'.toUpperCase()", "日本");
  });

  // --- Delegation and iterator closing --------------------------------------

  AddTest(tests, "JsConformance/YieldStarForwardsAThrowToItsDelegate", [] {
    // The inner generator's `catch` sees it, not the outer one's. Without
    // forwarding the throw stopped at the outer `yield` and the inner
    // generator never learned of it.
    ExpectEval("function* inner(){ try { yield 1 } catch (e) { yield 'caught:' + e } }"
               "function* outer(){ yield* inner() }"
               "const it = outer(); it.next(); it.throw('x').value",
               "caught:x");
  });

  AddTest(tests, "JsConformance/YieldStarForwardsAReturnToItsDelegate", [] {
    ExpectEval("let ran = false;"
               "function* inner(){ try { yield 1 } finally { ran = true } }"
               "function* outer(){ yield* inner() }"
               "const it = outer(); it.next(); const v = it.return(5).value;"
               "v + ':' + ran + ':' + it.next().done",
               "5:true:true");
  });

  AddTest(tests, "JsConformance/YieldStarPassesValuesThroughBothWays", [] {
    // `next(v)` has to reach the *inner* generator's yield, and what the inner
    // one returns has to be what the `yield*` expression is worth.
    ExpectEval("function* inner(){ const x = yield 'a'; const y = yield 'b'; return x + y }"
               "function* outer(){ const a = yield* inner(); return 'end:' + a }"
               "const it = outer();"
               "[it.next().value, it.next(10).value, it.next(20).value].join()",
               "a,b,end:30");
    ExpectEval("function* g(){ yield* [1,2]; yield 3 } [...g()].join()", "1,2,3");
  });

  AddTest(tests, "JsConformance/AThrowPastAForOfClosesTheIterator", [] {
    ExpectEval("let closed = false;"
               "const it = { [Symbol.iterator](){ return { next(){ return {value:1,done:false} },"
               "return(){ closed = true; return {done:true} } } } };"
               "try { for (const x of it) { throw 1 } } catch (e) {} closed",
               "true");
    // A generator gets its `finally` run, which is the case that matters: its
    // frame is filed and only a close drops it.
    ExpectEval("let ran = false;"
               "function* g(){ try { yield 1; yield 2 } finally { ran = true } }"
               "try { for (const x of g()) { throw 1 } } catch (e) {} ran",
               "true");
  });

  // --- What enumeration sees ------------------------------------------------
  //
  // `for...in` walks the prototype chain, which it did not. Making it do so
  // required the built-ins to become non-enumerable first -- otherwise
  // `for (const k in [])` reports every method on Array.prototype.

  AddTest(tests, "JsConformance/ForInWalksThePrototypeChain", [] {
    ExpectEval("const p = {a:1}; const o = Object.create(p); o.b = 2;"
               "const out = []; for (const k in o) out.push(k); out.join()",
               "b,a");
    // Each name once, even when a nearer object shadows a further one.
    ExpectEval("const p = {a:1}; const o = Object.create(p); o.a = 2;"
               "const out = []; for (const k in o) out.push(k); out.join()",
               "a");
  });

  AddTest(tests, "JsConformance/BuiltInsAreInvisibleToEnumeration", [] {
    ExpectEval("const out = []; for (const k in []) out.push(k); out.length", "0");
    ExpectEval("const out = []; for (const k in {}) out.push(k); out.length", "0");
    ExpectEval("const out = []; for (const k in new Map()) out.push(k); out.length", "0");
    ExpectEval("const out = []; for (const k in new Date()) out.push(k); out.length", "0");
    ExpectEval("Object.keys(Array.prototype).length", "0");
  });

  AddTest(tests, "JsConformance/AClassMemberIsNotEnumerable", [] {
    // The one place a class and an object literal differ in what enumeration
    // sees, and a difference a page relies on when it copies own keys.
    ExpectEval("class C { m(){} } Object.keys(C.prototype).length", "0");
    ExpectEval("class C { m(){} } const out = []; for (const k in new C()) out.push(k);"
               "out.length",
               "0");
    ExpectEval("const o = { m(){} }; Object.keys(o).join()", "m");
  });

  AddTest(tests, "JsConformance/APrivateFieldIsInvisible", [] {
    ExpectEval("class C { #x = 1; y = 2 } const out = [];"
               "for (const k in new C()) out.push(k); out.join()",
               "y");
    ExpectEval("class C { #x = 1; y = 2 } Object.keys(new C()).join()", "y");
    ExpectEval("class C { #x = 1; y = 2 } JSON.stringify(new C())", "{\"y\":2}");
  });

  // --- Regular expressions over code points ---------------------------------

  AddTest(tests, "JsConformance/DotUnderUMatchesAWholeCodePoint", [] {
    // Four bytes, one match. Without `/u` a `.` is one byte, which is what a
    // pattern written before ES6 expects and what still happens.
    ExpectEval("/^.$/u.test('\\u{1F600}')", "true");
    ExpectEval("/^.$/.test('\\u{1F600}')", "false");
    ExpectEval("/^.$/u.test('é')", "true");
    ExpectEval("'a\\u{1F600}b'.replace(/./gu, 'X')", "XXX");
  });

  AddTest(tests, "JsConformance/PropertyEscapesNameSetsOfCodePoints", [] {
    ExpectEval("[/\\p{L}/u.test('a'), /\\p{L}/u.test('é'), /\\p{L}/u.test('日'),"
               "/\\p{L}/u.test('1')].join()",
               "true,true,true,false");
    ExpectEval("[/^\\p{Lu}+$/u.test('ABC'), /^\\p{Ll}+$/u.test('abc')].join()", "true,true");
    ExpectEval("'a1é日'.replace(/\\p{L}/gu, 'X')", "X1XX");
    ExpectEval("/\\P{L}/u.test('1')", "true");
    ExpectEval("[/\\p{Script=Han}/u.test('日'), /\\p{Script=Han}/u.test('a')].join()",
               "true,false");
    // Without the flag it is the letter p, which is what a pattern written
    // before ES6 meant.
    ExpectEval("/\\p{L}/.test('p')", "false");
    ExpectEval("/\\p{L}/.test('p{L}')", "true");
  });

  AddTest(tests, "JsConformance/AnUnknownPropertyIsRefusedRatherThanEmpty", [] {
    // A pattern that matches nothing is a validator that accepts nothing, and
    // a page would find that as a rejected form rather than as a broken regex.
    ExpectEval("try { new RegExp('\\\\p{Nope}', 'u') } catch (e) { e instanceof SyntaxError }",
               "true");
  });

  AddTest(tests, "JsConformance/AnObjectCanStandInForAPattern", [] {
    // `Symbol.replace` and its four siblings are how a library object -- a
    // template, a tokenizer, an internationalised matcher -- is used where a
    // RegExp would be. Without them the object was stringified instead.
    ExpectEval("class R { [Symbol.replace](s, r){ return 'R:' + s + ':' + r } }"
               "'abc'.replace(new R(), 'y')",
               "R:abc:y");
    ExpectEval("class R { [Symbol.match](s){ return ['M:' + s] } }"
               "'abc'.match(new R()).join()",
               "M:abc");
    ExpectEval("class R { [Symbol.split](s){ return ['S', s] } }"
               "'abc'.split(new R()).join()",
               "S,abc");
    ExpectEval("class R { [Symbol.search](){ return 42 } } 'abc'.search(new R())", "42");
    // The ordinary forms are untouched.
    ExpectEval("'abc'.replace('b', 'X') + ':' + 'a,b'.split(',').join('|')", "aXc:a|b");
  });

  AddTest(tests, "JsConformance/AFunctionKnowsItsNameAndArity", [] {
    // Arity stops at the first default and at a rest element, which is what
    // every library that dispatches on it reads.
    ExpectEval("[(function(a,b){}).length, (function(a,b,c=1){}).length,"
               "(function(...r){}).length, ((x)=>x).length].join()",
               "2,2,0,1");
    // Named evaluation: an anonymous function takes the name of the binding
    // it is assigned to.
    ExpectEval("const f = () => {}; const g = function(){}; const C = class {};"
               "const o = { h: () => {} };"
               "[f.name, g.name, C.name, o.h.name].join()",
               "f,g,C,h");
    ExpectEval("let z; z = () => {}; z.name", "z");
    // Already named wins: `const f = function g(){}` is still `g`.
    ExpectEval("const f = function g(){}; f.name", "g");
  });

  // --- Modules --------------------------------------------------------------
  //
  // Loading is the host's job -- a specifier's meaning is a URL question -- so
  // the engine takes a resolver and does the linking. These tests are the
  // resolver being a map.

  // `this` at the top level is the global object in a script and undefined in
  // a module, and strict mode does not change either -- it changes what `this`
  // is inside a *function*. Both halves matter: the script rule is how a
  // bundle claims its namespace (`this.x = this.x || {}`) and how twenty years
  // of libraries found the global object (`(function(g){...})(this)`), and the
  // module rule is the one exception to it.
  AddTest(tests, "JsConformance/TopLevelThisIsTheGlobalInAScriptAndNotInAModule", [] {
    ExpectEval("this === globalThis", "true");
    ExpectEval("'use strict'; this === globalThis", "true");
    ExpectEval("this.claimed = this.claimed || {}; typeof globalThis.claimed", "object");
    ExpectEval("(function (g) { g.viaUmd = 7 })(this); globalThis.viaUmd", "7");
    // A bare call in non-strict code gets the global object as `this`. youtube's
    // player is `(function(g){var window=this;...})(_yt_player)` and needs it.
    ExpectEval("(function () { return this === globalThis })()", "true");
    ExpectEval("var got; (function () { got = this })(); got === globalThis", "true");
    // Strict mode still applies where it actually applies.
    ExpectEval("'use strict'; function f(){ return this } f()", "undefined");
    ExpectEval("(function () { 'use strict'; return this })()", "undefined");
    // The exception, and it has to shadow rather than simply be absent: a
    // module's scope has the global scope as its parent.
    ExpectEqString(EvalModules({{"main", "console.log(this === globalThis, this)"}}),
                   "false undefined", "a module's top-level this is undefined");
  });

  AddTest(tests, "JsConformance/AModuleImportsWhatAnotherExports", [] {
    ExpectEqString(EvalModules({
                       {"main", "import { add, PI } from 'm';"
                                "console.log(add(1,2), PI)"},
                       {"m", "export const PI = 3.14;"
                             "export function add(a,b){ return a+b }"},
                   }),
                   "3 3.14", "named imports");
    ExpectEqString(EvalModules({
                       {"main", "import d, * as all from 'm';"
                                "console.log(d, Object.keys(all).sort().join())"},
                       {"m", "export const x = 1; export default 'D'"},
                   }),
                   "D default,x", "default and namespace imports");
  });

  AddTest(tests, "JsConformance/ADependencyRunsBeforeTheModuleThatNeedsIt", [] {
    ExpectEqString(EvalModules({
                       {"main", "import 'a'; import 'b'; console.log('main')"},
                       {"a", "console.log('a')"},
                       {"b", "import 'a'; console.log('b')"},
                   }),
                   "a|b|main", "post-order, and a shared dependency runs once");
  });

  AddTest(tests, "JsConformance/AModuleRunsOnTheMachine", [] {
    // Not the tree-walker: a module whose body was walked would create
    // functions with AST bodies, and an async function among them would be
    // refused at the call. A module is where a page's real code lives.
    ExpectEqString(EvalModules({
                       {"main", "import { go } from 'm';"
                                "go().then(v => console.log('awaited', v));"
                                "function* g(){ yield 1; yield 2 }"
                                "console.log('gen', [...g()].join())"},
                       {"m", "export async function go(){ return 'value' }"},
                   }),
                   "gen 1,2|awaited value", "async and generators inside a module");
  });

  AddTest(tests, "JsConformance/ExportStarRepublishesEverythingButDefault", [] {
    ExpectEqString(EvalModules({
                       {"main", "import * as s from 'r';"
                                "console.log(Object.keys(s).sort().join())"},
                       {"r", "export * from 'm'; export { PI as TAU } from 'm'"},
                       {"m", "export const PI = 1; export default 'd'"},
                   }),
                   "PI,TAU", "no default through a star");
  });

  AddTest(tests, "JsConformance/AMissingModuleFailsAtTheImport", [] {
    ExpectEqString(EvalModules({{"main", "import 'nope'; console.log('never')"}}),
                   "throw TypeError: cannot resolve module 'nope'", "unresolved specifier");
    ExpectEqString(EvalModules({
                       {"main", "import 'boom'; console.log('never')"},
                       {"boom", "throw new Error('blew up')"},
                   }),
                   "throw Error: blew up", "a dependency that throws");
  });

  AddTest(tests, "JsConformance/ACycleIsBrokenRatherThanHung", [] {
    // `b` runs first because `a` imports it, and sees `a`'s export as
    // undefined -- the documented deviation, and not a hang.
    ExpectEqString(EvalModules({
                       {"main", "import 'a'"},
                       {"a", "import { b } from 'b'; export const a = 'A';"
                             "console.log('a sees', String(b))"},
                       {"b", "import { a } from 'a'; export const b = 'B';"
                             "console.log('b sees', String(a))"},
                   }),
                   "b sees undefined|a sees B", "a cycle terminates");
  });

  AddTest(tests, "JsConformance/ImportMetaAndDynamicImport", [] {
    ExpectEqString(EvalModules({
                       {"main", "console.log(import.meta.url);"
                                "import('m').then(m => console.log('dynamic', m.x));"
                                "import('nope').catch(e => console.log('rejected'))"},
                       {"m", "export const x = 7"},
                   }),
                   "main|dynamic 7|rejected", "import.meta and import()");
  });

  AddTest(tests, "JsConformance/AModulesNamesAreNotGlobals", [] {
    // The first thing modules were added to the language for.
    ExpectEqString(EvalModules({
                       {"main", "import 'm';"
                                "console.log(typeof globalThis.hidden)"},
                       {"m", "const hidden = 1; export {}"},
                   }),
                   "undefined", "a module's declarations stay in it");
  });

  // --- BigInt ---------------------------------------------------------------
  //
  // A *type*, not a library: `typeof 1n` is "bigint", `1n + 1` is a TypeError,
  // and every operator has a case for it. Half a type would be worse than
  // none, because a page cannot tell which half it has.

  AddTest(tests, "JsConformance/BigIntArithmeticIsExact", [] {
    ExpectEval("typeof 1n", "bigint");
    ExpectEval("const a = 10n, b = 3n;"
               "[a+b, a-b, a*b, a/b, a%b, a**b].join()",
               "13,7,30,3,1,1000");
    // The point of the type: past what a double can hold.
    ExpectEval("(2n ** 100n).toString()", "1267650600228229401496703205376");
    ExpectEval("(9007199254740993n * 2n).toString()", "18014398509481986");
    // Truncating division, and a remainder with the dividend's sign.
    ExpectEval("[(-7n / 2n), (-7n % 2n)].join()", "-3,-1");
  });

  AddTest(tests, "JsConformance/BigIntBitwiseWorksOverTheInfiniteExpansion", [] {
    ExpectEval("[10n & 3n, 10n | 3n, 10n ^ 3n, ~10n, -10n].join()", "2,11,9,-11,-10");
    ExpectEval("[10n << 2n, 10n >> 1n, -5n >> 1n, -1n >> 1n].join()", "40,5,-3,-1");
    // There is no unsigned right shift for an unbounded integer: it is defined
    // over a fixed width, and a bigint has none.
    ExpectEval("try { 1n >>> 1n } catch (e) { e instanceof TypeError }", "true");
  });

  AddTest(tests, "JsConformance/MixingABigIntAndANumberIsAnError", [] {
    // The rule the type exists for: a bigint that silently became a double
    // would lose the precision it is there to keep.
    ExpectEval("try { 1n + 1 } catch (e) { e instanceof TypeError }", "true");
    ExpectEval("try { +1n } catch (e) { e instanceof TypeError }", "true");
    ExpectEval("try { BigInt(1.5) } catch (e) { e instanceof RangeError }", "true");
    ExpectEval("try { 1n / 0n } catch (e) { e instanceof RangeError }", "true");
    ExpectEval("try { 2n ** -1n } catch (e) { e instanceof RangeError }", "true");
    // The three exceptions, all of which compare or concatenate rather than
    // compute.
    ExpectEval("[1n == 1, 1n === 1, 1n < 1.5, 2n > 1.5].join()", "true,false,true,true");
    ExpectEval("(1n + 'x') + ':' + ('x' + 1n)", "1x:x1");
  });

  AddTest(tests, "JsConformance/BigIntIsAValueRatherThanAnIdentity", [] {
    // `1n === 1n` compares digits: the two literals are different cells.
    ExpectEval("1n === 1n", "true");
    ExpectEval("const m = new Map(); m.set(1n, 'x'); m.get(1n) + ':' + String(m.get(1))",
               "x:undefined");
    ExpectEval("[0n ? 't' : 'f', 1n ? 't' : 'f'].join()", "f,t");
  });

  AddTest(tests, "JsConformance/BigIntConvertsAndTruncates", [] {
    ExpectEval("[BigInt(42), BigInt('0x1f'), BigInt(true), BigInt('  -7 ')].join()",
               "42,31,1,-7");
    ExpectEval("(255n).toString(16) + ':' + (255n).toString(2)", "ff:11111111");
    ExpectEval("[BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n), BigInt.asUintN(64, -1n)].join()",
               "-1,255,18446744073709551615");
    ExpectEval("let c = 5n; c++; const after = c; c--; [after, c].join()", "6,5");
  });

  AddTest(tests, "JsConformance/ABigIntHasNoJsonForm", [] {
    // A TypeError rather than a lossy number: a page serializing an identifier
    // that does not fit a double needs to know.
    ExpectEval("try { JSON.stringify({a: 1n}) } catch (e) { e instanceof TypeError }", "true");
  });

  // --- Recursion ------------------------------------------------------------

  AddTest(tests, "JsConformance/RecursionGoesAsDeepAsAPageNeeds", [] {
    // A frame on the machine is a vector slot, not a C++ frame, so the limit
    // is a number this engine chooses. Two hundred was the figure from when a
    // call cost a C++ frame, and a recursive walk over a document exceeds it.
    ExpectEval("function f(n){ return n === 0 ? 0 : f(n-1) + 1 } f(2000)", "2000");
    ExpectEval("function g(){ return g() } try { g() } catch (e) { e instanceof RangeError }",
               "true");
  });

  AddTest(tests, "JsConformance/AUnaryOperatorRunsTheConversionToo", [] {
    ExpectEval("-({valueOf(){return 3}})", "-3");
    ExpectEval("~({valueOf(){return 0}})", "-1");
    ExpectEval("let o = {valueOf(){return 4}}; o++; o", "5");
  });
}

}  // namespace microbrowser::tests
