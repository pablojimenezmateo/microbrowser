#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "net/Hpack.h"

// HPACK, fed arbitrary bytes as a sequence of header blocks on one connection.
//
// The sequence is the point, and it is what a fuzzer over a single block would
// miss entirely. HPACK is stateful: the dynamic table survives from one block
// to the next, and the bugs worth finding are the ones where block N leaves the
// table in a shape that makes block N+1 read an entry that was never inserted.
// So the input is split on a marker byte and every piece is decoded by the
// *same* decoder, in order.
//
// The properties:
//
//  - no input reads out of bounds, whatever lengths it declares;
//  - the dynamic table never exceeds the size this browser advertised, however
//    many size updates the input contains;
//  - a decoder that has failed once never decodes again, because a block half
//    applied has already left the table disagreeing with the peer's;
//  - and every field that comes out has a non-empty name, since an empty one
//    is what a header-injection sink downstream would be handed.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  namespace hpack = microbrowser::net::hpack;

  hpack::Decoder decoder;
  const auto* bytes = reinterpret_cast<const std::byte*>(data);

  // 0xFF is never the first byte of a well-formed *field*: an indexed field of
  // 0x7F would be an integer continuation, so the marker steers the fuzzer
  // towards block boundaries without eating a byte it would otherwise mutate.
  std::size_t start = 0;
  bool failed_once = false;
  for (std::size_t at = 0; at <= size; ++at) {
    if (at != size && data[at] != 0xFF) {
      continue;
    }
    const std::span<const std::byte> block(bytes + start, at - start);
    start = at + 1;

    std::vector<hpack::Header> headers;
    const bool decoded = decoder.Decode(block, headers);
    if (failed_once && decoded) {
      __builtin_trap();  // a failed decoder must stay failed
    }
    if (!decoded) {
      failed_once = true;
      if (!decoder.Failed() || decoder.Error() == nullptr) {
        __builtin_trap();  // and must say so, with a reason
      }
      continue;
    }
    if (decoder.TableBytes() > hpack::kDynamicTableBytes) {
      __builtin_trap();  // the peer does not get to choose how much we hold
    }
    if (headers.size() > hpack::kMaxHeaderCount) {
      __builtin_trap();
    }
    std::size_t list_bytes = 0;
    for (const hpack::Header& field : headers) {
      if (field.name.empty()) {
        __builtin_trap();
      }
      list_bytes += field.name.size() + field.value.size() + 32;
    }
    if (list_bytes > hpack::kMaxHeaderListBytes) {
      __builtin_trap();
    }

    // Whatever came out has to survive this browser's own encoder and its own
    // decoder, unchanged. A round trip is the cheap way to catch an encoder
    // that mis-frames a length its own decoder happens to tolerate -- which is
    // exactly the asymmetry a request would take to a real server.
    std::string encoded;
    hpack::Encode(headers, encoded);
    hpack::Decoder again;
    std::vector<hpack::Header> back;
    const auto* at_encoded = reinterpret_cast<const std::byte*>(encoded.data());
    if (!again.Decode({at_encoded, encoded.size()}, back)) {
      __builtin_trap();
    }
    if (back.size() != headers.size()) {
      __builtin_trap();
    }
    for (std::size_t i = 0; i < back.size(); ++i) {
      if (back[i].name != headers[i].name || back[i].value != headers[i].value) {
        __builtin_trap();
      }
    }
  }
  return 0;
}
