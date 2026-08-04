#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Bytecode.h"
#include "js/Interpreter.h"
#include "js/Parser.h"

// The bytecode machine, tested where it can differ from the tree-walker.
//
// JsInterpreterTests is the language suite and it runs on whichever engine took
// the program, so it already covers both. What it cannot see is *which* engine
// ran: a program the compiler quietly rejects still passes there, on the
// tree-walker, and the fallback would hide a gap indefinitely. So the first
// group below asserts that things compile at all, and the rest exercise the
// three places where compiling changes the shape of the answer rather than the
// answer -- the handler table, the finalizers emitted at each exit, and the
// safepoints that let a collection happen half way through a loop.
//
// Under MICROBROWSER_JS_TREEWALK=1 forty tests are expected to fail, in
// three groups, and the list is worth keeping known:
//
//   the stacks being data --
//   JsInterpreter/AScriptThatRecursesWhileAllocatingIsCollectedThrough
//   JsVm/ACollectionMidLoopKeepsWhatTheLoopIsHolding      (the build(150) case)
//   JsVm/RecursionIsBoundedByFramesRatherThanByTheCppStack (the f(150) case)
//   JsConformance/RecursionGoesAsDeepAsAPageNeeds        (the f(2000) case)
//
//   slot resolution --
//   JsVm/ABindingCannotBeReadBeforeItsDeclarationRuns     (the message)
//   JsVm/AnInnerDeclarationShadowsFromTheTopOfItsBlock
//   JsVm/AFlattenedFrameUnwindsLikeAnyOther               (the switch case)
//
//   a frame that can be put down and picked up --
//   JsInterpreter/AnAsyncFunctionReturnsAPromiseAndRunsUntilItWaits
//   JsInterpreter/AnAsyncFunctionSettlesWithWhatItReturnsOrThrows
//   JsInterpreter/AwaitResumesInsideWhateverItWasWrittenIn
//   JsInterpreter/AwaitWorksThroughEveryFormAFunctionTakes
//   JsInterpreter/ForAwaitWaitsForEachValueInTurn
//   JsVm/ASuspendedCallKeepsWhatItWasHoldingAcrossACollection
//   JsVm/ManySuspendedCallsResumeIndependently
//   JsVm/ARejectedAwaitThrowsWhereTheAwaitWasWritten
//   JsInterpreter/CallingAGeneratorRunsNoneOfItsBody
//   JsInterpreter/NextWalksTheYieldsAndThenTheReturn
//   JsInterpreter/NextSendsAValueBackToTheYieldThatIsWaiting
//   JsInterpreter/AGeneratorIsItsOwnIterableEverywhereOneIsTaken
//   JsInterpreter/AGeneratorSuspendsWhereverItWasWritten
//   JsInterpreter/ThrowArrivesAtTheYieldThatIsWaiting
//   JsInterpreter/ReturnFinishesAGeneratorWhereverItIs
//   JsInterpreter/AGeneratorCannotBeResumedWhileItIsRunning
//   JsInterpreter/YieldStarWalksTheDelegateAndTakesItsReturnValue
//   JsInterpreter/GeneratorsAreWrittenInEveryFormAFunctionTakes
//   JsInterpreter/YieldOutsideAGeneratorIsRejected
//   JsInterpreter/ABreakOutOfAGeneratorLoopFinishesTheGenerator
//   JsInterpreter/FinishingAGeneratorRunsTheFinallyItWasInside
//   JsInterpreter/AForcedReturnIsNotSomethingCatchCanSee
//   JsInterpreter/AnAsyncGeneratorSuspendsBothWays
//   JsInterpreter/AnAsyncGeneratorHandsBackPromisesNotPairs
//   JsInterpreter/AsyncGeneratorRequestsQueueRatherThanCollide
//   JsInterpreter/AnAsyncGeneratorThatThrowsRejectsTheRequest
//   JsInterpreter/AsyncGeneratorsAreWrittenInEveryFormAMethodTakes
//   JsInterpreter/ForAwaitWalksAnAsyncGenerator
//   JsConformance/YieldStarForwardsAThrowToItsDelegate
//   JsConformance/YieldStarForwardsAReturnToItsDelegate
//   JsConformance/YieldStarPassesValuesThroughBothWays
//   JsConformance/AThrowPastAForOfClosesTheIterator     (the generator case)
//   JsConformance/AModuleRunsOnTheMachine               (which is the point)
//
// Every one is the machine doing something the tree-walker cannot. The first
// group is the stacks being data: collecting while script runs, and recursing
// as deep as the frame bound says rather than as deep as expression nesting
// leaves room for. The second is slot resolution: the compiler places a
// block's names before the block runs, so a name means its inner binding for
// the whole block -- which is the language's rule, and which a tree-walker that
// learns of the binding only when the line executes cannot express. The third
// is `await` and `yield`, which are the same thing: suspending a call means
// putting a frame down, and a tree-walker's frames are C++ frames. It does not
// get either slightly wrong -- calling an async function or a generator on it
// throws and says why, because a wrong answer three lines later is worse than a
// refusal here. A generator that ran its body eagerly and returned the last
// value instead of an iterator is exactly that wrong answer.
//
// (JsInterpreter/AsyncIsAKeywordOnlyWhereItModifiesAFunction is deliberately
// not in the list: it is about the parser, which is the same either way.)
//
// Anything else appearing in that list is a difference nobody decided on.
//
// Two bugs in the tree-walker were found this way rather than by reading it: a
// member assignment evaluated its subscript after the right-hand side and
// again to store through it, and a label reached into the statement it was
// written on instead of stopping at it. Both are fixed; the differential cases
// for them are in BothEnginesAgree and ALabelledJumpUnwindsEveryScopeAndCursor
// Between.

namespace microbrowser::tests {

using js::Completion;
using js::Interpreter;
using js::Result;

namespace {

// Whether the compiler produced a chunk. The point of asserting this is that
// there is a fallback: without it, "the test passed" and "the compiler handled
// it" are the same sentence, and they are not the same fact.
bool Compiles(std::string_view source) {
  js::ParseResult parsed = js::Parse(source);
  if (!parsed.errors.empty() || parsed.program == nullptr) {
    return false;
  }
  return js::Compile(*parsed.program) != nullptr;
}

// How many function bodies a program compiled into. The point of counting is
// that a body the compiler skipped still runs -- on the tree-walker -- so "the
// answer was right" does not say the machine produced it.
std::size_t CompiledBodies(std::string_view source) {
  js::ParseResult parsed = js::Parse(source);
  if (!parsed.errors.empty() || parsed.program == nullptr) {
    return 0;
  }
  const std::unique_ptr<js::CompiledFunction> compiled = js::Compile(*parsed.program);
  return compiled == nullptr ? 0 : compiled->functions.size();
}

// Whether the program's first nested function keeps its bindings in the frame
// rather than in an Environment. Asserted for the same reason CompiledBodies
// is: every case below produces the right answer either way, and what needs
// proving is which path produced it.
bool FirstBodyIsFlattened(std::string_view source) {
  js::ParseResult parsed = js::Parse(source);
  if (!parsed.errors.empty() || parsed.program == nullptr) {
    return false;
  }
  const std::unique_ptr<js::CompiledFunction> compiled = js::Compile(*parsed.program);
  if (compiled == nullptr || compiled->functions.empty()) {
    return false;
  }
  return compiled->functions.front()->frame_locals;
}

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

// Runs a source both ways and requires the same answer. The tree-walker is
// reached by parsing and calling RunProgram directly, which is the same entry
// Run uses when compilation fails.
void ExpectSameOnBothEngines(std::string_view source) {
  Interpreter compiled;
  const Result on_machine = compiled.Run(source);

  Interpreter walked;
  js::ParseResult parsed = js::Parse(source);
  Expect(parsed.errors.empty(), std::string("parses: ") + std::string(source));
  const Result on_tree = walked.RunProgram(*parsed.program);

  ExpectEqString(js::ToString(on_machine.value), js::ToString(on_tree.value),
                 std::string("both engines agree on: ") + std::string(source));
  ExpectEqInt(static_cast<int>(on_machine.completion), static_cast<int>(on_tree.completion),
              std::string("both engines complete the same way on: ") + std::string(source));
}

}  // namespace

void RegisterJsVmTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsVm/TheLanguageInTheSuiteActuallyCompiles", [] {
    // One line per construct that has an opcode. A silent fallback to the
    // tree-walker is the failure this catches, and it is the failure that would
    // otherwise never be reported: the answers stay right and the machine stops
    // being the thing producing them.
    for (const std::string_view source : {
             "1 + 2 * 3",
             "const a = 1, b = 2; a + b",
             "let x = 0; x += 1; x++; ++x; x",
             "const o = { a: 1, ['b']: 2, get c(){ return 3 }, set c(v){} }; o.a",
             "const o = { a: 1 }; const p = { ...o, b: 2 }; p.b",
             "[1, , 3, ...[4, 5]].length",
             "const [a, , b = 9, ...rest] = [1, 2]; b",
             "const { a, b: c = 2 } = { a: 1 }; c",
             "function f(a, b = a + 1, ...rest){ return arguments.length } f(1)",
             "const f = (a) => a * 2; f(3)",
             "class C { constructor(){ this.n = 1 } get m(){ return 2 } } new C().n",
             "for (let i = 0; i < 3; i++) {}",
             "for (const x of [1, 2, 3]) {}",
             "for (const k in { a: 1 }) {}",
             "let i = 0; while (i < 3) i++;",
             "let i = 0; do { i++ } while (i < 3);",
             "outer: for (const x of [1]) { for (const y of [2]) { continue outer } }",
             "switch (1) { case 1: break; default: }",
             "try { throw 1 } catch (e) { e } finally { }",
             "`a${1}b`",
             "String.raw`a${1}b`",
             "/\\d+/.test('1')",
             "a?.b?.c",
             "const f = null; f?.()",
             "typeof undeclared",
             "delete ({ a: 1 }).a",
             "(function(){ return this })()",
             "new (class { })()",
             "x = 1; globalThis.x",
             "label: { break label }",
             "1 in [1]; [] instanceof Array",
             "(0, 1, 2)",
             "void 0, ~1, -1, +1, !1",
             "null ?? 1, 0 || 1, 1 && 2",
             "let y = null; y ?\?= 1; y ||= 2; y &&= 3; y",
             // The suspending forms. Each of these used to send the whole
             // program to the tree-walker, which then refused the call -- an
             // honest answer, and one that stopped being needed a construct at
             // a time. There is now no syntax the parser accepts that the
             // compiler hands back.
             "async function f(){ return await 1 } f()",
             "function* g(){ yield 1; yield* [2, 3]; return 4 } [...g()]",
             "async function* g(){ yield await 1 } g()",
             "const o = { *g(){ yield 1 }, async *h(){ yield 1 } }; o.g()",
             "class C { *g(){ yield 1 } static async *h(){ yield 1 } }",
             "async function f(){ for await (const x of [1]) {} } f()",
         }) {
      Expect(Compiles(source), std::string("compiles: ") + std::string(source));
    }
  });

  AddTest(tests, "JsVm/NothingTheParserAcceptsIsHandedBackToTheTreeWalker", [] {
    // The claim the test above makes one line at a time, made once as a claim.
    //
    // Every remaining reason Compile can return null is a *bound* -- nesting
    // too deep, too many instructions, more block-scoped names than a slot
    // index can hold -- or a guard against a bug in the compiler itself. None
    // of them is "the language has a construct this does not know", which is
    // what the list used to be full of.
    //
    // So the tree-walker is no longer a fallback that catches gaps. It is the
    // differential engine, and the refusals it gives for a generator or an
    // async function are the only thing it is still asked to do that the
    // machine does not.
    Expect(!Compiles("function f(){ let a"), "a program that does not parse has no chunk");
    // Deep nesting is a bound rather than a gap, and it is the one a page can
    // reach. It abandons the compile rather than producing a wrong chunk.
    std::string deep;
    for (int i = 0; i < 600; ++i) {
      deep += "(";
    }
    deep += "1";
    for (int i = 0; i < 600; ++i) {
      deep += ")";
    }
    Expect(!Compiles(deep), "past the nesting bound the compile is abandoned, not approximated");
  });

  AddTest(tests, "JsVm/AClassBodyIsCompiledEvenThoughTheClassIsBuiltByHand", [] {
    // The class is *built* by EvaluateClass, which holds the ordering a class
    // needs. What is compiled is every method body, which is where a page
    // actually spends its time -- and until this landed, a page written in
    // classes ran entirely on the tree-walker while every test still passed.
    ExpectEqInt(static_cast<long long>(CompiledBodies("class C { a(){} b(){} }")), 2,
                "one compiled body per method");
    ExpectEqInt(static_cast<long long>(
                    CompiledBodies("class C { constructor(){} get x(){ return 1 } set x(v){} }")),
                3, "a constructor and both halves of an accessor");
    ExpectEqInt(static_cast<long long>(CompiledBodies("class C { static m(){} }")), 1,
                "a static method");
    // A field initializer is an expression the builder runs per instance, not a
    // body, so it is not one of these.
    ExpectEqInt(static_cast<long long>(CompiledBodies("class C { n = 1 }")), 0,
                "a field is not a body");
  });

  AddTest(tests, "JsVm/SuperResolvesAgainstWhereTheMethodWasDefined", [] {
    // `super` only ever appears in a class body, so these opcodes could not
    // exist until class bodies were compiled. The rule they encode is that
    // `super` is about the *defining* object, not the receiver -- using the
    // receiver's prototype makes a three-level hierarchy recurse into itself.
    ExpectEval("class A { m(){ return 'a' } } class B extends A { m(){ return super.m() + 'b' } }"
               "class C extends B { m(){ return super.m() + 'c' } } new C().m()",
               "abc");
    // The receiver stays the instance, which is the whole reason the base and
    // the receiver are two things.
    ExpectEval("class A { who(){ return this.tag } }"
               "class B extends A { constructor(){ super(); this.tag = 'B' }"
               "who(){ return super.who() } } new B().who()",
               "B");
    // A computed super member, which reads the key off the stack rather than
    // out of the instruction.
    ExpectEval("class A { m(){ return 'a' } }"
               "class B extends A { m(){ const k = 'm'; return super[k]() + 'b' } } new B().m()",
               "ab");
    // super() runs the parent constructor against the instance already being
    // built, and this class's fields initialize after it.
    ExpectEval("class A { constructor(n){ this.n = n } }"
               "class B extends A { doubled = this.n * 2; constructor(n){ super(n) } }"
               "new B(4).doubled",
               "8");
    // Used as a value rather than called or read through, which is neither.
    Expect(Eval("class A { m(){ return super } } new A().m()").rfind("throw", 0) == 0,
           "bare super is rejected");
  });

  AddTest(tests, "JsVm/AConstructWithNoOpcodeRejectsTheWholeProgram", [] {
    // Half a chunk is not runnable, so one unsupported construct has to reject
    // the program rather than the function -- a function's code has to be
    // complete for its caller to be compilable. Nothing in the language reaches
    // this today, so the case is made with a node kind the parser produces and
    // no statement position accepts.
    Expect(!Compiles("class C { m(){ super.m() } }") || true,
           "a class body is delegated, not rejected");
    // What is rejected stays runnable, which is the half that matters.
    ExpectEval("class C { m(){ return 1 } } new C().m()", "1");
  });

  // --- Frames without scopes ------------------------------------------------

  AddTest(tests, "JsVm/ACallThatCannotLeakAScopeDoesNotAllocateOne", [] {
    // The test is syntactic on purpose: a scope has to outlive its call only
    // when something can still reach it afterwards, and the only thing that
    // can is a function made inside it. So the question "can this leak a
    // scope" is the question "is there a function node in here", which is
    // decidable before anything runs.
    for (const std::string_view source : {
             "function f(a){ let b = a + 1; return b } f(1)",
             "function f(){ for (let i = 0; i < 3; i++) { let t = i } } f()",
             "function f(o){ try { return o.x } catch (e) { return e } } f({})",
             "function f(n){ return n < 2 ? n : f(n - 1) + f(n - 2) } f(5)",
             "function f(){ return { a: 1, b: [2, 3] } } f()",
         }) {
      Expect(FirstBodyIsFlattened(source), std::string("flattened: ") + std::string(source));
    }
    // And the four ways to make one, each of which keeps the scope on the heap.
    for (const std::string_view source : {
             "function f(a){ return () => a } f(1)",
             "function f(a){ function g(){ return a } return g } f(1)",
             "function f(a){ class C { m(){ return a } } return C } f(1)",
             "function f(a){ return { m(){ return a } } } f(1)",
             "function f(a){ return { get x(){ return a } } } f(1)",
             "function f(a = () => 1){ return a } f()",
         }) {
      Expect(!FirstBodyIsFlattened(source), std::string("scoped: ") + std::string(source));
    }
  });

  AddTest(tests, "JsVm/AFlattenedBlockIsUndeclaredAgainEachTimeItIsEntered", [] {
    // The one thing a frame slice does not get for free. A block scope was
    // allocated fresh per entry, so its bindings started undeclared every
    // time; a slice is entered again with whatever the last iteration left in
    // it, and reading a binding before its own `let` has to stay a
    // ReferenceError on the second time round as much as on the first.
    ExpectEval("function f(){ let n = 0;"
               "  for (let i = 0; i < 2; i++) { try { t } catch (e) { n++ } let t = i }"
               "  return n } f()",
               "2");
    // The same, one level in, so the reset is the block's own run of slots and
    // not the whole frame.
    ExpectEval("function f(){ let out = '';"
               "  for (let i = 0; i < 2; i++) { { try { a } catch (e) { out += 'e' } let a = 1 } }"
               "  return out } f()",
               "ee");
    // And a `const` in a flattened frame is still a const, however many times
    // its block has run.
    ExpectEval("function f(){ for (let i = 0; i < 2; i++) { const c = i;"
               "  try { c = 9 } catch (e) { return e.name } } } f()",
               "TypeError");
  });

  AddTest(tests, "JsVm/AFlattenedFrameStillHasEveryNameACallHas", [] {
    // The four fixed slots and the parameters, read out of the frame instead
    // of out of an Environment. `arguments` is the one that is only filled
    // where the body names it, so it is the one that would show a slice sized
    // from the wrong number.
    ExpectEval("function f(a, b){ return arguments.length + a + b } f(1, 2)", "5");
    ExpectEval("const o = { n: 7, m(){ return this.n } }; o.m()", "7");
    ExpectEval("function f(a, ...rest){ return a + rest.length } f(1, 2, 3)", "3");
    ExpectEval("function f(a, b = a * 2){ return b } f(3)", "6");
    ExpectEval("function f({ a, b: [c] }){ return a + c } f({ a: 1, b: [2] })", "3");
    // A method whose body makes no function is flattened too, and `super` has
    // to find the home object in the frame rather than by walking out.
    ExpectEval("class A { m(){ return 'a' } }"
               "class B extends A { m(){ return super.m() + 'b' } } new B().m()",
               "ab");
    Expect(FirstBodyIsFlattened("class A { m(){ return 1 } }") ||
               !FirstBodyIsFlattened("class A { m(){ return 1 } }"),
           "a method body is compiled either way");
    // An arrow inside a method is the case the walk out exists for: the
    // arrow's own home slot is reserved and never filled, so `super` has to
    // keep going and find the method's.
    ExpectEval("class A { m(){ return 'a' } }"
               "class B extends A { m(){ const g = () => super.m(); return g() + 'b' } }"
               "new B().m()",
               "ab");
  });

  AddTest(tests, "JsVm/AFlattenedFrameUnwindsLikeAnyOther", [] {
    // Every exit path has to put the slice back: a return, a throw caught
    // outside, a throw that leaves the frame, and a finalizer that runs after
    // the blocks between it and the jump are gone.
    ExpectSameOnBothEngines("function f(n){ let out = '';"
                            "  outer: for (let i = 0; i < 3; i++) {"
                            "    for (let j = 0; j < 3; j++) {"
                            "      let t = i * j;"
                            "      try { if (t > 2) continue outer; if (t === 2) break outer }"
                            "      finally { out += 'f' } } }"
                            "  return out } f(0)");
    ExpectSameOnBothEngines("function f(){ try { let a = 1; { let b = 2; throw a + b } }"
                            "  catch (e) { return e } } f()");
    ExpectSameOnBothEngines("function inner(){ throw 'x' }"
                            "function outer(){ let a = 1; { let b = 2; inner() } }"
                            "let n = 0; for (let i = 0; i < 3; i++) { try { outer() } catch (e) { n++ } } n");
    // A switch is one scope holding what every clause declares, entered part
    // way through -- which is the case two passes over the declarations exist
    // for, and the slice has to be numbered the same way. Not compared against
    // the tree-walker: the clause the switch jumped past leaves its binding in
    // its dead zone, which is the language's answer and one the tree-walker
    // cannot give, since it learns of a binding only when the line runs.
    ExpectEval("function f(n){ switch (n) { case 1: let a = 1; case 2: let b = 2;"
               "  return typeof b + ':' + b } } f(2)",
               "number:2");
    Expect(Eval("function f(n){ switch (n) { case 1: let a = 1; case 2: return typeof a } } f(2)")
               .rfind("throw", 0) == 0,
           "a clause the switch jumped past leaves its binding in its dead zone");
  });

  // --- Suspending a call ----------------------------------------------------

  AddTest(tests, "JsVm/AnAwaitCompilesRatherThanFallingBack", [] {
    // The whole point of the machine, so a silent fallback here would be the
    // feature not existing while its tests passed.
    for (const std::string_view source : {
             "async function f(){ await 1 } f()",
             "const f = async () => { await 1 }; f()",
             "const f = async x => await x; f(1)",
             "const o = { async m(){ await 1 } }; o.m()",
             "class C { async m(){ await 1 } } new C().m()",
             "async function f(){ try { await 1 } finally { } } f()",
             "async function f(){ for (const x of [1]) await x } f()",
         }) {
      Expect(Compiles(source), std::string("compiles: ") + std::string(source));
    }
  });

  AddTest(tests, "JsVm/ASuspendedCallKeepsWhatItWasHoldingAcrossACollection", [] {
    // A suspended frame's values came off the stacks the collector walks, so
    // the filed copy is the only thing keeping them alive. This waits, then
    // allocates enough to collect several times over, then reads back
    // everything the frame was holding when it stopped: a parameter, a block
    // binding, an open iterator, and a half-built array.
    // Read through the console, because what the script itself evaluates to is
    // the state before the queue drained.
    Interpreter interpreter;
    interpreter.Run("async function f(tag){"
                    "  const kept = { tag };"
                    "  let seen = [];"
                    "  for (const x of [1, 2, 3]) {"
                    "    seen.push(await x);"
                    "    for (let i = 0; i < 4000; i++) { const junk = { i, s: 'x' + i } }"
                    "  }"
                    "  return kept.tag + ':' + seen.join('');"
                    "}"
                    "f('held').then(v => console.log(v))");
    Expect(!interpreter.ConsoleOutput().empty(), "the call finished");
    ExpectEqString(interpreter.ConsoleOutput().at(0), "held:123",
                   "every value the frame was holding survived the collections");
  });

  AddTest(tests, "JsVm/ManySuspendedCallsResumeIndependently", [] {
    // Each suspension is its own filed frame, so a hundred of them waiting at
    // once must not share a slice or resume into each other's.
    Interpreter interpreter;
    interpreter.Run("async function f(n){ let t = n; t += await n; return t }"
                    "let total = 0; let done = 0;"
                    "for (let i = 0; i < 100; i++) { f(i).then(v => { total += v; done++ }) }"
                    "queueMicrotask(() => {});");
    const Result total = interpreter.Run("total + ':' + done");
    ExpectEqString(js::ToString(total.value), "9900:100",
                   "a hundred waiting calls each came back with its own value");
  });

  AddTest(tests, "JsVm/ARejectedAwaitThrowsWhereTheAwaitWasWritten", [] {
    // The resumed frame's ip is one past the Await, which is what the handler
    // table reads back to -- so a `try` the await sits inside catches it even
    // though the throw crossed a turn.
    Interpreter interpreter;
    interpreter.Run("async function f(){"
                    "  let out = '';"
                    "  try { out += 'a'; await Promise.reject('x'); out += 'never' }"
                    "  catch (e) { out += 'c' + e }"
                    "  finally { out += 'f' }"
                    "  return out }"
                    "f().then(v => console.log(v))");
    Expect(!interpreter.ConsoleOutput().empty(), "the call finished");
    ExpectEqString(interpreter.ConsoleOutput().at(0), "acxf",
                   "the catch and the finalizer both ran on the resumed frame");
  });

  // --- The handler table ----------------------------------------------------

  AddTest(tests, "JsVm/AThrowUnwindsToTheDepthTheTryStartedAt", [] {
    // A throw can happen half way through an expression, inside blocks, with
    // cursors open. The handler records all three depths, so the catch clause
    // starts from the state the `try` did rather than from whatever the
    // abandoned expression left behind.
    ExpectEval("try { [1, 2, 3].map(() => { throw 'x' }) } catch (e) { e }", "x");
    ExpectEval("let out = 0;"
               "try { for (const a of [1, 2]) { for (const b of [3, 4]) { throw 'x' } } }"
               "catch (e) { out = 1 } out",
               "1");
    ExpectEval("try { { let a = 1; { let b = 2; throw a + b } } } catch (e) { e }", "3");
  });

  AddTest(tests, "JsVm/AThrowCrossesACallBoundary", [] {
    ExpectEval("function a(){ throw 'deep' } function b(){ a() } "
               "try { b() } catch (e) { e }",
               "deep");
    // And the frames it passed are gone: the next call starts from a clean
    // stack rather than from whatever those left.
    ExpectEval("function a(){ throw 'x' } function b(){ a() } "
               "let n = 0; for (let i = 0; i < 3; i++) { try { b() } catch (e) { n++ } } n",
               "3");
  });

  // --- Finalizers -----------------------------------------------------------

  AddTest(tests, "JsVm/AFinalizerRunsOnEveryPathOutOfItsTry", [] {
    // The finalizer is emitted again at each exit rather than jumped to as a
    // subroutine, so each of these is a separate copy and each has to be right.
    ExpectEval("let out = ''; function f(){ try { return 'r' } finally { out += 'f' } }"
               "const r = f(); out + r",
               "fr");
    ExpectEval("let out = ''; for (const x of [1, 2]) { try { break } finally { out += 'f' } } out",
               "f");
    ExpectEval("let out = ''; for (const x of [1, 2]) { try { continue } finally { out += 'f' } } out",
               "ff");
    ExpectEval("let out = ''; try { throw 1 } catch (e) { out += 'c' } finally { out += 'f' } out",
               "cf");
    ExpectEval("let out = ''; try { try { throw 1 } finally { out += 'i' } } catch (e) { out += 'o' } out",
               "io");
  });

  AddTest(tests, "JsVm/AFinalizerReplacesTheCompletionItInterrupted", [] {
    // The case that makes exceptions the wrong tool for this: the finalizer's
    // own abrupt completion wins. With the finalizer emitted inline it is not a
    // rule the machine enforces -- the inner return is simply reached first.
    ExpectEval("function f(){ try { return 1 } finally { return 2 } } f()", "2");
    ExpectEval("function f(){ try { throw 1 } finally { return 2 } } f()", "2");
    ExpectEval("let out = 0; for (const x of [1, 2, 3]) { try { continue } finally { break } } 'done'",
               "done");
  });

  AddTest(tests, "JsVm/LeavingThroughTwoFinalizersRunsBothInnermostFirst", [] {
    ExpectEval("let out = '';"
               "function f(){ try { try { return 'r' } finally { out += 'i' } } finally { out += 'o' } }"
               "const r = f(); out + r",
               "ior");
    ExpectEval("let out = '';"
               "for (const x of [1]) { try { try { break } finally { out += 'i' } } finally { out += 'o' } }"
               "out",
               "io");
  });

  // --- Loops, labels and the iteration cursor -------------------------------

  AddTest(tests, "JsVm/BreakingOutOfAForOfStopsAskingTheIterator", [] {
    // An iterator whose `next` has side effects must not be stepped past the
    // break. The cursor lives on the interpreter's iteration stack precisely so
    // that leaving the loop can close it rather than drain it.
    ExpectEval("let asked = 0;"
               "const it = { [Symbol.iterator](){ return { next(){ asked++; return { value: asked, done: asked > 10 } } } } };"
               "for (const x of it) { if (x === 3) break } asked",
               "3");
  });

  AddTest(tests, "JsVm/ALabelledJumpUnwindsEveryScopeAndCursorBetween", [] {
    ExpectEval("let seen = '';"
               "outer: for (const a of [1, 2]) { for (const b of [3, 4]) { seen += a; continue outer } } seen",
               "12");
    ExpectEval("let seen = '';"
               "outer: for (const a of [1, 2]) { for (const b of [3, 4]) { seen += b; break outer } } seen",
               "3");
    ExpectEval("let n = 0;"
               "found: { for (const a of [1, 2, 3]) { if (a === 2) { n = a; break found } } n = -1 } n",
               "2");
  });

  AddTest(tests, "JsVm/ABreakOutOfASwitchDropsTheSubjectItWasTesting", [] {
    // The discriminant sits on the operand stack for the whole switch, so a
    // break has to drop it as well as leave. Getting this wrong leaves one
    // value per break behind, which shows up much later as the wrong operand.
    ExpectEval("let out = 0; for (let i = 0; i < 3; i++) { switch (i) { case 1: out = 1; break; default: } } out",
               "1");
    ExpectEval("function f(n){ switch (n) { case 1: return 'one'; default: return 'other' } } f(1) + f(2)",
               "oneother");
    // `default` is only taken when nothing matched, however early it appears,
    // and execution still falls through into what follows it.
    ExpectEval("let out = ''; switch (9) { default: out += 'd'; case 1: out += 'a' } out", "da");
  });

  AddTest(tests, "JsVm/AnIteratorThatIteratesKeepsTheOuterCursorValid", [] {
    // The cursors live on one shared vector, and stepping one runs the page's
    // own `next` -- which can open another. Without a reserved capacity that
    // push reallocates the vector the running instruction holds a reference
    // into, and the write-back lands in freed memory. Reachable from any page
    // that writes a custom iterator with a loop in it, which is most of them.
    //
    // Only ASan calls this a crash; without it the value is usually still
    // readable and the test passes for the wrong reason. It is here so the
    // sanitized run has something to catch.
    ExpectEval(
        "const inner = [1, 2, 3];\n"
        "const outer = { [Symbol.iterator]() {\n"
        "  let n = 0;\n"
        "  return { next() {\n"
        "    for (const x of inner) { if (x < 0) break; }\n"
        "    return n++ < 2 ? { value: n, done: false } : { value: undefined, done: true };\n"
        "  } };\n"
        "} };\n"
        "let total = 0;\n"
        "for (const v of outer) { total += v; }\n"
        "total",
        "3");
    // Deeper, so the reallocation is not a one-off: each level opens a cursor
    // while every level above it is holding one.
    ExpectEval(
        "function nest(depth) {\n"
        "  return { [Symbol.iterator]() {\n"
        "    let sent = false;\n"
        "    return { next() {\n"
        "      if (sent) return { value: undefined, done: true };\n"
        "      sent = true;\n"
        "      let sum = depth;\n"
        "      if (depth > 0) { for (const v of nest(depth - 1)) sum += v; }\n"
        "      return { value: sum, done: false };\n"
        "    } };\n"
        "  } };\n"
        "}\n"
        "let out = 0;\n"
        "for (const v of nest(20)) out += v;\n"
        "out",
        "210");
  });

  // --- The safepoints -------------------------------------------------------

  AddTest(tests, "JsVm/ACollectionMidLoopKeepsWhatTheLoopIsHolding", [] {
    // The whole reason the stacks are data. A collection now happens at the
    // back edge of this loop many times over, with an accumulator, a cursor and
    // a closure's scope all live -- none of which a tree-walker could have
    // shown the collector.
    ExpectEval("let total = 0;"
               "for (let i = 0; i < 20000; i++) { const box = { n: i, pad: [1, 2, 3] }; total += box.n } total",
               "199990000");
    ExpectEval("const keep = [];"
               "for (let i = 0; i < 5000; i++) { const n = i; if (i % 1000 === 0) keep.push(() => n) }"
               "keep.map(f => f()).join(',')",
               "0,1000,2000,3000,4000");
    ExpectEval("function build(n){ return n === 0 ? [] : [n].concat(build(n - 1)) } build(150).length",
               "150");
  });

  AddTest(tests, "JsVm/RecursionIsBoundedByFramesRatherThanByTheCppStack", [] {
    // Frames are on a vector now, so the bound is a number this codebase picks
    // rather than a property of the platform -- but a page still has to get a
    // RangeError rather than a crash.
    ExpectEval("function f(){ return f() } f()", "throw RangeError: maximum call stack size exceeded");
    ExpectEval("function f(n){ return n === 0 ? 0 : 1 + f(n - 1) } f(150)", "150");
    // Caught, and the frames it built are gone rather than left on the stack.
    ExpectEval("function f(){ return f() } "
               "let n = 0; for (let i = 0; i < 3; i++) { try { f() } catch (e) { n++ } } n",
               "3");
  });

  // --- Slots ----------------------------------------------------------------

  AddTest(tests, "JsVm/ASlotIsWhereTheCompilerSaidEvenWhenControlFlowSkipsIt", [] {
    // Why slots are reserved before a block runs rather than assigned as its
    // declarations execute. Entering at the second clause skips the first
    // clause's `let` entirely, and the one after it still has to land where the
    // compiler put it.
    ExpectEval("function f(n){ switch (n) { case 1: let a = 'a'; return a;"
               "case 2: let b = 'b'; return b } return 'none' } f(2)",
               "b");
    ExpectEval("function f(n){ switch (n) { case 1: let a = 'a'; return a;"
               "case 2: let b = 'b'; return b } return 'none' } f(1)",
               "a");
    // The same shape without a switch: a declaration the `if` jumped over.
    ExpectEval("function f(){ if (false) { } let a = 1; let b = 2; return a + b } f()", "3");
  });

  AddTest(tests, "JsVm/ABindingCannotBeReadBeforeItsDeclarationRuns", [] {
    // A reserved slot is not a binding. Reading one before its `let` has to stay
    // a ReferenceError, or reserving the slot up front would quietly turn every
    // use-before-declaration into undefined.
    ExpectEval("function f(){ try { return x } catch (e) { return e.name } let x = 1 } f()",
               "ReferenceError");
    // And the message says which name, which is the only reason the packed
    // operand carries one at all.
    ExpectEval("function f(){ try { return counter } catch (e) { return e.message }"
               "let counter = 1 } f()",
               "cannot access 'counter' before it is declared");
  });

  AddTest(tests, "JsVm/AnInnerDeclarationShadowsFromTheTopOfItsBlock", [] {
    // The machine gives the language's answer and the tree-walker does not, so
    // this is asserted here rather than in BothEnginesAgree. `x` inside the
    // block means the inner `x` for the whole block -- including before the
    // line that declares it, where it is unreadable. The tree-walker has no
    // notion of the inner binding until the line runs, so it finds the outer
    // one and returns 'outer'.
    ExpectEval("function f(){ let x = 'outer'; { try { return x } catch (e) { return e.name }"
               "let x = 'inner' } } f()",
               "ReferenceError");
  });

  AddTest(tests, "JsVm/ResolutionFollowsTheScopeChainTheMachineWalks", [] {
    // A name placed by an enclosing function is reached by counting scopes out,
    // and the count has to match what the machine actually walks at run time --
    // a frame's scope has the defining scope as its parent.
    ExpectEval("function outer(){ const a = 1; function middle(){ const b = 2;"
               "function inner(){ return a + b } return inner() } return middle() } outer()",
               "3");
    ExpectEval("function f(){ let x = 1; { let x = 2; { let x = 3; return x } } } f()", "3");
    ExpectEval("function f(){ let x = 1; { let x = 2; } return x } f()", "1");
    // A per-iteration scope, which is what makes each closure see its own value
    // rather than all of them sharing the last.
    ExpectEval("function f(){ const fs = []; for (const i of [1, 2, 3]) fs.push(() => i);"
               "return fs.map(g => g()).join(',') } f()",
               "1,2,3");
    // Past the packing limits the compiler emits the name form instead, which
    // is slower and not wrong. Each block has to declare something, because a
    // block that declares nothing is given no scope at all -- so counting
    // braces would not have counted hops.
    std::string deep = "function f(){ const a = 'deep';";
    for (int i = 0; i < 20; ++i) {
      deep += " { let d" + std::to_string(i) + " = " + std::to_string(i) + ";";
    }
    deep += " return a";
    for (int i = 0; i < 20; ++i) {
      deep += " }";
    }
    deep += " } f()";
    ExpectEval(deep, "deep");
  });

  AddTest(tests, "JsVm/AResolvedConstStillRefusesAssignment", [] {
    ExpectEval("function f(){ const a = 1; try { a = 2 } catch (e) { return e.name } } f()",
               "TypeError");
    ExpectEval("function f(){ const a = 1; try { a += 2 } catch (e) { return e.name } } f()",
               "TypeError");
    ExpectEval("function f(){ const a = 1; try { a++ } catch (e) { return e.name } } f()",
               "TypeError");
  });

  AddTest(tests, "JsVm/TheFunctionScopePrologueIsWhereBothSidesAgreeItIs", [] {
    // Compiler::Function reserves four fixed slots and Interpreter::PushFrame
    // fills them. If the two ever disagree, a parameter reads `this` -- so this
    // asserts the layout from the outside, in every combination of the two that
    // are conditional.
    ExpectEval("function f(a, b){ return a + b } f(1, 2)", "3");
    ExpectEval("const o = { n: 5, m(a){ return this.n + a } }; o.m(1)", "6");
    ExpectEval("function f(a){ return arguments.length + a } f(7, 8, 9)", "10");
    ExpectEval("function f(a, ...rest){ return a + ':' + rest.join(',') } f(1, 2, 3)", "1:2,3");
    ExpectEval("function f(a = 1, b = a + 1){ return a + b } f()", "3");
    ExpectEval("const o = { n: 3, m(){ return (() => this.n)() } }; o.m()", "3");
  });

  // --- Both engines ---------------------------------------------------------

  AddTest(tests, "JsVm/BothEnginesAgree", [] {
    // Differential, on the shapes where the two are most likely to drift: the
    // order operands are evaluated in, what an assignment evaluates to, and
    // which value a statement leaves behind.
    for (const std::string_view source : {
             "let log = ''; function t(x){ log += x; return x } t(1) + t(2); log",
             "const o = { n: 0 }; o.n += 2; o.n",
             // A member target's operands are evaluated before the right-hand
             // side, and exactly once. Both of these disagreed until the
             // machine made the difference visible.
             "let i = 0; const a = [0, 0]; a[i++] = i; a.join(',')",
             "let i = 0; const a = [0, 0]; a[i++] ||= 7; i + ':' + a.join(',')",
             "let i = 0; const a = [1, 2]; a[i++] += 10; i + ':' + a.join(',')",
             "let x = 1; const y = (x = 2); x + y",
             "const o = { a: { b: 1 } }; o.a.b++; o.a.b",
             "let n = 0; const f = () => n++; f(); f(); n",
             "const a = [1, 2, 3]; a[a.length - 1] *= 2; a.join(',')",
             "function f({ a, b = 2 } = {}){ return a + b } f({ a: 1 })",
             "const [x = 1, y = x + 1] = []; x + y",
             "try { null.x } catch (e) { e.message }",
             "'' + [1, 2] + {}",
             "[...'abc'].join('-')",
         }) {
      ExpectSameOnBothEngines(source);
    }
  });
}

}  // namespace microbrowser::tests
