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
}

}  // namespace microbrowser::tests
