#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::util {

// `application/x-www-form-urlencoded`, parsed and serialized.
//
// One implementation, because there are three callers and they must agree byte
// for byte: `engine` builds a form's data set with it, `bindings` implements
// `URLSearchParams` on it, and a page routinely round-trips a query string
// through both -- reddit's challenge reads `location.search` into a
// `URLSearchParams` and submits the result as a form. Two encoders that
// disagreed about `!` would make that round trip lossy in a way nothing tests.
//
// The serializer is *not* a percent-encode set from the URL standard. The
// urlencoded serializer keeps ASCII alphanumerics and `*-._` and nothing else,
// and writes a space as `+`. `PercentEncodeSet::Component` keeps `!'()~` as
// well, which is close enough to look right and wrong enough that a form field
// containing an apostrophe reaches the server differently from every other
// browser.
using QueryPair = std::pair<std::string, std::string>;

// Appends one `name=value`, with a separating `&` when `out` is not empty.
void AppendUrlEncodedPair(std::string_view name, std::string_view value, std::string& out);

std::string SerializeUrlEncoded(const std::vector<QueryPair>& pairs);

// The WHATWG urlencoded parser. Never fails: a component with no `=` is a name
// with an empty value, an empty component is skipped, and a malformed escape is
// the literal bytes -- which is what PercentDecode already promises and what
// every other browser does.
//
// A leading `?` is accepted and ignored, because that is what
// `new URLSearchParams(location.search)` hands it.
std::vector<QueryPair> ParseUrlEncoded(std::string_view input);

}  // namespace microbrowser::util
