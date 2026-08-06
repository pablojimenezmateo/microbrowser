#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/EventStream.h"

// `text/event-stream`, from a server that holds the connection for hours.
//
// ADR 0020 §5. The properties asserted are the ones a *streaming* caller depends on, and
// they are stronger than "does not crash":
//
//   * **`consumed` never exceeds the input**, or the caller erases past the end of its
//     own buffer on the next read.
//   * **Everything before `consumed` is finished business.** Parsing the consumed prefix
//     alone must produce the same events -- if it did not, the caller that discards the
//     prefix would lose or duplicate a message at every packet boundary.
//   * **No event's data exceeds the bound.** It is a bound on memory a peer controls, so
//     a stream that never sends a blank line must not be able to grow it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  if (size < 1) {
    return 0;
  }
  // The bound out of the input so the boundary is explored rather than one arbitrary
  // limit.
  const std::size_t ceiling = static_cast<std::size_t>(data[0]) * 4u + 1u;
  const std::string_view stream(reinterpret_cast<const char*>(data + 1), size - 1);

  const net::EventStreamResult result = net::ParseEventStream(stream, ceiling);
  if (result.consumed > stream.size()) {
    __builtin_trap();
  }
  for (const net::ServerSentEvent& event : result.events) {
    if (event.data.size() > ceiling) {
      __builtin_trap();
    }
  }
  // The consumed prefix is the whole of what was decided. A caller keeps the rest and
  // parses it again with more bytes, so a prefix that decided something different is a
  // message lost or delivered twice at a packet boundary.
  const net::EventStreamResult prefix =
      net::ParseEventStream(stream.substr(0, result.consumed), ceiling);
  if (prefix.events.size() != result.events.size() || prefix.consumed != result.consumed) {
    __builtin_trap();
  }
  for (std::size_t i = 0; i < prefix.events.size(); ++i) {
    if (prefix.events[i].data != result.events[i].data ||
        prefix.events[i].type != result.events[i].type) {
      __builtin_trap();
    }
  }
  return 0;
}
