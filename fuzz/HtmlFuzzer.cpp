#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dom/Node.h"
#include "html/Tokenizer.h"
#include "html/TreeBuilder.h"

// The HTML tokenizer, fed arbitrary bytes.
//
// HTML has no failure mode: every input is a document, and the recovery is
// normative rather than a quality-of-implementation matter. So the property
// being fuzzed is not "does not crash on valid input" but "terminates and
// reaches EOF on *any* input" — a tokenizer that can loop forever on malformed
// markup is a denial of service reachable by anyone who can serve a page.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  microbrowser::html::Tokenizer tokenizer(input);

  // Bounded so a hang is a finding rather than a timeout nobody diagnoses. No
  // input can produce more tokens than it has bytes, plus the EOF token.
  const std::size_t limit = size + 2;
  std::size_t produced = 0;
  bool saw_eof = false;
  while (const auto token = tokenizer.Next()) {
    if (token->kind == microbrowser::html::Token::Kind::EndOfFile) {
      saw_eof = true;
    }
    if (++produced > limit) {
      __builtin_trap();  // more tokens than input bytes: the state machine is looping
    }
  }
  if (!saw_eof) {
    __builtin_trap();  // finished without an EOF token
  }

  // Tree construction over the same bytes. HTML has no failure mode, so the
  // property is that *every* input produces a document with an html element —
  // not that well-formed input does.
  //
  // Bounded because a page can nest ten thousand elements, and both building
  // and destroying such a tree recurse. A stack overflow here is reachable by
  // anyone who can serve a page.
  if (size <= 64 * 1024) {
    const std::unique_ptr<microbrowser::dom::Document> document =
        microbrowser::html::ParseDocument(input);
    if (document == nullptr || document->DocumentElement() == nullptr) {
      __builtin_trap();
    }
    // Walking and serializing exercise the tree the builder actually made,
    // rather than only the code that made it.
    std::size_t nodes = 0;
    document->ForEachDescendant([&nodes](const microbrowser::dom::Node&) { ++nodes; });
    const std::string serialized = document->SerializeChildren();
    if (serialized.empty()) {
      __builtin_trap();  // every document serializes to at least <html>...
    }
  }
  return 0;
}
