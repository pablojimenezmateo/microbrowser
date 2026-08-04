#pragma once

#include <string_view>

namespace microbrowser::util {

// What this browser calls itself, on the wire and to script.
//
// One string, and the same one in both places. It lives in util rather than in
// net because `navigator.userAgent` and the `User-Agent` request header must
// agree: a page that varies its markup by user agent and *also* branches on
// `navigator.userAgent` gets two different answers if these are two constants,
// and the bug that produces is a page that renders one way and scripts another.
// Neither net nor bindings may include the other, so the string goes underneath
// both.
//
// **It says what this browser is and nothing about the machine it is on** — no
// platform, no kernel, no version of anything installed, no build date. That is
// the same decision `Accept-Language: en-US` is: every copy of this browser
// sends the identical bytes, so the header carries zero bits about the user.
//
// It is also not a lie. Claiming to be Chrome would get better markup from
// sites that sniff, and it would be one line to write. It is refused because
// the whole point of a named compatibility target (ADR 0007) is to find out
// what this browser cannot do; a page served Chrome's markup and rendered by
// an engine that is not Chrome fails further from the cause. Measured against
// old.reddit.com, the honest string is served the same page as Chrome — what
// its edge blocks is a request with *no* User-Agent at all, not an unfamiliar
// one.
inline constexpr std::string_view kUserAgent = "microbrowser";

}  // namespace microbrowser::util
