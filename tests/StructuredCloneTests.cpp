// The structured clone algorithm, as bytes.
//
// ADR 0026 §1: `history.state` is stored as bytes because a history entry
// outlives the document that created it. The tests worth reading are the two
// that would be wrong in a way nothing notices: a cycle that comes back as one
// object rather than two, and a value that is *refused* rather than silently
// dropped.

#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "js/Interpreter.h"
#include "js/StructuredClone.h"

namespace microbrowser::tests {

namespace {

using js::Interpreter;
using js::SerializedValue;
using js::Value;

// Evaluates `source`, clones whatever it produced, puts the clone back into the
// same interpreter as `clone`, and returns what `check` evaluates to. One
// interpreter for both halves is the harder case, not the easier one: it is
// where "the clone shares nothing with the original" can actually be observed.
std::string RoundTrip(std::string_view source, std::string_view check) {
  Interpreter interpreter;
  const js::Result made = interpreter.Run("globalThis.original = (" + std::string(source) + ");");
  // The message is built only on failure, deliberately: a fixture here can be a
  // self-referential array, and `ToString` on one recurses until the stack ends.
  // An `Expect` whose message stringifies the value would crash on the very
  // cases this file exists to test.
  if (made.completion == js::Completion::Throw) {
    Expect(false, "the fixture must evaluate: " + js::ToString(made.value));
  }
  const Value* original = interpreter.Global()->Get("original");
  Expect(original != nullptr, "and leave something behind");

  const std::optional<SerializedValue> bytes = js::StructuredSerialize(interpreter, *original);
  Expect(bytes.has_value(), "and be clonable");
  interpreter.Global()->Set("clone", js::StructuredDeserialize(interpreter, *bytes));

  const js::Result result = interpreter.Run("String(" + std::string(check) + ")");
  if (result.completion == js::Completion::Throw) {
    Expect(false, "the check must not throw: " + js::ToString(result.value));
  }
  return js::ToString(result.value);
}

// Whether `source` can be cloned at all.
bool Clonable(std::string_view source) {
  Interpreter interpreter;
  interpreter.Run("globalThis.original = (" + std::string(source) + ");");
  const Value* original = interpreter.Global()->Get("original");
  return original != nullptr &&
         js::StructuredSerialize(interpreter, *original).has_value();
}

}  // namespace

void RegisterStructuredCloneTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StructuredClone/CarriesEveryPrimitive", [] {
    ExpectEqString(RoundTrip("undefined", "typeof clone"), "undefined", "undefined");
    ExpectEqString(RoundTrip("null", "clone"), "null", "null");
    ExpectEqString(RoundTrip("true", "clone"), "true", "true");
    ExpectEqString(RoundTrip("-0", "1 / clone"), "-Infinity", "negative zero survives");
    ExpectEqString(RoundTrip("NaN", "clone !== clone"), "true", "NaN is a value, not an error");
    ExpectEqString(RoundTrip("1e308 * 10", "clone"), "Infinity", "Infinity");
    ExpectEqString(RoundTrip("'a\\u0000b'", "clone.length"), "3",
                   "a string is bytes and a NUL is one of them");
    ExpectEqString(RoundTrip("123456789012345678901234567890n", "clone + 1n"),
                   "123456789012345678901234567891", "a bigint keeps every digit");
  });

  AddTest(tests, "StructuredClone/RebuildsAnObjectGraphWithoutSharingIt", [] {
    ExpectEqString(RoundTrip("({a: 1, b: {c: 'x'}})", "clone.b.c"), "x", "nested");
    ExpectEqString(RoundTrip("({a: 1})", "clone === original"), "false", "a copy, not the value");
    ExpectEqString(RoundTrip("({a: 1})", "(original.a = 2, clone.a)"), "1",
                   "and writing the original does not reach the clone");
    ExpectEqString(RoundTrip("({a: 1, b: 2})", "Object.keys(clone).join(',')"), "a,b",
                   "in insertion order");
  });

  AddTest(tests, "StructuredClone/AValueSeenTwiceIsOneObjectAgain", [] {
    // The property a deep copy gets wrong: a page that stores a graph and reads
    // back a tree has a bug it cannot see from the values.
    ExpectEqString(RoundTrip("(function () { const s = {n: 1}; return {x: s, y: s} })()",
                             "clone.x === clone.y"),
                   "true", "shared stays shared");
    ExpectEqString(RoundTrip("(function () { const o = {}; o.self = o; return o })()",
                             "clone.self === clone"),
                   "true", "and a cycle closes rather than recursing forever");
    ExpectEqString(RoundTrip("(function () { const a = [1]; a.push(a); return a })()",
                             "clone[1] === clone"),
                   "true", "including through an array");
  });

  AddTest(tests, "StructuredClone/AnArrayKeepsItsHolesAndItsNamedProperties", [] {
    ExpectEqString(RoundTrip("[1, , 3]", "clone.length + ':' + (1 in clone)"), "3:false",
                   "a hole is not an undefined");
    ExpectEqString(RoundTrip("(function () { const a = [1]; a.tag = 'x'; return a })()",
                             "clone.length + ':' + clone.tag"),
                   "1:x", "an array can carry a named property");
    ExpectEqString(RoundTrip("[]", "Array.isArray(clone)"), "true", "and is still an array");
  });

  AddTest(tests, "StructuredClone/RebuildsADateARegExpAMapAndASet", [] {
    ExpectEqString(RoundTrip("new Date(86400000)", "clone.getTime()"), "86400000", "a Date");
    ExpectEqString(RoundTrip("new Date(86400000)", "clone instanceof Date"), "true",
                   "as a real one");
    ExpectEqString(RoundTrip("/ab+c/gi", "clone.source + ':' + clone.flags"), "ab+c:gi",
                   "a RegExp with its flags");
    // A Map's index lives beside the heap, so a Map assembled without going
    // through `set` answers `get` with undefined for a key it contains. This is
    // the assertion that catches that.
    ExpectEqString(RoundTrip("new Map([['k', 1], ['j', 2]])", "clone.get('j')"), "2",
                   "a Map answers lookups");
    ExpectEqString(RoundTrip("new Map([['k', 1]])", "clone.size"), "1", "and knows its size");
    ExpectEqString(RoundTrip("new Set([1, 2, 2])", "clone.has(2) + ':' + clone.size"), "true:2",
                   "a Set too");
    ExpectEqString(RoundTrip("new Map([['k', {a: 1}]])", "clone.get('k').a"), "1",
                   "and its values are cloned in turn");
  });

  AddTest(tests, "StructuredClone/CarriesBytes", [] {
    ExpectEqString(RoundTrip("new Uint8Array([1, 2, 255])", "clone[2] + ':' + clone.length"),
                   "255:3", "a typed array");
    ExpectEqString(RoundTrip("new Uint8Array([1])", "clone instanceof Uint8Array"), "true",
                   "of the right kind");
    ExpectEqString(RoundTrip("new Float64Array([0.5])", "clone[0]"), "0.5", "and the right width");
    ExpectEqString(RoundTrip("new Uint8Array([1, 2, 3]).buffer", "clone.byteLength"), "3",
                   "an ArrayBuffer");
    ExpectEqString(
        RoundTrip("(function () { const v = new DataView(new ArrayBuffer(4));"
                  " v.setInt32(0, -7); return v })()",
                  "clone.getInt32(0)"),
        "-7", "a DataView");
  });

  AddTest(tests, "StructuredClone/RefusesWhatItCannotCarryRatherThanDroppingIt", [] {
    // The whole design. A serializer that dropped a function would hand a page
    // back an object that is *nearly* the one it stored, and the bug surfaces a
    // navigation later in code that has no idea a clone happened.
    Expect(!Clonable("function () {}"), "a function");
    Expect(!Clonable("({run: function () {}})"), "or one inside an object");
    Expect(!Clonable("[1, function () {}]"), "or one inside an array");
    Expect(!Clonable("Symbol('x')"), "a symbol, whose identity is all it is");
    Expect(!Clonable("new Proxy({}, {})"), "a proxy, whose behaviour is a function");
    Expect(!Clonable("new Error('x')"), "an Error, deliberately");
    Expect(!Clonable("({get a() { return 1 }})") == false,
           "a getter is skipped rather than refused -- calling it would be re-entrancy");
    ExpectEqString(RoundTrip("({get a() { return 1 }, b: 2})",
                             "Object.keys(clone).join(',')"),
                   "b", "so an accessor simply does not travel");
  });

  AddTest(tests, "StructuredClone/RefusesBytesItCannotRead", [] {
    Interpreter interpreter;
    SerializedValue truncated;
    // A `Number` tag with no number behind it.
    truncated.bytes = {4, 0, 0};
    Expect(js::StructuredDeserialize(interpreter, truncated).IsUndefined(),
           "a truncated stream is undefined rather than a partly-built value");
    SerializedValue unknown;
    unknown.bytes = {200};
    Expect(js::StructuredDeserialize(interpreter, unknown).IsUndefined(), "an unknown tag");
    SerializedValue lying;
    // A `String` tag claiming a length longer than the stream.
    lying.bytes = {5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 'a'};
    Expect(js::StructuredDeserialize(interpreter, lying).IsUndefined(),
           "a length longer than what is left is a refusal, not an allocation");
    SerializedValue empty;
    Expect(js::StructuredDeserialize(interpreter, empty).IsUndefined(), "no bytes at all");
  });

  AddTest(tests, "StructuredClone/BoundsHowDeeplyAStreamMayNest", [] {
    // A page can write the bytes indirectly by nesting a value it stores, and
    // the reader recurses. ADR 0009's shape: a refusal rather than a stack
    // overflow.
    Interpreter interpreter;
    const js::Result made = interpreter.Run(
        "let deep = {}; for (let i = 0; i < 5000; i++) { deep = {next: deep} } deep");
    Expect(made.completion != js::Completion::Throw, "the fixture builds");
    // Refused on the way *out*, not on the way back in: both halves recurse, so
    // both carry the bound, and the writer reaching it first is what keeps the
    // reader from ever seeing a stream this deep.
    Expect(!js::StructuredSerialize(interpreter, made.value).has_value(),
           "a graph deeper than the bound is refused rather than overflowing the stack");
    const js::Result shallow = interpreter.Run(
        "let ok = {}; for (let i = 0; i < 100; i++) { ok = {next: ok} } ok");
    const std::optional<SerializedValue> bytes =
        js::StructuredSerialize(interpreter, shallow.value);
    Expect(bytes.has_value(), "and one inside it is not");
    Expect(!js::StructuredDeserialize(interpreter, *bytes).IsUndefined(), "and reads back");
  });
}

}  // namespace microbrowser::tests
