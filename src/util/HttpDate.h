#pragma once

// HTTP-date (RFC 7231 §7.1.1.1), as cookies and cache headers write it.
//
// Cookie `Expires=` almost always carries an IMF-fix string
// (`Wed, 09 Nov 1994 08:49:37 GMT`), never a bare epoch. Parsing only digits
// left youtube's consent path unable to *delete* `TESTCOOKIESENABLED` or a
// stale `PREF` — the past-dated `Expires` was ignored, the write became a
// session cookie, and `document.cookie` kept showing the names the page
// thought it had cleared.

#include <cstdint>
#include <optional>
#include <string_view>

namespace microbrowser::util {

// Seconds since 1970-01-01 00:00:00 UTC. Nullopt when the text is not a
// recognised HTTP-date (or is outside what int64 can hold).
std::optional<std::int64_t> ParseHttpDate(std::string_view text);

}  // namespace microbrowser::util
