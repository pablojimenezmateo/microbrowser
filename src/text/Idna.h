#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::text {

// UTS #46, "Unicode IDNA Compatibility Processing" — how a domain name written in a human script
// becomes the ASCII a DNS query and a TLS certificate are made of.
//
// **This is a security boundary, not an internationalization feature.** The host that gets
// connected to must be the host that was checked, and every step below is a way those two could
// come apart: `Ｇｏ.com` maps to `go.com`, a soft hyphen inside a label is *ignored* rather than
// escaped, and U+3002 IDEOGRAPHIC FULL STOP separates labels exactly like a dot. A parser that
// passed any of those through unchanged would compare one string and resolve another, which is the
// shape of every homograph and origin-confusion bug there has ever been.
//
// It lives in `src/text` rather than in `src/url` because it is a Unicode Technical Standard
// algorithm over Unicode character properties, which is what this module is — UAX #9 and UAX #14
// are already here — and because it needs three tables this module already owns or generates:
// bidi classes, combining classes, and normalization. `src/url` asks it one question.
//
// This is UTS #46's ToASCII and only that. The URL Standard's own "domain to ASCII" wraps it with
// two web-compatibility rules — an all-ASCII domain is lowercased whatever this says of it, and the
// result is then checked for forbidden domain code points — and those live in `src/url/Host.cpp`,
// where a URL is what is being parsed. Keeping them apart is what makes this function checkable
// against UTS #46 line by line.
//
// The parameters the URL Standard does not vary are fixed rather than exposed: CheckBidi and
// CheckJoiners are true, Transitional_Processing and IgnoreInvalidPunycode are false. `be_strict`
// is the one knob it turns, and it turns three at once — CheckHyphens, UseSTD3ASCIIRules and
// VerifyDnsLength.

// Nullopt is the standard's failure value. There is no partial result: a domain that half-converted
// is a domain this code and a resolver would disagree about.
std::optional<std::string> UnicodeToAscii(std::string_view domain, bool be_strict);

}  // namespace microbrowser::text
