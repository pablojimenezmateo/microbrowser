#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "net/ContentEncoding.h"
#include "net/HttpMessage.h"

// `Content-Encoding`, fed arbitrary bytes on both sides at once.
//
// Two hostile inputs meet here and the interesting bugs are in how they
// interact: the header names the codings, and the body is what each of them is
// asked to decode. A fuzzer that varied only the body would never reach the
// chained case, and one that varied only the header would never get past the
// first decode.
//
// The properties: no input reads out of bounds, no input decodes to more than
// the caller's bound however many codings it names, and a failed decode never
// leaves a partially-decoded body behind for the caller to parse. The bound is
// deliberately small so the fuzzer spends its time on framing rather than on
// megabytes of inflate output.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // The first byte picks the header, so the fuzzer can steer between codings
  // without having to discover the spelling of "gzip" by mutation.
  static constexpr std::string_view kCodings[] = {
      "gzip",     "deflate",       "identity",      "gzip, gzip",
      "br",       "GZIP",          "deflate, gzip", "identity, gzip, identity",
      "gzip,,",   "x-gzip",        "",              "gzip, deflate, gzip, deflate, gzip",
  };
  const std::string_view coding =
      kCodings[data[0] % (sizeof(kCodings) / sizeof(kCodings[0]))];

  microbrowser::net::HttpResponse response;
  response.status = 200;
  response.headers.Add("Content-Encoding", coding);
  response.headers.Add("Content-Length", std::to_string(size - 1));
  const auto* bytes = reinterpret_cast<const std::byte*>(data + 1);
  response.body.assign(bytes, bytes + (size - 1));

  microbrowser::net::DecodeLimits limits;
  limits.max_output = 1u << 20;
  limits.min_output = 4096;
  const auto status = microbrowser::net::DecodeContentEncoding(response, limits);
  if (status == microbrowser::net::DecodeStatus::Decoded) {
    // A decoded response describes itself honestly or not at all.
    if (response.headers.Has("content-encoding") || response.headers.Has("content-length")) {
      __builtin_trap();
    }
    if (response.body.size() > limits.max_output) {
      __builtin_trap();
    }
  }
  return 0;
}
