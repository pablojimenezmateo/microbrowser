#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "html/Token.h"
#include "html/Tokenizer.h"

namespace microbrowser::tests {

using html::Token;
using html::Tokenizer;
using html::TokenizerState;

namespace {

std::vector<Token> TokenizeAll(std::string_view input) {
  Tokenizer tokenizer(input);
  std::vector<Token> tokens;
  while (const auto token = tokenizer.Next()) {
    tokens.push_back(*token);
    if (tokens.size() > 10000) {
      Expect(false, "tokenizer did not terminate");
      break;
    }
  }
  return tokens;
}

// Everything that is not a character or EOF token, so a test can talk about
// structure without spelling out every text run.
std::vector<Token> Structural(std::string_view input) {
  std::vector<Token> out;
  for (const Token& token : TokenizeAll(input)) {
    if (token.kind != Token::Kind::Character && token.kind != Token::Kind::EndOfFile) {
      out.push_back(token);
    }
  }
  return out;
}

std::string TextOf(std::string_view input) {
  std::string text;
  for (const Token& token : TokenizeAll(input)) {
    if (token.kind == Token::Kind::Character) {
      text += token.data;
    }
  }
  return text;
}

}  // namespace

void RegisterTokenizerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Tokenizer/EmitsTagsTextAndEof", [] {
    const std::vector<Token> tokens = TokenizeAll("<p>hi</p>");
    ExpectEqInt(static_cast<long long>(tokens.size()), 4, "start tag, text, end tag, EOF");
    Expect(tokens[0].kind == Token::Kind::StartTag, "a start tag");
    ExpectEqString(tokens[0].data, "p", "named p");
    Expect(tokens[1].kind == Token::Kind::Character, "then text");
    ExpectEqString(tokens[1].data, "hi", "with the text run coalesced into one token");
    Expect(tokens[2].kind == Token::Kind::EndTag, "then an end tag");
    Expect(tokens[3].kind == Token::Kind::EndOfFile, "then EOF");
  });

  AddTest(tests, "Tokenizer/LowercasesTagAndAttributeNames", [] {
    const std::vector<Token> tokens = Structural("<DIV CLASS=Big>");
    ExpectEqString(tokens.at(0).data, "div", "tag names are lowercased");
    ExpectEqString(tokens.at(0).attributes.at(0).name, "class", "and attribute names");
    ExpectEqString(tokens.at(0).attributes.at(0).value, "Big",
                   "but not attribute values, which are data");
  });

  AddTest(tests, "Tokenizer/ParsesEveryAttributeValueForm", [] {
    const std::vector<Token> tokens = Structural(
        "<input a=\"double\" b='single' c=unquoted d e=>");
    const Token& tag = tokens.at(0);
    ExpectEqString(*tag.AttributeValue("a"), "double", "double quoted");
    ExpectEqString(*tag.AttributeValue("b"), "single", "single quoted");
    ExpectEqString(*tag.AttributeValue("c"), "unquoted", "unquoted");
    Expect(tag.HasAttribute("d"), "a bare attribute exists");
    ExpectEqString(*tag.AttributeValue("d"), "", "with an empty value");
  });

  AddTest(tests, "Tokenizer/TheFirstOfADuplicateAttributeWins", [] {
    const std::vector<Token> tokens = Structural("<img src=first src=second>");
    ExpectEqInt(static_cast<long long>(tokens.at(0).attributes.size()), 1, "one attribute kept");
    ExpectEqString(*tokens.at(0).AttributeValue("src"), "first",
                   "the first wins and the difference is observable: this decides which image "
                   "actually loads");
  });

  AddTest(tests, "Tokenizer/RecognizesSelfClosingAndEndTags", [] {
    const std::vector<Token> tokens = Structural("<br/><hr />");
    Expect(tokens.at(0).self_closing, "a self-closing tag is marked");
    Expect(tokens.at(1).self_closing, "with or without a space");
  });

  AddTest(tests, "Tokenizer/ParsesComments", [] {
    const std::vector<Token> tokens = Structural("<!-- hello -->");
    Expect(tokens.at(0).kind == Token::Kind::Comment, "a comment");
    ExpectEqString(tokens.at(0).data, " hello ", "with its data");

    const std::vector<Token> nested = Structural("<!--a--b-->");
    ExpectEqString(nested.at(0).data, "a--b", "a double dash inside a comment is data");

    const std::vector<Token> empty = Structural("<!---->");
    ExpectEqString(empty.at(0).data, "", "an empty comment");
  });

  AddTest(tests, "Tokenizer/ParsesDoctypes", [] {
    const std::vector<Token> html5 = Structural("<!DOCTYPE html>");
    Expect(html5.at(0).kind == Token::Kind::Doctype, "a doctype");
    ExpectEqString(html5.at(0).data, "html", "named html, lowercased");
    Expect(!html5.at(0).force_quirks, "and not forcing quirks");

    const std::vector<Token> legacy = Structural(
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://www.w3.org/TR/html4/strict.dtd\">");
    ExpectEqString(legacy.at(0).public_identifier, "-//W3C//DTD HTML 4.01//EN", "public id");
    ExpectEqString(legacy.at(0).system_identifier, "http://www.w3.org/TR/html4/strict.dtd",
                   "system id");

    const std::vector<Token> broken = Structural("<!DOCTYPE>");
    Expect(broken.at(0).force_quirks,
           "a doctype with no name forces quirks mode, which is a rendering decision rather "
           "than an error to report");
  });

  // Character references are where a tokenizer is most often wrong in ways that
  // matter for security: an unexpanded reference is a filter bypass, and an
  // over-eager one corrupts URLs.
  AddTest(tests, "Tokenizer/ExpandsCharacterReferences", [] {
    ExpectEqString(TextOf("&amp;"), "&", "named, terminated");
    ExpectEqString(TextOf("&amp"), "&", "named, unterminated — legal and common");
    ExpectEqString(TextOf("&lt;script&gt;"), "<script>", "the ones that matter for escaping");
    ExpectEqString(TextOf("&#65;"), "A", "decimal");
    ExpectEqString(TextOf("&#x41;"), "A", "hexadecimal");
    ExpectEqString(TextOf("&#X41;"), "A", "with either case of x");
    ExpectEqString(TextOf("&nbsp;"), "\xC2\xA0", "expanded as UTF-8");
    // **`&notareference;` is `¬areference;`**, and this test asserted the opposite until the full
    // 2,231-entry table landed: `&not` *is* a reference -- one of the specification's historical
    // unterminated ones -- so the longest match consumes it and the rest is text. The HTML Standard
    // uses `&notit;` as its own example of exactly this. The old expectation was not a decision; it
    // was what a 42-entry table happened to produce, written down as though it were one.
    ExpectEqString(TextOf("&notareference;"), "\xC2\xAC" "areference;",
                   "`&not` is a reference, so the longest match takes it and the rest is text");
    ExpectEqString(TextOf("&notin;"), "\xE2\x88\x89",
                   "and `&notin;` is a *longer* one, which is why the match is longest-first: "
                   "first-match would have made it `U+00AC` followed by `in;`");
    ExpectEqString(TextOf("&nosuchname;"), "&nosuchname;",
                   "a name with no reference in it at all is left alone");
  });

  AddTest(tests, "Tokenizer/NormalizesDangerousNumericReferences", [] {
    ExpectEqString(TextOf("&#0;"), "\xEF\xBF\xBD", "NUL becomes U+FFFD");
    ExpectEqString(TextOf("&#xD800;"), "\xEF\xBF\xBD",
                   "a surrogate becomes U+FFFD; passing it through would put invalid UTF-8 in "
                   "the DOM");
    ExpectEqString(TextOf("&#x110000;"), "\xEF\xBF\xBD", "out of range becomes U+FFFD");
    ExpectEqString(TextOf("&#x80;"), "\xE2\x82\xAC",
                   "the C1 range maps through Windows-1252, which the spec requires because a "
                   "decade of documents declared UTF-8 and emitted these");
    // A reference with a thousand digits must not wrap into a plausible code
    // point.
    ExpectEqString(TextOf("&#" + std::string(1000, '9') + ";"), "\xEF\xBF\xBD",
                   "an absurd numeric reference saturates rather than wrapping");
  });

  // The legacy attribute rule. Without it `?a&copy=1` becomes `?a©=1` and the
  // query parameter is gone — real URLs depend on this.
  AddTest(tests, "Tokenizer/DoesNotExpandUnterminatedReferencesInsideAttributes", [] {
    const std::vector<Token> tokens = Structural("<a href='?x&copy=1'>");
    ExpectEqString(*tokens.at(0).AttributeValue("href"), "?x&copy=1",
                   "an unterminated reference followed by = keeps its literal text inside an "
                   "attribute");

    const std::vector<Token> terminated = Structural("<a href='?x&copy;=1'>");
    ExpectEqString(*terminated.at(0).AttributeValue("href"), "?x\xC2\xA9=1",
                   "but a terminated one is still expanded");

    ExpectEqString(TextOf("?x&copy=1"), "?x\xC2\xA9=1",
                   "and the rule applies only inside attributes, not in text");
  });

  // RCDATA and RAWTEXT: inside them, a tag is only a tag if it closes the
  // element that opened the mode.
  AddTest(tests, "Tokenizer/RcDataOnlyEndsOnItsOwnEndTag", [] {
    Tokenizer tokenizer("<title>a <b> c</title>");
    std::vector<Token> tokens;
    while (const auto token = tokenizer.Next()) {
      tokens.push_back(*token);
      if (token->kind == Token::Kind::StartTag && token->data == "title") {
        // The tree builder is what switches the mode, because whether `<title>`
        // means RCDATA depends on where in the tree it appeared.
        tokenizer.SwitchTo(TokenizerState::RcData);
      }
    }
    std::string text;
    bool saw_b = false;
    for (const Token& token : tokens) {
      if (token.kind == Token::Kind::Character) {
        text += token.data;
      }
      saw_b = saw_b || (token.kind == Token::Kind::StartTag && token.data == "b");
    }
    Expect(!saw_b, "a tag inside RCDATA is text, not a tag");
    ExpectEqString(text, "a <b> c", "so it lands in the text run");
    Expect(tokens.back().kind == Token::Kind::EndOfFile, "and the mode ended on </title>");
  });

  AddTest(tests, "Tokenizer/ScriptDataDoesNotEndOnAForeignEndTag", [] {
    Tokenizer tokenizer("<script>if (a</b> b) {}</script>");
    std::string text;
    bool saw_foreign_end_tag = false;
    while (const auto token = tokenizer.Next()) {
      if (token->kind == Token::Kind::StartTag && token->data == "script") {
        tokenizer.SwitchTo(TokenizerState::ScriptData);
      }
      if (token->kind == Token::Kind::Character) {
        text += token->data;
      }
      if (token->kind == Token::Kind::EndTag && token->data == "b") {
        saw_foreign_end_tag = true;
      }
    }
    Expect(!saw_foreign_end_tag, "</b> inside a script is script text");
    Expect(text.find("</b>") != std::string::npos, "and stays in the text");
  });

  // Malformed input is not an error path in HTML — the recovery is normative,
  // every browser implements the same one, and pages depend on it.
  AddTest(tests, "Tokenizer/RecoversFromMalformedMarkupWithoutFailing", [] {
    for (const std::string_view input : {
             "<", "</", "<>", "</>", "<a", "<a ", "<a b", "<a b=", "<a b='", "<!", "<!-",
             "<!--", "<!--a", "<!--a-", "<!DOCTYPE", "<!DOCTYPE ", "<!DOCTYPE h", "&", "&#",
             "&#x", "&#;", "<a b=\"unterminated", "<//>", "<a/", "<?php echo 1; ?>",
         }) {
      const std::vector<Token> tokens = TokenizeAll(input);
      Expect(!tokens.empty(), std::string("no tokens for: ") + std::string(input));
      Expect(tokens.back().kind == Token::Kind::EndOfFile,
             std::string("did not reach EOF for: ") + std::string(input));
    }
  });

  AddTest(tests, "Tokenizer/HandlesEveryByteValueInText", [] {
    // Text from the network is arbitrary bytes, including invalid UTF-8.
    std::string all_bytes;
    for (int i = 1; i < 256; ++i) {
      all_bytes.push_back(static_cast<char>(i));
    }
    const std::vector<Token> tokens = TokenizeAll(all_bytes);
    Expect(tokens.back().kind == Token::Kind::EndOfFile, "reaches EOF");
  });

  AddTest(tests, "Tokenizer/CountsParseErrorsWithoutStopping", [] {
    Tokenizer tokenizer("<a b=\"x\" b=\"y\"><!--");
    while (tokenizer.Next()) {
    }
    Expect(tokenizer.ErrorCount() > 0,
           "parse errors are counted so a page's markup quality is observable, and the "
           "tokenizer keeps going, because HTML has no failure mode");
  });
}

}  // namespace microbrowser::tests
