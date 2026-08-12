#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Parser.h"

namespace microbrowser::tests {

using js::Node;
using js::NodeKind;
using js::ParseResult;

namespace {

std::string ParseToText(std::string_view source) {
  const ParseResult result = js::Parse(source);
  Expect(result.program != nullptr, std::string("no tree for: ") + std::string(source));
  if (!result.errors.empty()) {
    return "ERROR: " + result.errors.front().message;
  }
  return js::DumpAst(*result.program);
}

void ExpectParse(std::string_view source, std::string_view expected) {
  ExpectEqString(ParseToText(source), std::string(expected),
                 std::string("parsing: ") + std::string(source));
}

void ExpectClean(std::string_view source) {
  const ParseResult result = js::Parse(source);
  Expect(result.Ok(), std::string("expected a clean parse of: ") + std::string(source) +
                          (result.errors.empty() ? "" : " -- " + result.errors.front().message));
}

void ExpectError(std::string_view source) {
  const ParseResult result = js::Parse(source);
  Expect(!result.errors.empty(),
         std::string("expected a parse error for: ") + std::string(source));
}

}  // namespace

void RegisterJsParserTests(std::vector<TestCase>& tests) {
  // --- Precedence and associativity ----------------------------------------

  AddTest(tests, "JsParser/BinaryPrecedenceFollowsTheGrammar", [] {
    ExpectParse("1 + 2 * 3",
                R"((Program (ExprStmt (Binary "+" (Number) (Binary "*" (Number) (Number))))))");
    ExpectParse("1 * 2 + 3",
                R"((Program (ExprStmt (Binary "+" (Binary "*" (Number) (Number)) (Number)))))");
    ExpectParse("a || b && c",
                R"((Program (ExprStmt (Logical "||" (Id "a") (Logical "&&" (Id "b") (Id "c"))))))");
    ExpectParse("a | b ^ c & d",
                R"((Program (ExprStmt (Binary "|" (Id "a") (Binary "^" (Id "b") )"
                R"((Binary "&" (Id "c") (Id "d")))))))");
  });

  AddTest(tests, "JsParser/ExponentiationIsRightAssociative", [] {
    // The only right-associative binary operator, which is why the table
    // carries associativity rather than assuming it.
    ExpectParse("2 ** 3 ** 2",
                R"((Program (ExprStmt (Binary "**" (Number) (Binary "**" (Number) (Number))))))");
    ExpectParse("2 - 3 - 4",
                R"((Program (ExprStmt (Binary "-" (Binary "-" (Number) (Number)) (Number)))))");
  });

  AddTest(tests, "JsParser/AssignmentIsRightAssociative", [] {
    ExpectParse("a = b = c",
                R"((Program (ExprStmt (Assign "=" (Id "a") (Assign "=" (Id "b") (Id "c"))))))");
  });

  AddTest(tests, "JsParser/ConditionalBindsLooserThanBinaryAndTighterThanComma", [] {
    ExpectParse("a + b ? c : d",
                R"((Program (ExprStmt (Cond (Binary "+" (Id "a") (Id "b")) (Id "c") (Id "d")))))");
    ExpectParse("a ? b : c, d",
                R"((Program (ExprStmt (Seq (Cond (Id "a") (Id "b") (Id "c")) (Id "d")))))");
  });

  AddTest(tests, "JsParser/InAndInstanceofAreBinaryOperators", [] {
    ExpectClean("'k' in obj");
    ExpectClean("x instanceof Y");
    ExpectParse("a in b === c",
                R"((Program (ExprStmt (Binary "===" (Binary "in" (Id "a") (Id "b")) (Id "c")))))");
  });

  // --- Automatic semicolon insertion ---------------------------------------

  AddTest(tests, "JsParser/ANewlineBeforeAPostfixOperatorEndsTheStatement", [] {
    // `a\n++b` is two statements. This is the ASI rule the newline flag on
    // every token exists for.
    ExpectParse("a\n++b",
                R"((Program (ExprStmt (Id "a")) (ExprStmt (Update "++" (Id "b")))))");
    ExpectParse("a\n+b", R"((Program (ExprStmt (Binary "+" (Id "a") (Id "b")))))");
  });

  AddTest(tests, "JsParser/ReturnFollowedByANewlineReturnsNothing", [] {
    // The ASI rule that surprises people, and the reason a return value must
    // never be put on the next line.
    ExpectParse("return\n1", R"((Program (Return _) (ExprStmt (Number))))");
    ExpectParse("return 1", R"((Program (Return (Number))))");
  });

  AddTest(tests, "JsParser/ThrowRefusesALineTerminatorAfterIt", [] {
    // Unlike return, there is no "throw nothing", so this is a syntax error
    // rather than a silent difference in meaning.
    ExpectError("throw\n1");
    ExpectClean("throw 1");
  });

  AddTest(tests, "JsParser/SemicolonsAreInsertedBeforeBracesAndAtEndOfInput", [] {
    ExpectClean("{ a = 1 }");
    ExpectClean("a = 1");
    ExpectClean("if (a) b = 1\nelse c = 2");
  });

  AddTest(tests, "JsParser/ALabelOnTheNextLineIsNotABreakTarget", [] {
    ExpectParse("break\nfoo", R"((Program (Break) (ExprStmt (Id "foo"))))");
    ExpectParse("break foo", R"((Program (Break "foo")))");
  });

  // --- The regex ambiguity, resolved by the grammar ------------------------

  AddTest(tests, "JsParser/ASlashWhereAValueIsExpectedIsARegExp", [] {
    ExpectParse("x = /ab+/g",
                R"((Program (ExprStmt (Assign "=" (Id "x") (RegExp "/ab+/g")))))");
    ExpectParse("a / b",
                R"((Program (ExprStmt (Binary "/" (Id "a") (Id "b")))))");
    ExpectClean("f(/x/)");
    ExpectClean("[/x/, 1]");
  });

  AddTest(tests, "JsParser/DivisionAfterAValueIsStillDivision", [] {
    ExpectParse("a / b / c",
                R"((Program (ExprStmt (Binary "/" (Binary "/" (Id "a") (Id "b")) (Id "c")))))");
  });

  // --- Functions and arrows -------------------------------------------------

  AddTest(tests, "JsParser/FunctionParametersIncludeDefaultsAndRest", [] {
    ExpectParse(
        "function f(a, b = 2, ...rest) {}",
        R"((Program (FunctionDecl "f" (Params (Id "a") (Default (Id "b") (Number)) )"
        R"((Rest (Id "rest"))) (Block))))");
  });

  AddTest(tests, "JsParser/AParenthesisedListIsAnArrowOnlyWhenAnArrowFollows", [] {
    // The same tokens until the arrow. Rather than guess, the parenthesised
    // form is parsed as an expression and re-parsed as parameters if an arrow
    // turns up.
    ExpectParse("(a, b)", R"((Program (ExprStmt (Seq (Id "a") (Id "b")))))");
    ExpectParse("(a, b) => a",
                R"((Program (ExprStmt (Arrow (Params (Id "a") (Id "b")) (Id "a")))))");
    ExpectParse("() => 1", R"((Program (ExprStmt (Arrow (Params) (Number)))))");
    ExpectParse("x => x", R"((Program (ExprStmt (Arrow (Params (Id "x")) (Id "x")))))");
  });

  AddTest(tests, "JsParser/AnArrowBodyMayBeABlockOrAnExpression", [] {
    ExpectParse("x => { return x; }",
                R"((Program (ExprStmt (Arrow (Params (Id "x")) (Block (Return (Id "x")))))))");
  });

  AddTest(tests, "JsParser/ALineTerminatorBeforeAnArrowIsNotAnArrow", [] {
    // `(a)\n=> b` is a syntax error rather than a function, and treating it as
    // one here means the expression stands instead of silently becoming one.
    const ParseResult result = js::Parse("(a)\n=> b");
    Expect(!result.errors.empty(), "a newline before => is an error");
  });

  // --- Objects, arrays, classes --------------------------------------------

  AddTest(tests, "JsParser/ObjectLiteralsCoverEveryPropertyForm", [] {
    ExpectParse("({ a: 1, b, [k]: 2, m() {}, 'q': 3 })",
                R"((Program (ExprStmt (Object (Property "a" (Number)) (Property "b" (Id "b")) )"
                R"((Property (Number) (Id "k")) (Property "m" (Function "m" (Params) (Block))) )"
                R"((Property "q" (Number))))))");
  });

  AddTest(tests, "JsParser/AKeywordIsALegalPropertyName", [] {
    // `{ if: 1 }` and `x.class` are both fine, which is why keywords are not
    // filtered out of property positions.
    ExpectClean("({ if: 1, class: 2, function: 3 })");
    ExpectClean("x.class");
    ExpectClean("x.default.new");
  });

  AddTest(tests, "JsParser/ArrayHolesAreKept", [] {
    // `[1,,3]` has three elements. Dropping the hole changes the length, so it
    // survives as a null child rather than being compacted away.
    ExpectParse("[1,,3]", R"((Program (ExprStmt (Array (Number) _ (Number)))))");
  });

  AddTest(tests, "JsParser/ClassBodiesHaveMethodsFieldsAndAccessors", [] {
    ExpectClean("class A { m() {} }");
    ExpectClean("class A extends B { constructor() { super(); } }");
    ExpectClean("class A { static x = 1; #private = 2; get y() { return 1; } set y(v) {} }");
    ExpectClean("class A { static m() {} }");
    ExpectClean("class A { get = 1; set = 2; static = 3; }");
  });

  // --- Statements -----------------------------------------------------------

  AddTest(tests, "JsParser/EveryLoopFormParses", [] {
    ExpectClean("for (;;) {}");
    ExpectClean("for (let i = 0; i < 10; i++) {}");
    ExpectClean("for (const k in o) {}");
    ExpectClean("for (const v of xs) {}");
    ExpectClean("for (x of xs) {}");
    ExpectClean("while (a) {}");
    ExpectClean("do {} while (a)");
  });

  AddTest(tests, "JsParser/TryNeedsACatchOrAFinally", [] {
    ExpectClean("try {} catch (e) {}");
    ExpectClean("try {} finally {}");
    ExpectClean("try {} catch {}");  // optional catch binding
    ExpectError("try {}");
  });

  AddTest(tests, "JsParser/SwitchClausesHoldStatements", [] {
    ExpectClean("switch (a) { case 1: b(); break; default: c(); }");
    ExpectClean("switch (a) {}");
  });

  AddTest(tests, "JsParser/LetIsADeclarationOnlyWhenABindingFollows", [] {
    // `let = 1` and `let[0]` are expressions. The difference is one token of
    // lookahead, and getting it wrong makes valid programs fail to parse.
    ExpectParse("let x = 1",
                R"((Program (Var "let" (Declarator (Id "x") (Number)))))");
    ExpectClean("let = 1");
    ExpectClean("let\nx = 1");
  });

  AddTest(tests, "JsParser/LabelledStatementsParse", [] {
    ExpectParse("outer: for (;;) break outer;",
                R"((Program (Labeled "outer" (For _ _ _ (Break "outer")))))");
    ExpectParse("outer: { break outer; }",
                R"((Program (Labeled "outer" (Block (Break "outer")))))");
  });

  // --- Templates ------------------------------------------------------------

  AddTest(tests, "JsParser/TemplateSubstitutionsAreParsed", [] {
    const ParseResult result = js::Parse("`a ${b + 1} c`");
    Expect(result.Ok(), "it parses");
    const Node* statement = result.program->Child(0);
    Expect(statement != nullptr, "there is a statement");
    const Node* literal = statement->Child(0);
    Expect(literal != nullptr && literal->kind == NodeKind::TemplateLiteral, "a template");
    Expect(!literal->children.empty(),
           "with its substitution parsed -- a template whose expressions were left as text "
           "would evaluate to nothing");
  });

  AddTest(tests, "JsParser/EverySubstitutionGetsAChild", [] {
    // One child per `${}`, in order, so the interpreter can pair them with the
    // literal chunks by index. Adjacent substitutions are the case that used to
    // lose all but the first.
    const ParseResult result = js::Parse("`${a}${b}${c}`");
    Expect(result.Ok(), "it parses");
    const Node* statement = result.program->Child(0);
    Expect(statement != nullptr, "there is a statement");
    const Node* literal = statement->Child(0);
    Expect(literal != nullptr && literal->kind == NodeKind::TemplateLiteral, "a template");
    ExpectEqInt(static_cast<long long>(literal->children.size()), 3, "three substitutions");
  });

  AddTest(tests, "JsParser/NestedTemplatesParse", [] {
    ExpectClean("`a ${ `b ${c}` } d`");
  });

  AddTest(tests, "JsParser/NestedTemplateWithImportAssertionStringParses", [] {
    // reddit's es-module-shims polyfill (line 364-369) -- found by snapshot.
    ExpectClean(
        "const x = `foo ${true ? `b(\\`import\"\\${b('','text/css')}\"with{type:\"css\"}\\`)` : "
        "'false'} bar`");
  });

  AddTest(tests, "JsParser/TemplateSubstitutionWithSingleQuoteRegexParses", [] {
    // es-module-shims urlJsString (line 636) -- /'/g inside a substitution.
    ExpectClean("function f(url) { return `'${url.replace(/'/g, \"\\\\'\")}'`; }");
  });

  AddTest(tests, "JsParser/ASubstitutionIsParsedAsAnExpression", [] {
    // A leading brace is an object literal here, not a block.
    ExpectClean("`${ { a: 1 }.a }`");
    ExpectClean("`${ (1, 2) }`");
  });

  AddTest(tests, "JsParser/TaggedTemplatesParse", [] {
    ExpectClean("tag`a ${b} c`");
  });

  // --- Errors and robustness ------------------------------------------------

  AddTest(tests, "JsParser/ObviousSyntaxErrorsAreReported", [] {
    for (const std::string_view source :
         {"(", "}", "function", "class {", "if", "a +", "var", "for (", "{"}) {
      ExpectError(source);
    }
  });

  AddTest(tests, "JsParser/DeepNestingIsAnErrorRatherThanAStackOverflow", [] {
    // Script is attacker-controlled and `((((((` nests as deeply as the input
    // is long, so the depth limit is memory safety rather than tidiness.
    std::string deep(4000, '(');
    const ParseResult result = js::Parse(deep);
    Expect(!result.errors.empty(), "it reports rather than crashing");
    Expect(result.program != nullptr, "and still returns a tree");
  });

  AddTest(tests, "JsParser/DeeplyNestedBlocksAreBounded", [] {
    std::string deep;
    for (int i = 0; i < 4000; ++i) {
      deep += "{";
    }
    const ParseResult result = js::Parse(deep);
    Expect(!result.errors.empty(), "reported");
    Expect(result.program != nullptr, "and no crash");
  });

  AddTest(tests, "JsParser/NestedArrowsDoNotCostExponentialTime", [] {
    // Found by the fuzzer as an out-of-memory. `(a) => x` and `(a)` are the
    // same tokens until the arrow, and parsing the contents once as an
    // expression and again as parameters costs 2^n for n nesting levels -- so
    // a hundred bytes of nested parentheses would hang the parser. The tree is
    // converted rather than re-parsed, which is linear.
    //
    // Asserted as a node count rather than a time, because a timing test on a
    // shared machine is a flake generator. An exponential parser allocates
    // exponentially too, and this is what it would blow up.
    std::string source;
    constexpr int kDepth = 40;
    for (int i = 0; i < kDepth; ++i) {
      source += "(";
    }
    source += "a";
    for (int i = 0; i < kDepth; ++i) {
      source += ")=>x";
    }
    const ParseResult result = js::Parse(source);
    Expect(result.program != nullptr, "it parses");
    Expect(js::DumpAst(*result.program).size() < 100000,
           "the tree is linear in the source rather than exponential in its nesting");
  });

  AddTest(tests, "JsParser/NestedAsyncCallsDoNotCostExponentialTimeEither", [] {
    // The same trap, one production over, and reached the same way: `async(x)`
    // is a call and `async (x) => x` is an arrow, so deciding by parsing the
    // parentheses and putting them back parses them twice per level.
    // `async(async(async(x)))` at eighteen deep took two seconds before the
    // decision became a token scan -- a hundred and twenty-seven bytes, which
    // is a hang any page could serve.
    //
    // Counted rather than timed, for the reason the arrow case above is: an
    // exponential parser allocates exponentially, and a timing test on a
    // shared machine is a flake generator.
    std::string source;
    constexpr int kDepth = 40;
    for (int i = 0; i < kDepth; ++i) {
      source += "async(";
    }
    source += "x";
    for (int i = 0; i < kDepth; ++i) {
      source += ")";
    }
    const ParseResult result = js::Parse(source);
    Expect(result.program != nullptr, "it parses");
    Expect(result.errors.empty(), "as a stack of calls");
    Expect(js::DumpAst(*result.program).size() < 100000,
           "and the tree is linear in the source");
  });

  AddTest(tests, "JsParser/AsyncModifiesAFunctionAndIsOtherwiseAName", [] {
    ExpectClean("async function f(){}");
    ExpectClean("const f = async function(){}");
    ExpectClean("const f = async () => 1");
    ExpectClean("const f = async (a, b) => a + b");
    ExpectClean("const f = async a => a");
    ExpectClean("const o = { async m(){}, async: 1 }");
    ExpectClean("class C { async m(){} static async n(){} async(){} }");
    // Every one of these is `async` as an ordinary name, and the lookahead
    // that decides has to put the token back for all of them.
    ExpectClean("async(1)");
    ExpectClean("async = 1");
    ExpectClean("let async = 1");
    ExpectClean("async.x");
    ExpectClean("[async, async]");
    // A line terminator between `async` and what it would modify separates
    // them: ASI makes this two statements and the spec says so.
    const ParseResult split = js::Parse("async\nfunction f(){}");
    Expect(split.errors.empty(), "it parses");
    Expect(js::DumpAst(*split.program).find("(ExprStmt (Id \"async\"))") != std::string::npos,
           "as an expression statement and then a declaration");
  });

  AddTest(tests, "JsParser/AStarMakesAFunctionAGenerator", [] {
    ExpectClean("function* g(){}");
    ExpectClean("const g = function*(){}");
    ExpectClean("async function* g(){}");
    ExpectClean("const o = { *g(){}, async *h(){} }");
    ExpectClean("class C { *g(){} static *h(){} async *i(){} }");
    // A star cannot start a property or member name, so unlike `async` and
    // `get` there is nothing to put back and nothing that stops being a name.
    ExpectClean("const o = { get: 1, set: 2, async: 3 }");
    // There is no such thing as a generator arrow, and the star has to be
    // rejected rather than skipped -- skipping it would silently make one.
    ExpectError("const g = *() => 1");
  });

  AddTest(tests, "JsParser/ForAwaitIsAForOfWithTheAwaitRecorded", [] {
    // It used to be a syntax error: the `await` was eaten with Eat, which
    // compares against punctuators, and `await` is a keyword.
    ExpectClean("async function f(){ for await (const x of xs) {} }");
    ExpectClean("async function f(){ for await (x of xs) {} }");
    const ParseResult parsed = js::Parse("async function f(){ for await (const x of xs) {} }");
    Expect(parsed.Ok(), "it parses");
    const Node* loop = parsed.program->Child(0)->Child(1)->Child(0);
    Expect(loop != nullptr && loop->kind == NodeKind::ForIn, "as a for-in node");
    Expect(loop->number != 0.0, "with the await recorded on it");
    const ParseResult plain = js::Parse("async function f(){ for (const x of xs) {} }");
    Expect(plain.Ok() && plain.program->Child(0)->Child(1)->Child(0)->number == 0.0,
           "and a plain for-of does not have it");
  });

  AddTest(tests, "JsParser/YieldTakesAnOperandOnlyWhenOneCanStart", [] {
    // Assignment precedence, not unary: the whole sum is yielded, and what
    // comes back is what gets assigned.
    ExpectParse("function* g(){ yield a + b }",
                "(Program (FunctionDecl \"g\" (Params) (Block (ExprStmt "
                "(Yield (Binary \"+\" (Id \"a\") (Id \"b\")))))))");
    ExpectParse("function* g(){ x = yield v }",
                "(Program (FunctionDecl \"g\" (Params) (Block (ExprStmt "
                "(Assign \"=\" (Id \"x\") (Yield (Id \"v\")))))))");
    // Bare `yield`: legal, and its argument is the null child rather than an
    // absent one, so a consumer sees the same shape either way.
    ExpectParse("function* g(){ yield; }",
                "(Program (FunctionDecl \"g\" (Params) (Block (ExprStmt (Yield _)))))");
    ExpectParse("function* g(){ f(yield) }",
                "(Program (FunctionDecl \"g\" (Params) (Block (ExprStmt "
                "(Call (Id \"f\") (Yield _))))))");
    // `yield*` has the same shape as `yield` -- the star is a flag in `number`,
    // which the dump does not print -- so the flag is read rather than the text
    // compared. A dump assertion here would pass whether or not the star was
    // seen at all.
    const auto delegating = [](std::string_view source) {
      const ParseResult parsed = js::Parse(source);
      Expect(parsed.Ok(), std::string("a clean parse of: ") + std::string(source));
      const Node* yield = parsed.program->Child(0)->Child(1)->Child(0)->Child(0);
      Expect(yield != nullptr && yield->kind == NodeKind::Yield, "the statement is a yield");
      return yield->number != 0.0;
    };
    Expect(delegating("function* g(){ yield* inner() }"), "`yield*` delegates");
    Expect(!delegating("function* g(){ yield inner() }"), "and plain `yield` does not");
    // A line terminator ends the yield, the way it ends a `return`. Without
    // this, `yield\nx` would yield x rather than undefined.
    ExpectParse("function* g(){ yield\nx }",
                "(Program (FunctionDecl \"g\" (Params) (Block (ExprStmt (Yield _)) "
                "(ExprStmt (Id \"x\")))))");
  });

  AddTest(tests, "JsParser/RestAndTrailingCommasAreParameterListOnly", [] {
    ExpectClean("(a, b,) => a");
    ExpectClean("(...rest) => rest");
    ExpectClean("(a, ...rest) => a");
    ExpectError("(...rest)");
  });

  AddTest(tests, "JsParser/AParenthesisedFormThatIsNotAPatternIsNotAParameterList", [] {
    ExpectError("(a + b) => x");
    ExpectError("(a.b) => x");
    ExpectError("(1) => x");
    ExpectClean("(a = 1) => x");
    ExpectClean("([a, b]) => a");
    ExpectClean("({ a }) => a");
  });

  AddTest(tests, "JsParser/ARecoveryPathThatConsumesNothingWouldLoopForever", [] {
    // Found by the fuzzer as an out-of-memory from a few hundred bytes of
    // `{{{{`. Past the depth limit, ParseBlock returned without consuming the
    // brace, so the caller's statement loop looked at the same token, called
    // straight back in, and appended an empty block forever. Every statement
    // loop now also refuses to run twice at the same offset.
    std::string braces(4000, '{');
    const ParseResult result = js::Parse(braces);
    Expect(result.program != nullptr, "it terminates and returns a tree");
    Expect(js::DumpAst(*result.program).size() < 200000,
           "without building a node per iteration of a loop that never advances");
  });

  AddTest(tests, "JsParser/ErrorsAreBounded", [] {
    // A parser that recovers badly produces an error per token, and a megabyte
    // of script becomes a megabyte of diagnostics.
    std::string garbage;
    for (int i = 0; i < 5000; ++i) {
      garbage += ") ";
    }
    const ParseResult result = js::Parse(garbage);
    Expect(result.errors.size() <= 32, "the diagnostic count is capped");
  });

  AddTest(tests, "JsParser/AnyInputTerminatesAndProducesATree", [] {
    for (const std::string_view source :
         {"", " ", "\n", "'", "`", "/*", "/", "0x", "a?", "a?b", "...", "=>", "#",
          "class", "new", "case", "default:", "else", "finally", "catch"}) {
      const ParseResult result = js::Parse(source);
      Expect(result.program != nullptr,
             std::string("no tree for: ") + std::string(source));
    }
  });

  AddTest(tests, "JsParser/ARealisticProgramParsesCleanly", [] {
    ExpectClean(R"(
      'use strict';
      const cache = new Map();
      function memoize(fn) {
        return function (...args) {
          const key = JSON.stringify(args);
          if (cache.has(key)) return cache.get(key);
          const value = fn.apply(this, args);
          cache.set(key, value);
          return value;
        };
      }
      class Counter {
        #count = 0;
        static zero = new Counter();
        increment(by = 1) { this.#count += by; return this; }
        get value() { return this.#count; }
      }
      const fib = memoize(n => (n < 2 ? n : fib(n - 1) + fib(n - 2)));
      for (let i = 0; i < 10; i++) {
        try {
          console.log(`fib(${i}) = ${fib(i)}`);
        } catch (error) {
          console.error(error && error.message || 'unknown');
        } finally {
          Counter.zero.increment();
        }
      }
      export_value = { fib, Counter, list: [1, 2, ...[3, 4]], re: /^a+b$/gi };
    )");
  });

  AddTest(tests, "JsParser/DynamicImportTakesAnOptionsBag", [] {
    // `import(spec, options)` -- the import-attributes form. The options are
    // parsed and dropped: there is nowhere to put an import attribute yet, and
    // a CSS module is a feature rather than an argument.
    //
    // **The reason this is a parse test and not a run test** is that a
    // SyntaxError here is not local. It aborts the whole script, so one
    // unparsed argument in one helper file takes every function defined beside
    // it -- which is exactly how
    // shadow-dom/declarative/tentative/…/support/helpers.js stopped defining
    // `createStylesheetHost` and failed a row of tests on a missing name rather
    // than on anything to do with imports.
    ExpectClean("import('./m.js')");
    ExpectClean("import('./m.js', { with: { type: 'css' } })");
    ExpectClean("import(url, opts)");
    // A trailing comma is legal at both arities.
    ExpectClean("import('./m.js',)");
    ExpectClean("import('./m.js', {},)");
    // And the specifier is still required.
    ExpectError("import()");
  });
}

}  // namespace microbrowser::tests
