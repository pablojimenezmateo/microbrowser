#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Interpreter.h"
#include "js/Realm.h"

// Realms: more than one global object in one heap. ADR 0042.
//
// Its own file rather than more of JsInterpreterTests.cpp, because every test
// here needs the same two things -- two realms, and a way to say which one a
// source ran in -- and none of them is about the language. What they are about is
// *identity*: which `Array.prototype` an array got, which global a function saw,
// and whether two things a page can compare compare equal.
//
// **That is the whole difficulty of this feature.** A wrong realm is almost never
// a wrong answer. `[].map(f)` returns the right array whichever realm's
// `Array.prototype` it carries, and the script that reads it works either way --
// right up to the line that asks `x instanceof Array`, or hands the value to a
// library that does. So nearly every assertion below is on an identity comparison
// rather than on a computed value, and the ones on values are there to prove the
// identity work did not break the computation.
//
// The other half is that a realm is a *security* boundary and not only a language
// one: a function that runs in the wrong realm sees the wrong `window`, which for
// a child browsing context is the parent's. `AFunctionSeesItsOwnGlobalNotItsCallers`
// is that case, and it is the one to look at first if this file ever goes red.

namespace microbrowser::tests {

using js::Completion;
using js::Interpreter;
using js::RealmId;
using js::Result;

namespace {

// Runs `source` in `realm` and answers what it evaluated to, or "throw ..." --
// the same convention JsVmTests uses, for the same reason: a test that expected a
// value and got a throw should say which throw.
std::string EvalIn(Interpreter& interpreter, RealmId realm, std::string_view source) {
  const Interpreter::RealmScope scope(interpreter, realm);
  const Result result = interpreter.Run(source);
  if (result.completion == Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  return js::ToString(result.value);
}

// A second realm, or a failed test. Every case here needs one and none of them is
// about `CreateRealm` refusing, which has its own test.
RealmId SecondRealm(Interpreter& interpreter) {
  const std::optional<RealmId> created = interpreter.CreateRealm();
  Expect(created.has_value(), "a second realm can be created");
  return created.value_or(js::kMainRealm);
}

}  // namespace

void RegisterJsRealmTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsRealm/ASecondRealmHasItsOwnIntrinsics", [] {
    Interpreter interpreter;
    ExpectEqInt(static_cast<long long>(interpreter.RealmCount()), 1,
                "an interpreter starts with exactly one realm");
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(js::kMainRealm), "and it is the one that is running");

    const RealmId second = SecondRealm(interpreter);
    ExpectEqInt(static_cast<long long>(interpreter.RealmCount()), 2, "now there are two");
    // Creating one does not enter it. The host decides when to run in a realm,
    // because a realm is created when a frame's document arrives and entered when
    // its script runs, and those are different turns of the loop.
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(js::kMainRealm),
                "creating a realm does not switch to it");

    Expect(interpreter.GlobalOf(second) != nullptr, "the second realm has a global");
    Expect(interpreter.GlobalOf(second) != interpreter.GlobalOf(js::kMainRealm),
           "and it is not the first realm's global");
    Expect(interpreter.GlobalScopeOf(second) != interpreter.GlobalScopeOf(js::kMainRealm),
           "nor its global scope");

    // The intrinsics are separate objects. This is the observable the whole
    // decision rests on: `frames[0].Array === Array` answering false is how a page
    // knows it is looking at another realm, and it is what every library uses to
    // decide whether a value came from somewhere else.
    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "globalThis.A = Array; 'set'"), "set",
                   "realm 0 can hold its own Array");
    ExpectEqString(EvalIn(interpreter, second, "globalThis.A = Array; 'set'"), "set",
                   "and realm 1 its own");
    // Compared through a value that crosses: realm 1 reads realm 0's constructor
    // off an object realm 0 handed it. `Object.getPrototypeOf([])` in each realm is
    // that realm's `Array.prototype`, and the two are different objects.
    ExpectEqString(
        EvalIn(interpreter, second,
               "var mine = Object.getPrototypeOf([]);"
               "var theirs = Object.getPrototypeOf(Array.prototype);"
               "mine === theirs"),
        "false", "a realm's Array.prototype is not its own Object.prototype");
  });

  AddTest(tests, "JsRealm/AGlobalIsNotSharedBetweenRealms", [] {
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "var x = 'parent'; x"), "parent",
                   "realm 0 declares x");
    // Not merely a different value -- *absent*. A shared global scope would have
    // answered "parent" here, and a per-realm one that inherited from it would
    // have too, which is why the assertion is on undefined rather than on a
    // different string.
    ExpectEqString(EvalIn(interpreter, second, "typeof x"), "undefined",
                   "realm 1 has never heard of it");
    ExpectEqString(EvalIn(interpreter, second, "var x = 'child'; x"), "child",
                   "and declares its own");
    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "x"), "parent",
                   "which did not disturb realm 0's");
  });

  AddTest(tests, "JsRealm/AFunctionSeesItsOwnGlobalNotItsCallers", [] {
    // The security half of ADR 0042 §2, and the reason the realm follows the
    // callee rather than the caller. A function compiled in a child realm and
    // called from the parent must see the *child's* global -- if it saw the
    // caller's, a same-origin frame's script would be handed its embedder's
    // `window`, which is an escape rather than a wrong answer.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "globalThis.who = 'parent'; 'ok'"), "ok",
                   "the parent marks its global");
    ExpectEqString(
        EvalIn(interpreter, second,
               "globalThis.who = 'child';"
               // Non-strict and called with no receiver, so OrdinaryCallBindThis
               // substitutes a global. Which one it substitutes is the question.
               "globalThis.ask = function () { return this.who };"
               "'ok'"),
        "ok", "the child marks its own and exposes a function");

    // Reached from realm 0, through realm 1's global object, and answering about
    // realm 1. Both halves matter: `globalThis.who` is still "parent" in the realm
    // doing the calling.
    Interpreter::RealmScope scope(interpreter, js::kMainRealm);
    js::Object* const child_global = interpreter.GlobalOf(second);
    Expect(child_global != nullptr, "the child global is reachable from the host");
    const js::Value* ask = child_global == nullptr ? nullptr : child_global->Get("ask");
    Expect(ask != nullptr && ask->IsObject(), "and so is the function on it");
    if (ask == nullptr || !ask->IsObject()) {
      return;
    }
    const Result called = interpreter.CallFunction(*ask, js::Value::Undefined(), {});
    ExpectEqInt(static_cast<int>(called.completion), static_cast<int>(Completion::Normal),
                "calling across the realm boundary does not throw");
    ExpectEqString(js::ToString(called.value), "child",
                   "a function's `this` global is its own realm's, not its caller's");
    // And the realm was put back. A guard that leaked would leave the parent
    // running in the child's realm, which is the same escape in the other
    // direction and is invisible until something allocates.
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(js::kMainRealm),
                "and the caller's realm is restored afterwards");
  });

  AddTest(tests, "JsRealm/ABuiltinAllocatesFromItsOwnRealm", [] {
    // The rule ADR 0042 §2 names: a builtin allocates from the realm of the
    // *function*, not of the caller. `frames[0].Array.prototype.map.call(x, f)`
    // has to answer an array carrying the child's `Array.prototype`.
    //
    // This is the case that a design keying the realm on the caller would get
    // wrong while passing every other test in this file, because the answer --
    // the mapped values -- is identical either way.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, second, "globalThis.childArray = Array; 'ok'"), "ok",
                   "the child exposes its Array");

    js::Object* const child_global = interpreter.GlobalOf(second);
    Expect(child_global != nullptr, "the child global is reachable");
    if (child_global == nullptr) {
      return;
    }
    const Interpreter::RealmScope scope(interpreter, js::kMainRealm);
    const js::Value* child_array = child_global->Get("childArray");
    Expect(child_array != nullptr && child_array->IsObject(), "and carries its Array");
    if (child_array == nullptr || !child_array->IsObject()) {
      return;
    }
    // `Array.prototype.map` read off the child's Array, applied to a parent array.
    const js::Value* child_prototype = child_array->object->Get("prototype");
    Expect(child_prototype != nullptr && child_prototype->IsObject(),
           "the child's Array has a prototype");
    if (child_prototype == nullptr || !child_prototype->IsObject()) {
      return;
    }
    const js::Value* map = child_prototype->object->Get("map");
    Expect(map != nullptr && map->IsObject(), "which has map on it");
    if (map == nullptr || !map->IsObject()) {
      return;
    }
    const Result subject = interpreter.Run("[1, 2, 3]");
    ExpectEqInt(static_cast<int>(subject.completion), static_cast<int>(Completion::Normal),
                "the parent makes an array");
    const Result identity = interpreter.Run("(function (v) { return v * 2 })");
    ExpectEqInt(static_cast<int>(identity.completion), static_cast<int>(Completion::Normal),
                "and a callback");
    const Result mapped = interpreter.CallFunction(*map, subject.value, {identity.value});
    ExpectEqInt(static_cast<int>(mapped.completion), static_cast<int>(Completion::Normal),
                "the cross-realm map runs");
    Expect(mapped.value.IsObject(), "and answers an object");
    if (!mapped.value.IsObject()) {
      return;
    }
    // The computation is right...
    ExpectEqInt(static_cast<long long>(mapped.value.object->ElementCount()), 3,
                "with the right number of elements");
    ExpectEqString(js::ToString(mapped.value.object->GetElement(1)), "4",
                   "and the right values in them");
    // ...and the array it came back in belongs to the realm `map` came from.
    ExpectEqInt(static_cast<long long>(mapped.value.object->RealmIndex()),
                static_cast<long long>(second),
                "and the array map allocated is the child realm's");
    Expect(mapped.value.object->Prototype() == child_prototype->object,
           "carrying the child realm's Array.prototype rather than the caller's");
  });

  AddTest(tests, "JsRealm/TheWellKnownSymbolsAreOneCellForEveryRealm", [] {
    // ADR 0042 §1. The specification shares these, and every protocol that
    // crosses a realm depends on it: an array from one realm iterated by a
    // `for...of` compiled in another has to find the *same* `Symbol.iterator`, or
    // the protocol silently does not connect.
    //
    // "Silently" is the word that makes this a test rather than a comment. With
    // two cells, spreading a cross-realm array produces an empty array and throws
    // nothing at all.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, second, "globalThis.childIterator = Symbol.iterator; 'ok'"),
                   "ok", "the child exposes its Symbol.iterator");

    js::Object* const child_global = interpreter.GlobalOf(second);
    Expect(child_global != nullptr, "the child global is reachable");
    if (child_global == nullptr) {
      return;
    }
    const Interpreter::RealmScope scope(interpreter, js::kMainRealm);
    const js::Value* child_iterator = child_global->Get("childIterator");
    Expect(child_iterator != nullptr && child_iterator->IsSymbol(),
           "and it is a symbol");
    if (child_iterator == nullptr || !child_iterator->IsSymbol()) {
      return;
    }
    Expect(child_iterator->object == interpreter.SymbolIterator(),
           "a second realm's Symbol.iterator is the same cell as the first's");

    // And the consequence, which is the reason for the above: a protocol crosses.
    ExpectEqString(EvalIn(interpreter, second,
                          "globalThis.childMade = [1, 2, 3];"
                          "globalThis.childSpread = function (v) { return [...v].length };"
                          "'ok'"),
                   "ok", "the child makes an array and a spread");
    const js::Value* child_made = child_global->Get("childMade");
    const js::Value* child_spread = child_global->Get("childSpread");
    Expect(child_made != nullptr && child_spread != nullptr, "both are reachable");
    if (child_made == nullptr || child_spread == nullptr) {
      return;
    }
    // A parent array spread by the child's code, and a child array spread by the
    // parent's. Either direction finding no iterator would answer 0.
    const Result parent_array = interpreter.Run("[7, 8]");
    const Result spread_parent =
        interpreter.CallFunction(*child_spread, js::Value::Undefined(), {parent_array.value});
    ExpectEqString(js::ToString(spread_parent.value), "2",
                   "the child's spread walks a parent array");
    const Result spread_child = interpreter.Run("(function (v) { return [...v].length })");
    const Result walked =
        interpreter.CallFunction(spread_child.value, js::Value::Undefined(), {*child_made});
    ExpectEqString(js::ToString(walked.value), "3", "and the parent's walks a child array");
  });

  AddTest(tests, "JsRealm/ARealmsObjectsSurviveACollectionWhileAnotherRealmRuns", [] {
    // The collector's root set has to walk *every* realm. A missed one is a
    // use-after-free on a live page rather than a leak: the parent is running, the
    // child's global holds every object its script made, and nothing else in the
    // walk reaches it.
    //
    // Driven by allocating hard in realm 0 -- enough to cross the collection
    // threshold several times over -- and then asking realm 1 whether what it made
    // before any of that is still there and still correct.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, second,
                          "globalThis.kept = {tag: 'child', list: [1, 2, 3]};"
                          "globalThis.keptFn = function () { return kept.tag + kept.list.length };"
                          "keptFn()"),
                   "child3", "the child builds something and can read it back");

    ExpectEqString(EvalIn(interpreter, js::kMainRealm,
                          "var sink = 0;"
                          "for (var i = 0; i < 60000; i++) { sink += ({v: [i, i, i]}).v[0] }"
                          "sink > 0"),
                   "true", "realm 0 allocates enough to force several collections");

    // Both the data and the function that reads it. A root set that reached the
    // global but not the intrinsics would pass the first and fail the second.
    ExpectEqString(EvalIn(interpreter, second, "keptFn()"), "child3",
                   "and the child's objects and functions are both still intact");
    ExpectEqString(EvalIn(interpreter, second, "kept.list.map(function (v) { return v * 2 })[2]"),
                   "6", "and its Array.prototype still works");
  });

  AddTest(tests, "JsRealm/RealmsAreBounded", [] {
    // ADR 0042 §4. A page creates a realm by appending an `<iframe>`, so the count
    // is page-controlled, and every page-controlled count here is bounded -- each
    // realm costs a full set of intrinsics plus a global scope holding every
    // builtin, so an unbounded count is a memory-exhaustion vector reachable from
    // three lines of script.
    Interpreter interpreter;
    std::size_t created = 1;  // realm 0
    while (created < js::kMaxRealms) {
      const std::optional<RealmId> next = interpreter.CreateRealm();
      Expect(next.has_value(), "a realm under the bound is created");
      if (!next.has_value()) {
        return;
      }
      ++created;
    }
    ExpectEqInt(static_cast<long long>(interpreter.RealmCount()),
                static_cast<long long>(js::kMaxRealms), "the bound is reached");
    Expect(!interpreter.CreateRealm().has_value(), "and the next one is refused");
    ExpectEqInt(static_cast<long long>(interpreter.RealmCount()),
                static_cast<long long>(js::kMaxRealms),
                "a refusal adds nothing rather than half a realm");
    // Refusing has to leave the interpreter usable: the caller's answer is "this
    // frame runs no script", not "this page is over".
    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "1 + 1"), "2",
                   "and the interpreter still runs after a refusal");
    ExpectEqString(EvalIn(interpreter, static_cast<RealmId>(js::kMaxRealms - 1), "1 + 1"), "2",
                   "as does the last realm it did hand out");
  });

  AddTest(tests, "JsRealm/EnteringARealmThatDoesNotExistChangesNothing", [] {
    // An id is not a pointer and cannot dangle, so the only way a caller gets one
    // wrong is by inventing it. Switching to a realm that does not exist would be
    // a null dereference on the next property access, so it is a no-op -- and the
    // running realm has to stay whatever it was rather than becoming realm 0,
    // because silently running a child's script in the parent is the failure this
    // whole file is about.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);
    ExpectEqString(EvalIn(interpreter, second, "globalThis.marker = 'child'; 'ok'"), "ok",
                   "the child realm is set up");

    const Interpreter::RealmScope outer(interpreter, second);
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(second), "the child realm is running");
    {
      const Interpreter::RealmScope invented(interpreter, static_cast<RealmId>(9999));
      ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                  static_cast<long long>(second),
                  "entering an id that was never handed out is a no-op");
      ExpectEqString(js::ToString(interpreter.Run("marker").value), "child",
                     "and the realm that was running is still the one running");
    }
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(second), "and leaving it restores correctly");
  });

  AddTest(tests, "JsRealm/AThrowInOneRealmLeavesTheOtherRunnable", [] {
    // Every way out of the interpreter is a `Result` rather than an exception, so
    // a realm guard that restored only on the normal path would be wrong on
    // exactly the interesting case. A child frame's script throwing is the common
    // case, not the rare one.
    Interpreter interpreter;
    const RealmId second = SecondRealm(interpreter);

    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "globalThis.who = 'parent'; 'ok'"), "ok",
                   "the parent is set up");
    const std::string thrown = EvalIn(interpreter, second, "throw new Error('child failed')");
    Expect(thrown.find("child failed") != std::string::npos,
           "the child's throw is reported: " + thrown);
    ExpectEqInt(static_cast<long long>(interpreter.CurrentRealm()),
                static_cast<long long>(js::kMainRealm),
                "and the realm is restored across the abrupt path");
    ExpectEqString(EvalIn(interpreter, js::kMainRealm, "who"), "parent",
                   "so the parent realm is intact");
  });
}

}  // namespace microbrowser::tests
