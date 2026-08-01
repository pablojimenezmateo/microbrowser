#include <cstddef>
#include <cstdint>
#include <string_view>

#include "html/Tokenizer.h"

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
  return 0;
}
