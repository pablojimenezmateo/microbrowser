#pragma once

#include <cstddef>

#include "net/HttpMessage.h"

namespace microbrowser::net {

// Undoing `Content-Encoding`, under a bound.
//
// This is the only place a response body grows, and a body that grows is the
// canonical amplification attack: a few hundred KB of zeros expand to gigabytes
// and the browser is out of memory before it has parsed anything. ADR 0010 says
// the answer is to bound it **twice** — an absolute ceiling and a maximum
// expansion ratio against what actually arrived — and to *fail* rather than
// truncate. A truncated document is a document the parser will happily misread,
// which is the argument ADR 0009 makes about half-understood programs.
//
// The ratio is the bound that matters. An absolute ceiling alone is passed by
// every bomb sized to sit just under it; the ratio is what makes a 200-byte
// response unable to become 64MB no matter what it claims.
struct DecodeLimits {
  // The ceiling, whatever the ratio allows. Matches HttpLimits::max_body: a
  // decoded body larger than the largest body we would have accepted uncoded is
  // not a body we would have accepted.
  std::size_t max_output = 64u * 1024u * 1024u;

  // Measured against the ADR 0010 table, whose best case is a 3.5MB stylesheet
  // in 369KB — 9.5x. Text of one repeated character compresses roughly 1000x,
  // which is what a bomb is made of, so 100 sits an order of magnitude above
  // anything real and an order of magnitude below anything hostile.
  std::size_t max_ratio = 100;

  // Below this the ratio does not apply. A 30-byte response expanding to 4KB is
  // a ratio of 130 and is also every small gzipped JSON reply on the web; the
  // floor is what keeps the ratio from being a rule about small responses
  // instead of a rule about amplification.
  std::size_t min_output = 64u * 1024u;

  // A `Content-Encoding` naming more codings than this is not a response, it is
  // a way to ask for n decompressions of one body.
  std::size_t max_codings = 4;
};

enum class DecodeStatus {
  // No coding, or `identity`. The body is untouched.
  Identity,
  // Decoded, and the headers that described the coded form are gone.
  Decoded,
  // A coding this browser never advertised. Handing the coded bytes to a parser
  // would render whatever they happen to look like, so this fails the response.
  UnsupportedCoding,
  // The bytes do not decode, their checksum does not hold, or they ran into the
  // bound without having declared where they were going.
  Malformed,
  // The member declared an uncompressed size past the bound, so it was refused
  // without decompressing at all. Reserved for that case: a decoder that stops
  // on the back reference which *would* have exceeded the ceiling cannot say
  // from its output length whether it was stopped or broken, and a status that
  // depends on which byte it happened to stop at is not a status.
  TooLarge,
};

// Decodes `response.body` in place.
//
// On success the `Content-Encoding` and `Content-Length` headers are removed:
// both describe the form the body arrived in, and a header that describes bytes
// the caller can no longer see is a lie waiting to be believed. The body's own
// size is the only length left, which is the one that cannot go stale.
DecodeStatus DecodeContentEncoding(HttpResponse& response, DecodeLimits limits = {});

// What `Accept-Encoding` says, and therefore exactly the set the function above
// accepts. One constant so the two cannot drift: advertising a coding we cannot
// decode turns every response using it into a failed load.
inline constexpr const char* kAcceptedContentEncodings = "gzip, deflate";

}  // namespace microbrowser::net
