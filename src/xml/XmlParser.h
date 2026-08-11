#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "dom/Node.h"

// XML, parsed the way XML says to parse it: **a well-formedness error is
// fatal**.
//
// That single sentence is the whole reason this exists as its own file rather
// than as a mode of the HTML tree builder. HTML recovers -- every error has a
// defined repair and a tree always comes out. XML does not: the moment the
// input is not well-formed, the parse stops and the document a page gets is one
// `parsererror` element and nothing else. `DOMParser.parseFromString(s,
// "text/xml")` does not throw for a bad string, so "refuse" here means "produce
// the error document", and the caller cannot tell the difference from a
// document that happened to contain a `parsererror` element -- which is exactly
// what every other browser does and what web-platform-tests checks.
//
// **Every byte here is attacker-controlled** (`guidelines/security.md`), so:
//
//   * There is no recursion. The element stack is a vector, so nesting costs
//     heap rather than C++ stack. A depth bound is still enforced, because
//     `dom::Node`'s *destructor* recurses over children and a million-deep tree
//     would overflow on the way out.
//   * There is no external entity resolution, no parameter entities and no
//     recursive general-entity expansion. A general entity declared in the
//     internal subset substitutes its literal text **once**, with only
//     character references expanded inside it, which makes "billion laughs" a
//     grammar this parser does not have rather than a bound somebody has to
//     get right.
//   * Every byte is decoded as UTF-8 before it is classified, and anything
//     ill-formed -- an overlong sequence, a truncated one, a surrogate code
//     point -- becomes U+FFFD. A parser that classified bytes would let a
//     partial sequence carry a `<` past a check.
//
// `fuzz/XmlFuzzer.cpp` is the target that holds this to it, and it lands on the
// same commit as the parser (repo policy, non-negotiable).

namespace microbrowser::xml {

// The namespace a `parsererror` element is in. Mozilla's, and every engine
// copied it, so a page testing for one tests for this URI.
inline constexpr std::string_view kParserErrorNamespace =
    "http://www.mozilla.org/newlayout/xml/parsererror.xml";

struct XmlParseResult {
  // Never null. On failure it is a document whose only child is a
  // `<parsererror>` element in kParserErrorNamespace carrying `error` as text.
  std::unique_ptr<dom::Document> document;
  bool ok = false;
  // Empty when `ok`. Human text; nothing branches on its contents.
  std::string error;
};

XmlParseResult ParseXml(std::string_view input);

}  // namespace microbrowser::xml
