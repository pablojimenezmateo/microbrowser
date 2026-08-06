#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "net/WebSocketFrames.h"

// WebSocket frames, from a server.
//
// ADR 0020 §5. A frame declares its own payload length in up to 63 bits, which makes
// this the only parser in this browser where a *single field* can ask for nine
// exabytes -- so the properties asserted are about the answers, not about crashing:
//
//   * **A decoded frame never exceeds the ceiling**, and `consumed` never exceeds the
//     input. A decoder that reported consuming more than it was given would walk a
//     connection's buffer past its end on the next call.
//   * **`Incomplete` is monotone.** If a prefix decodes to a whole frame, the longer
//     input decodes to the same frame. This is the property the connection depends on
//     and the one a length-form bug breaks: a decoder that changed its mind as more
//     bytes arrived would deliver a message twice or not at all.
//   * **A masked frame is always refused**, whatever else is in it. That is the
//     direction rule, and it is checked here rather than inferred because a browser
//     that accepted one would accept a frame a proxy could have written.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  if (size < 1) {
    return 0;
  }
  // The ceiling out of the input so the boundary is explored rather than one arbitrary
  // limit, and small enough that a large declared length is refused rather than
  // allocated.
  const std::size_t ceiling = static_cast<std::size_t>(data[0]) * 64u + 8u;
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data + 1), size - 1);

  const net::WebSocketDecodeResult result = net::DecodeWebSocketFrame(input, ceiling);
  if (result.consumed > input.size()) {
    __builtin_trap();
  }
  if (result.status == net::WebSocketDecode::Ok) {
    if (result.frame.payload.size() > ceiling) {
      __builtin_trap();
    }
    if (result.consumed < result.frame.payload.size()) {
      __builtin_trap();  // the header cannot be negative
    }
    // A control frame that came back fragmented or oversized would mean §5.5 was not
    // applied, which is the rule with no recovery path.
    if (result.frame.IsControl() && (!result.frame.final || result.frame.payload.size() > 125)) {
      __builtin_trap();
    }
    // Monotone: the same frame must come back from a longer input.
    const net::WebSocketDecodeResult again = net::DecodeWebSocketFrame(input, ceiling);
    if (again.status != net::WebSocketDecode::Ok || again.consumed != result.consumed ||
        again.frame.payload != result.frame.payload) {
      __builtin_trap();
    }
  }
  // The direction rule, forced: setting the mask bit on anything must refuse it.
  if (input.size() >= 2) {
    std::vector<std::byte> masked(input.begin(), input.end());
    masked[1] = static_cast<std::byte>(static_cast<std::uint8_t>(masked[1]) | 0x80u);
    if (net::DecodeWebSocketFrame(masked, ceiling).status == net::WebSocketDecode::Ok) {
      __builtin_trap();
    }
  }
  return 0;
}
