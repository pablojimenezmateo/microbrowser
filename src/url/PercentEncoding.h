#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace microbrowser::url {

// Percent-encoding, as the WHATWG URL Standard defines it.
//
// The standard does not have "a" percent-encode set, it has six, and which one
// applies depends on where in the URL the byte sits and whether the scheme is
// special. Getting that wrong is not cosmetic: a `#` that should have been
// encoded in a query truncates the URL at a fragment boundary, and a `/` that
// should have been encoded in userinfo moves the host.
//
// Named exactly as the specification names them, so a reader can check this
// file against section 1.3 line by line.
enum class PercentEncodeSet : std::uint8_t {
  C0Control,
  Fragment,
  Query,
  SpecialQuery,
  Path,
  Userinfo,
  Component,
};

bool ShouldPercentEncode(unsigned char byte, PercentEncodeSet set);

// Appends `input`, percent-encoding every byte in `set`. UTF-8 is encoded byte
// by byte, which is what the standard says and is why this takes bytes rather
// than code points.
void PercentEncodeInto(std::string_view input, PercentEncodeSet set, std::string& out);

std::string PercentEncode(std::string_view input, PercentEncodeSet set);

// Percent-*decoding* never fails. `%zz` is not an error, it is the literal
// three characters, because that is what the standard says and because a
// decoder that rejected it would disagree with every other browser about what a
// URL means — and disagreeing about what a URL means is a security bug, not a
// compatibility one.
std::string PercentDecode(std::string_view input);

}  // namespace microbrowser::url
