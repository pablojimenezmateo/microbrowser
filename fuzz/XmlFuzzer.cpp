#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dom/Node.h"
#include "xml/XmlParser.h"
#include "xml/XmlSerializer.h"

// The XML parser and serializer, fed arbitrary bytes.
//
// XML's failure mode is the interesting half, and it is the opposite of HTML's.
// HTML always produces a document, so its fuzzer asks "does the state machine
// terminate". XML is allowed to refuse, so the properties here are:
//
//   * It always answers. A `parsererror` document is an answer; a hang or a
//     trap is not. That is the denial-of-service property: anyone who can serve
//     a page can hand this string to `DOMParser`.
//   * The answer is always a document with exactly one element child, whether
//     the parse succeeded or failed. A failed parse that left a half-built tree
//     behind would be a document nothing in the DOM describes.
//   * Serializing whatever came back terminates and produces something the
//     parser will take again. The second parse is where a serializer that
//     invents an unbalanced prefix declaration shows up: it is fine for the
//     round trip to *change* the markup, and not fine for it to produce
//     something that is no longer XML.
//
// The last of those is the one that needs saying out loud: a serializer is an
// output path, and an output path that can be driven to produce syntactically
// broken markup is how a document round-tripped through `XMLSerializer` stops
// being the document a page thought it had.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  microbrowser::xml::XmlParseResult parsed = microbrowser::xml::ParseXml(input);
  if (parsed.document == nullptr) {
    __builtin_trap();  // the contract is "never null", success or failure
  }
  if (parsed.ok != parsed.error.empty()) {
    __builtin_trap();  // "ok" and "carries an error" are the same fact, twice
  }

  std::size_t elements = 0;
  for (const auto& child : parsed.document->Children()) {
    if (child->GetKind() == microbrowser::dom::Node::Kind::Element) {
      ++elements;
    }
  }
  if (elements != 1) {
    __builtin_trap();  // every answer has exactly one document element
  }

  const std::string written = microbrowser::xml::SerializeXml(*parsed.document);

  // Round trip. A successful parse must serialize to something that parses
  // again; a failed one produces the error document, which is trivially valid
  // and is checked the same way rather than special-cased.
  const microbrowser::xml::XmlParseResult again = microbrowser::xml::ParseXml(written);
  if (again.document == nullptr) {
    __builtin_trap();
  }
  if (parsed.ok && !again.ok) {
    __builtin_trap();  // the serializer produced something that is not XML
  }
  return 0;
}
