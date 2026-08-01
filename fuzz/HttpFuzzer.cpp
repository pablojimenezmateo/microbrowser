#include <cstddef>
#include <cstdint>
#include <span>

#include "net/HttpMessage.h"

// The HTTP/1.1 response parser, fed arbitrary bytes.
//
// This parser reads whatever a server sends, which makes it the first hostile
// input in the network stack. It is also where request smuggling lives: every
// documented attack is two parsers resolving an ambiguous framing differently,
// so the property being fuzzed is not only "does not crash" but "never accepts
// a message whose framing is ambiguous".
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);

  // Whole-message.
  {
    microbrowser::net::ResponseParser parser;
    parser.Consume(input);
    parser.Finish();
  }

  // Split at a byte the fuzzer chooses, which is how the parser actually sees
  // data and is the shape most likely to break an incremental state machine.
  if (size > 1) {
    const std::size_t split = static_cast<std::size_t>(data[0]) % size;
    microbrowser::net::ResponseParser parser;
    parser.Consume(input.subspan(0, split));
    parser.Consume(input.subspan(split));
    parser.Finish();
  }
  return 0;
}
