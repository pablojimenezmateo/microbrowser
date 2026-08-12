#include <string>
#include <vector>

#include "TestSupport.h"
#include "util/PercentEncoding.h"
#include "util/UrlEncoded.h"

// Percent-encoding and `application/x-www-form-urlencoded`.
//
// One test file because there is now one implementation. There were three
// percent-decoders -- url's, engine's, and the one bindings was about to need
// -- and the failure mode of a fourth is not a missing feature, it is two parts
// of the browser disagreeing about what a URL means. See util/PercentEncoding.h.

namespace microbrowser::tests {

namespace {

std::string Roundtrip(std::string_view query) {
  return util::SerializeUrlEncoded(util::ParseUrlEncoded(query));
}

}  // namespace

void RegisterUrlEncodedTests(std::vector<TestCase>& tests) {
  AddTest(tests, "UrlEncoded/PercentDecodingNeverFails", [] {
    ExpectEqString(util::PercentDecode("a%20b"), "a b", "a valid escape decodes");
    ExpectEqString(util::PercentDecode("a%zzb"), "a%zzb",
                   "an invalid escape is literal text, because that is what every other "
                   "browser does and disagreeing about what a URL means is a security bug");
    ExpectEqString(util::PercentDecode("a%"), "a%", "a truncated escape is literal too");
    ExpectEqString(util::PercentDecode("%41%42"), "AB", "consecutive escapes decode");
  });

  AddTest(tests, "UrlEncoded/ParsesTheFormsAQueryStringComesIn", [] {
    // A *query*, with no leading `?`. Stripping one here would be a second place the question mark
    // is removed -- `new URLSearchParams(location.search)` already does it, once -- and `??a=b` is
    // a query whose first parameter is genuinely named `?a`.
    std::vector<util::QueryPair> pairs = util::ParseUrlEncoded("a=1&b=2");
    ExpectEqInt(static_cast<long long>(pairs.size()), 2, "two pairs");
    ExpectEqString(pairs[0].first, "a", "first name");
    ExpectEqString(util::ParseUrlEncoded("?a=1")[0].first, "?a",
                   "a leading ? is part of the first name, because the caller strips it");
    ExpectEqString(pairs[1].second, "2", "second value");

    pairs = util::ParseUrlEncoded("flag&x=1");
    ExpectEqInt(static_cast<long long>(pairs.size()), 2, "a component with no = is still a pair");
    ExpectEqString(pairs[0].first, "flag", "the whole component is the name");
    ExpectEqString(pairs[0].second, "", "and the value is empty");

    pairs = util::ParseUrlEncoded("a=1&&b=2&");
    ExpectEqInt(static_cast<long long>(pairs.size()), 2, "empty components are skipped");

    pairs = util::ParseUrlEncoded("q=a+b%2Bc");
    ExpectEqString(pairs[0].second, "a b+c",
                   "a literal + is a space and an encoded one is a plus, which is why the "
                   "substitution happens after the split rather than before it");

    pairs = util::ParseUrlEncoded("a=1=2");
    ExpectEqString(pairs[0].second, "1=2", "only the first = splits");

    ExpectEqInt(static_cast<long long>(util::ParseUrlEncoded("").size()), 0, "an empty query has no pairs");
    ExpectEqInt(static_cast<long long>(util::ParseUrlEncoded("?").size()), 1,
                "a bare ? is a parameter named ?, because this parses a query rather than a search");
  });

  AddTest(tests, "UrlEncoded/SerializesTheSpecificationsKeepSet", [] {
    // Not PercentEncodeSet::Component, which keeps `!'()~`. The urlencoded
    // serializer keeps ASCII alphanumerics and `*-._` and nothing else. The
    // difference showed up as a form field with an apostrophe in it reaching
    // the server differently from every other browser.
    std::string out;
    util::AppendUrlEncodedPair("q", "it's a (test)~*-._", out);
    ExpectEqString(out, "q=it%27s+a+%28test%29%7E*-._", "the keep-set is exactly four symbols");

    out.clear();
    util::AppendUrlEncodedPair("a", "1", out);
    util::AppendUrlEncodedPair("b", "2", out);
    ExpectEqString(out, "a=1&b=2", "the separator goes between rather than before");

    out.clear();
    util::AppendUrlEncodedPair("n&m=", "x&y", out);
    ExpectEqString(out, "n%26m%3D=x%26y",
                   "a name carrying a separator must not become two pairs");
  });

  AddTest(tests, "UrlEncoded/RoundTripsWhatAPageRoundTrips", [] {
    // reddit's challenge reads location.search into a URLSearchParams and
    // submits the result as a form, so parse-then-serialize is a path a real
    // page takes and a lossy one would be silent.
    ExpectEqString(Roundtrip("token=a%2Bb&js_challenge=1"), "token=a%2Bb&js_challenge=1",
                   "an encoded plus survives the round trip");
    ExpectEqString(Roundtrip("q=hello+world"), "q=hello+world", "and so does a space");
    ExpectEqString(Roundtrip("utf8=%E2%9C%93"), "utf8=%E2%9C%93",
                   "multi-byte UTF-8 is encoded byte by byte");
  });
}

}  // namespace microbrowser::tests
