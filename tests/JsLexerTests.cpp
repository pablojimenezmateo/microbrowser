#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "js/Lexer.h"

namespace microbrowser::tests {

using js::Lexer;
using js::Token;
using js::TokenType;

namespace {

std::vector<Token> Lex(std::string_view source) { return js::TokenizeAll(source); }

std::vector<std::string> Lexemes(std::string_view source) {
  std::vector<std::string> out;
  for (const Token& token : Lex(source)) {
    if (token.type == TokenType::EndOfFile) {
      break;
    }
    out.emplace_back(token.lexeme);
  }
  return out;
}

std::string Joined(std::string_view source) {
  std::string joined;
  for (const std::string& lexeme : Lexemes(source)) {
    if (!joined.empty()) {
      joined.push_back('|');
    }
    joined += lexeme;
  }
  return joined;
}

const Token& At(const std::vector<Token>& tokens, std::size_t index) {
  Expect(index < tokens.size(), "token index out of range");
  return tokens[index];
}

}  // namespace

void RegisterJsLexerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsLexer/EmptyInputIsJustEndOfFile", [] {
    const std::vector<Token> tokens = Lex("");
    ExpectEqInt(static_cast<long long>(tokens.size()), 1, "one token");
    Expect(tokens.at(0).type == TokenType::EndOfFile, "and it is the end");
  });

  AddTest(tests, "JsLexer/KeywordsAreDistinguishedFromIdentifiers", [] {
    const std::vector<Token> tokens = Lex("if let x await");
    Expect(At(tokens, 0).type == TokenType::Keyword, "if is reserved");
    Expect(At(tokens, 1).type == TokenType::Keyword, "and so is let");
    Expect(At(tokens, 2).type == TokenType::Identifier, "x is not");
    Expect(At(tokens, 3).type == TokenType::Keyword, "await is");
  });

  AddTest(tests, "JsLexer/ContextualKeywordsAreIdentifiers", [] {
    // `var get = 1` is legal. Lexing `get`, `of`, `async` and `static` as
    // keywords would break it, which is why they are not in the table.
    for (const std::string_view word : {"get", "set", "of", "async", "static", "from"}) {
      const std::vector<Token> tokens = Lex(word);
      Expect(At(tokens, 0).type == TokenType::Identifier,
             std::string(word) + " is an identifier except where the parser says otherwise");
    }
  });

  // --- The longest-match rule ----------------------------------------------

  AddTest(tests, "JsLexer/PunctuatorsTakeTheLongestMatch", [] {
    ExpectEqString(Joined("a>>>=b"), "a|>>>=|b", "four characters, one operator");
    ExpectEqString(Joined("a>>>b"), "a|>>>|b", "three");
    ExpectEqString(Joined("a>>b"), "a|>>|b", "two");
    ExpectEqString(Joined("a>b"), "a|>|b", "and one");
    ExpectEqString(Joined("a===b"), "a|===|b", "strict equality, not = then ==");
    ExpectEqString(Joined("a?\?=b"), "a|?\?=|b", "nullish assignment");
    ExpectEqString(Joined("a**=b"), "a|**=|b", "exponent assignment");
    ExpectEqString(Joined("a...b"), "a|...|b", "spread");
  });

  AddTest(tests, "JsLexer/OptionalChainingDoesNotSwallowATernary", [] {
    // `a?.3:0` is a conditional whose consequent is `.3`. Lexing `?.` there
    // would make it a syntax error, and the spec carves out exactly this case.
    ExpectEqString(Joined("a?.3:0"), "a|?|.3|:|0", "a ternary with a fractional consequent");
    ExpectEqString(Joined("a?.b"), "a|?.|b", "while a property access is optional chaining");
  });

  // --- Numbers --------------------------------------------------------------

  AddTest(tests, "JsLexer/NumericLiteralsInEveryBase", [] {
    struct Case {
      std::string_view source;
      double value;
    };
    for (const Case& test : {Case{"0", 0.0}, Case{"42", 42.0}, Case{"3.5", 3.5},
                             Case{".5", 0.5}, Case{"1e3", 1000.0}, Case{"1.5e-2", 0.015},
                             Case{"1.5e+2", 150.0}, Case{"0x1F", 31.0}, Case{"0xF_F", 255.0},
                             Case{"0b1011", 11.0}, Case{"0o17", 15.0},
                             Case{"1_000_000", 1000000.0}}) {
      const std::vector<Token> tokens = Lex(test.source);
      Expect(At(tokens, 0).type == TokenType::NumericLiteral,
             std::string(test.source) + " is a number");
      Expect(At(tokens, 0).number == test.value,
             std::string(test.source) + " has the wrong value");
    }
  });

  AddTest(tests, "JsLexer/ANumberTouchingAnIdentifierIsInvalid", [] {
    // `3in` is not `3 in`. Catching it here means the parser never sees a token
    // pair that could not have been written.
    Expect(At(Lex("3in"), 0).type == TokenType::Invalid, "3in");
    Expect(At(Lex("0x"), 0).type == TokenType::Invalid, "a hex prefix with no digits");
    Expect(At(Lex("0b2"), 0).type == TokenType::Invalid, "a binary literal with a 2 in it");
  });

  AddTest(tests, "JsLexer/AnIncompleteExponentIsAnInvalidNumber", [] {
    // `1e` is a syntax error in JavaScript, not the number 1 followed by an
    // identifier. Reporting it as one Invalid token says so; two tokens that
    // each look fine would push the error to whoever noticed they were
    // adjacent.
    Expect(At(Lex("1e"), 0).type == TokenType::Invalid, "1e is not a number");
    Expect(At(Lex("1e+"), 0).type == TokenType::Invalid, "nor is 1e+");
    Expect(At(Lex("1e5"), 0).number == 100000.0, "while a complete exponent is fine");
  });

  // --- Strings --------------------------------------------------------------

  AddTest(tests, "JsLexer/StringEscapesAreDecoded", [] {
    struct Case {
      std::string_view source;
      std::string_view value;
    };
    for (const Case& test : {Case{"'a\\nb'", "a\nb"}, Case{"'\\t'", "\t"},
                             Case{"'\\x41'", "A"}, Case{"'\\u0041'", "A"},
                             Case{"'\\u{1F600}'", "\xF0\x9F\x98\x80"},
                             Case{"'\\''", "'"}, Case{"'\\\\'", "\\"},
                             Case{"'q\\\nr'", "qr"}}) {
      const std::vector<Token> tokens = Lex(test.source);
      Expect(At(tokens, 0).type == TokenType::StringLiteral,
             std::string(test.source) + " is a string");
      ExpectEqString(At(tokens, 0).value, std::string(test.value),
                     std::string(test.source) + " decoded wrong");
    }
  });

  AddTest(tests, "JsLexer/AnUnterminatedStringDoesNotEatTheProgram", [] {
    // A raw newline ends a string. Without that rule one missing quote makes
    // the rest of the file part of the literal.
    const std::vector<Token> tokens = Lex("'abc\nlet x = 1");
    Expect(At(tokens, 0).type == TokenType::Invalid, "the string is invalid");
    Expect(At(tokens, 1).type == TokenType::Keyword && At(tokens, 1).lexeme == "let",
           "and the next line still lexes");
  });

  AddTest(tests, "JsLexer/ALegacyOctalEscapeIsRefused", [] {
    Expect(At(Lex("'\\01'"), 0).type == TokenType::Invalid,
           "\\01 is a legacy octal escape and a syntax error in strict mode");
    ExpectEqString(At(Lex("'\\0'"), 0).value, std::string(1, '\0'),
                   "while \\0 with no digit after it is a NUL");
  });

  AddTest(tests, "JsLexer/AnIncompleteHexEscapeIsRefused", [] {
    Expect(At(Lex("'\\x4'"), 0).type == TokenType::Invalid, "one hex digit is not two");
    Expect(At(Lex("'\\u00'"), 0).type == TokenType::Invalid, "nor two four");
    Expect(At(Lex("'\\u{}'"), 0).type == TokenType::Invalid, "and an empty escape is not a value");
    Expect(At(Lex("'\\u{110000}'"), 0).type == TokenType::Invalid,
           "nor is a code point past the maximum");
  });

  // --- Comments and the newline flag ---------------------------------------

  AddTest(tests, "JsLexer/CommentsAreSkipped", [] {
    ExpectEqString(Joined("a // b\nc"), "a|c", "a line comment runs to the newline");
    ExpectEqString(Joined("a /* b */ c"), "a|c", "and a block comment to its terminator");
    ExpectEqString(Joined("a /* unterminated"), "a",
                   "an unterminated block comment ends the input rather than looping");
  });

  AddTest(tests, "JsLexer/ALineTerminatorIsRecordedOnTheFollowingToken", [] {
    // Automatic semicolon insertion is defined in terms of this, and whitespace
    // is gone by the time the parser runs -- so if the lexer does not record
    // it, ASI becomes a guess.
    const std::vector<Token> same_line = Lex("a b");
    Expect(!At(same_line, 1).newline_before, "no newline between a and b");

    const std::vector<Token> wrapped = Lex("a\nb");
    Expect(At(wrapped, 1).newline_before, "but there is one here");
  });

  AddTest(tests, "JsLexer/ABlockCommentWithANewlineInItCountsAsALineTerminator", [] {
    // Real rule, and the reason `a = b /*\n*/ ++c` parses differently from the
    // same source on one line.
    const std::vector<Token> tokens = Lex("a /*\n*/ b");
    Expect(At(tokens, 1).newline_before,
           "the comment contained a line terminator, so the token after it saw one");
  });

  AddTest(tests, "JsLexer/CrlfIsOneLineTerminator", [] {
    const std::vector<Token> tokens = Lex("a\r\nb");
    ExpectEqInt(static_cast<long long>(At(tokens, 1).line), 2,
                "not two, or every line number doubles on Windows-authored source");
  });

  // --- The regex ambiguity --------------------------------------------------

  AddTest(tests, "JsLexer/ASlashIsDivisionUntilTheParserSaysOtherwise", [] {
    // No amount of lookahead settles `a /b/ g`: it is three divisions or one
    // regex depending on what `a` is. The lexer picks division and the parser
    // asks for a rescan where the grammar allows a literal.
    const std::vector<Token> tokens = Lex("a /b/ g");
    Expect(At(tokens, 1).IsPunctuator("/"), "division by default");
  });

  AddTest(tests, "JsLexer/RescanningYieldsARegExpLiteral", [] {
    Lexer lexer("/ab+c/gi");
    const Token slash = lexer.Next();
    Expect(slash.IsPunctuator("/"), "it lexed as division first");

    const Token regex = lexer.RescanAsRegExp(slash);
    Expect(regex.type == TokenType::RegExpLiteral, "and rescans as a regex");
    ExpectEqString(std::string(regex.lexeme), "/ab+c/gi", "flags included");
  });

  AddTest(tests, "JsLexer/ASlashInsideACharacterClassIsNotTheEnd", [] {
    Lexer lexer("/[/]/");
    const Token slash = lexer.Next();
    const Token regex = lexer.RescanAsRegExp(slash);
    Expect(regex.type == TokenType::RegExpLiteral, "it is a regex");
    ExpectEqString(std::string(regex.lexeme), "/[/]/",
                   "the slash inside the class is a literal slash, which is why the class "
                   "state is tracked at all");
  });

  AddTest(tests, "JsLexer/AnUnterminatedRegExpIsInvalidRatherThanUnbounded", [] {
    Lexer lexer("/abc\nlet x = 1");
    const Token slash = lexer.Next();
    const Token regex = lexer.RescanAsRegExp(slash);
    Expect(regex.type == TokenType::Invalid,
           "a regex may not span lines, or an unterminated one eats the program");
  });

  AddTest(tests, "JsLexer/AnEscapedSlashDoesNotEndARegExp", [] {
    Lexer lexer("/a\\/b/");
    const Token slash = lexer.Next();
    const Token regex = lexer.RescanAsRegExp(slash);
    ExpectEqString(std::string(regex.lexeme), "/a\\/b/", "the escaped slash is part of the body");
  });

  // --- Templates and private names -----------------------------------------

  AddTest(tests, "JsLexer/ATemplateIsOneTokenIncludingItsSubstitutions", [] {
    const std::vector<Token> tokens = Lex("`a ${b + `c ${d}`} e`");
    Expect(At(tokens, 0).type == TokenType::TemplateString, "one token");
    ExpectEqString(std::string(At(tokens, 0).lexeme), "`a ${b + `c ${d}`} e`",
                   "nested templates and all -- the nesting rules live in the parser rather "
                   "than in a brace counter here and another one there");
  });

  AddTest(tests, "JsLexer/AnUnterminatedTemplateIsInvalid", [] {
    Expect(At(Lex("`abc"), 0).type == TokenType::Invalid, "and does not loop");
  });

  AddTest(tests, "JsLexer/PrivateNamesLexAsTheirOwnKind", [] {
    const std::vector<Token> tokens = Lex("this.#count");
    Expect(At(tokens, 2).type == TokenType::PrivateIdentifier, "#count is a private name");
    ExpectEqString(std::string(At(tokens, 2).lexeme), "#count", "including the hash");
    Expect(At(Lex("#"), 0).type == TokenType::Invalid, "a bare hash is not a name");
  });

  // --- HTML-like comments (Annex B §B.1.3) ---------------------------------

  AddTest(tests, "JsLexer/HtmlLikeCommentsAreComments", [] {
    // `<script type="text/javascript"><!--` is how a page written before 1998 hid its script from a
    // browser that did not have one, and it is still on a great many documents -- aozora.gr.jp's front
    // page among them, where without this the *whole* first script fails with
    // `SyntaxError: unexpected token '<'` rather than one line of it. Found by rendering that page.
    ExpectEqString(Joined("<!-- hidden\nvar a = 1;"), "var|a|=|1|;", "the open form is a line comment");
    ExpectEqString(Joined("var a = 1;\n--> also hidden\nvar b = 2;"), "var|a|=|1|;|var|b|=|2|;",
                   "and so is the close form on its own line");
    // At the very start of the source there is no previous line, and the close form is still a comment.
    ExpectEqString(Joined("--> gone\nvar a;"), "var|a|;", "including at offset zero");
  });

  AddTest(tests, "JsLexer/TheCloseFormOnlyCountsAtTheStartOfALine", [] {
    // The rule that keeps Annex B from breaking arithmetic: `a-->b` is `a-- > b`, and it is real code.
    // A lexer that took `-->` as a comment wherever it appeared would silently delete the rest of the
    // line -- which is a wrong program rather than a syntax error.
    ExpectEqString(Joined("a-->b"), "a|--|>|b", "mid-line it is a decrement and a comparison");
    ExpectEqString(Joined("a\n-->b"), "a", "at the start of a line it is a comment");
    // A block comment before it on the line does not spoil it: the condition is "nothing but whitespace
    // and comments", which is exactly what the skip loop has already consumed by the time it gets here.
    ExpectEqString(Joined("a\n/* c */ --> b\nc"), "a|c", "a comment before it still counts");
  });

  // --- Robustness -----------------------------------------------------------

  AddTest(tests, "JsLexer/EveryInputTerminatesAndEndsAtEndOfFile", [] {
    // Source is attacker-controlled: a page serves it. The property is that
    // lexing terminates and the last token is always EndOfFile, whatever the
    // bytes are.
    for (const std::string_view source :
         {"", "\\", "'", "\"", "`", "/*", "0x", "#", "\xFF\xFE", "'\\u{", "a\\",
          "/**/**/", "0b", "1e+", ".", "...", "?", "?\?"}) {
      const std::vector<Token> tokens = Lex(source);
      Expect(!tokens.empty(), "at least one token");
      Expect(tokens.back().type == TokenType::EndOfFile,
             std::string("did not reach EndOfFile for: ") + std::string(source));
      Expect(tokens.size() <= source.size() + 1,
             "no more tokens than bytes plus the end -- a lexer that can emit more than that "
             "has a path that consumes nothing");
    }
  });

  AddTest(tests, "JsLexer/AnEscapeAtTheEndOfInputDoesNotRunPastIt", [] {
    // Found by the fuzzer. A backslash as the last byte made the scanner skip
    // two bytes from the end of the buffer, and every token carries its end
    // offset -- so an error message that slices the source by it reads out of
    // bounds.
    for (const std::string_view source : {"`a\\", "'a\\", "\"a\\"}) {
      const std::vector<Token> tokens = Lex(source);
      for (const Token& token : tokens) {
        Expect(token.end <= source.size(),
               std::string("a token escaped the buffer for: ") + std::string(source));
      }
    }

    Lexer lexer("/a\\");
    const Token slash = lexer.Next();
    const Token regex = lexer.RescanAsRegExp(slash);
    Expect(regex.end <= 3, "and the regex rescan is bounded too");
  });

  AddTest(tests, "JsLexer/OffsetsSpanTheSourceExactly", [] {
    // Every error a user sees points at source, and a parser that reconstructs
    // positions from lexemes gets them wrong the first time a string contains a
    // newline.
    const std::string_view source = "let x = 'a\\nb'; // t\ny";
    for (const Token& token : Lex(source)) {
      Expect(token.start <= token.end && token.end <= source.size(),
             "a token's range is inside the source");
      if (token.type != TokenType::EndOfFile) {
        Expect(source.substr(token.start, token.end - token.start) == token.lexeme,
               "and its lexeme is exactly that range");
      }
    }
  });
}

}  // namespace microbrowser::tests
