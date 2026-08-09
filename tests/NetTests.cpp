#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "net/ContentEncoding.h"
#include "net/CookieJar.h"
#include "net/HttpMessage.h"
#include "net/ResolverCache.h"
#include "util/PerformanceCounters.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::tests {

using net::Cookie;
using net::CookieJar;
using net::HttpHeaders;
using net::ResponseParser;
using net::SameSite;
using url::ContainerId;
using url::PartitionKey;
using url::Site;
using url::Url;

namespace {

std::span<const std::byte> Bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string BodyOf(const ResponseParser& parser) {
  const auto& body = parser.Response().body;
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

// Feeds a whole response in one go.
ResponseParser ParseWhole(std::string_view text) {
  ResponseParser parser;
  parser.Consume(Bytes(text));
  return parser;
}

Url MustParse(std::string_view text) {
  const auto url = Url::Parse(text);
  Expect(url.has_value(), std::string("expected to parse: ") + std::string(text));
  return *url;
}

PartitionKey KeyFor(std::string_view url, ContainerId container = ContainerId::Default()) {
  return PartitionKey::ForTopLevel(container, MustParse(url));
}

// --- Content coding fixtures, all written by zlib -----------------------------

// gzip("hello hello hello world")
constexpr std::uint8_t kGzipHello[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xCB, 0x48,
    0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x22, 0xCB, 0xF3, 0x8B, 0x72, 0x52,
    0x00, 0x26, 0xE6, 0x5A, 0x81, 0x17, 0x00, 0x00, 0x00};

// zlib("hello hello hello world") -- what RFC 9110 means by `deflate`.
constexpr std::uint8_t kZlibHello[] = {0x78, 0xDA, 0xCB, 0x48, 0xCD, 0xC9, 0xC9,
                                       0x57, 0xC8, 0x40, 0x22, 0xCB, 0xF3, 0x8B,
                                       0x72, 0x52, 0x00, 0x68, 0x7D, 0x08, 0xC5};

// gzip(gzip("hello hello hello world")), for `Content-Encoding: gzip, gzip`.
constexpr std::uint8_t kDoubleGzipHello[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x93, 0xEF, 0xE6,
    0x60, 0x00, 0x01, 0x26, 0xE6, 0xD3, 0x1E, 0x67, 0x4F, 0x9E, 0x0C, 0x3F, 0xE1,
    0xA0, 0x74, 0xFA, 0x73, 0x77, 0x51, 0x10, 0x83, 0xDA, 0xB3, 0xA8, 0x46, 0x71,
    0xA0, 0x04, 0x00, 0x8F, 0x9D, 0xE4, 0x13, 0x21, 0x00, 0x00, 0x00};

// 132 bytes that become 100,000. A ratio of 758, which is what a bomb is: not
// large in itself, large only relative to what arrived.
constexpr std::uint8_t kGzipBomb[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xED, 0xC1, 0x31, 0x01, 0x00,
    0x00, 0x00, 0xC2, 0xA0, 0xF5, 0x4F, 0x6D, 0x0D, 0x0F, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x57, 0x03, 0x7D, 0x95, 0x11, 0xD4, 0xA0, 0x86, 0x01, 0x00};

net::HttpResponse Coded(std::string_view coding, const std::uint8_t* data, std::size_t size) {
  net::HttpResponse response;
  response.status = 200;
  if (!coding.empty()) {
    response.headers.Add("Content-Encoding", coding);
  }
  response.headers.Add("Content-Length", std::to_string(size));
  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  response.body.assign(bytes, bytes + size);
  return response;
}

std::string BodyText(const net::HttpResponse& response) {
  return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

}  // namespace

void RegisterNetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Http/ParsesASimpleResponse", [] {
    util::ResetPerformanceCounters();
    ResponseParser parser = ParseWhole(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nhello");
    Expect(parser.IsComplete(), "a complete response parses");
    ExpectEqInt(parser.Response().status, 200, "status");
    ExpectEqString(parser.Response().reason, "OK", "reason phrase");
    ExpectEqString(std::string(*parser.Response().headers.Get("content-type")), "text/html",
                   "header lookup is case-insensitive");
    ExpectEqString(BodyOf(parser), "hello", "body");
    ExpectEqInt(static_cast<long long>(util::ReadPerformanceCounter(
                    util::PerfCounterId::NetResponsesParsed)),
                1, "fixed-length completions count as parsed responses");
  });

  AddTest(tests, "Http/ParsesAcrossArbitrarySplits", [] {
    // Responses arrive in whatever pieces the network hands over, and a parser
    // that only works on whole messages is one that buffers unbounded attacker
    // data before it can reject anything.
    const std::string message =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nX-A: 1\r\n\r\nabc";
    for (std::size_t split = 1; split < message.size(); ++split) {
      ResponseParser parser;
      Expect(parser.Consume(Bytes(std::string_view(message).substr(0, split))), "first half");
      Expect(parser.Consume(Bytes(std::string_view(message).substr(split))), "second half");
      Expect(parser.IsComplete(),
             std::string("split at ") + std::to_string(split) + " did not complete");
      ExpectEqString(BodyOf(parser), "abc", "body survives the split");
    }
  });

  AddTest(tests, "Http/ParsesChunkedBodies", [] {
    ResponseParser parser = ParseWhole(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n");
    Expect(parser.IsComplete(), "a chunked response completes at the zero chunk");
    ExpectEqString(BodyOf(parser), "hello world", "chunks are concatenated");
  });

  AddTest(tests, "Http/ChunkExtensionsAndTrailersAreToleratedNotMerged", [] {
    ResponseParser parser = ParseWhole(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3;ext=value\r\nabc\r\n"
        "0\r\n"
        "X-Trailer: late\r\n\r\n");
    Expect(parser.IsComplete(), "extensions and trailers do not break the framing");
    ExpectEqString(BodyOf(parser), "abc", "body");
    Expect(!parser.Response().headers.Has("x-trailer"),
           "a trailer must not join the header set; a server could otherwise change "
           "Content-Type after the body was already being interpreted");
  });

  AddTest(tests, "Http/ChunkDataMustEndWithCrlf", [] {
    ResponseParser parser = ParseWhole(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello"
        "6\r\n world\r\n"
        "0\r\n\r\n");
    Expect(parser.Failed(),
           "a chunk's declared bytes must be followed by CRLF before the next chunk size");
  });

  // Request smuggling, which is the reason this parser exists rather than a
  // simpler one. Every documented attack is two intermediaries resolving an
  // ambiguous framing differently, so an ambiguous framing is refused.
  AddTest(tests, "Http/RefusesAmbiguousMessageFraming", [] {
    ResponseParser both = ParseWhole(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello");
    Expect(both.Failed(), "Content-Length and Transfer-Encoding together is unresolvable");

    ResponseParser duplicate = ParseWhole(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello");
    Expect(duplicate.Failed(), "two Content-Lengths that disagree");

    ResponseParser same = ParseWhole(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    Expect(same.Failed(),
           "and two that agree, because the benefit of accepting it is zero and the cost is "
           "a special case that the next parser may not share");

    ResponseParser folded = ParseWhole(
        "HTTP/1.1 200 OK\r\nX-Long: one\r\n  two\r\nContent-Length: 0\r\n\r\n");
    Expect(folded.Failed(),
           "obsolete line folding is rejected rather than unfolded; two parsers that unfold "
           "differently disagree about the header set");

    ResponseParser space_before_colon =
        ParseWhole("HTTP/1.1 200 OK\r\nX-Bad : v\r\nContent-Length: 0\r\n\r\n");
    Expect(space_before_colon.Failed(), "a space before the colon is not a header name");

    ResponseParser weird_encoding = ParseWhole(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, identity\r\n\r\n0\r\n\r\n");
    Expect(weird_encoding.Failed(), "an encoding list we do not implement is not guessed at");

    ResponseParser duplicate_encoding = ParseWhole(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n\r\n"
        "0\r\n\r\n");
    Expect(duplicate_encoding.Failed(), "two Transfer-Encoding headers are ambiguous too");
  });

  AddTest(tests, "Http/RejectsMalformedStartAndHeaders", [] {
    Expect(ParseWhole("garbage\r\n\r\n").Failed(), "not a status line");
    Expect(ParseWhole("HTTP/9.9 200 OK\r\n\r\n").Failed(), "unsupported version");
    Expect(ParseWhole("HTTP/1.1 999 X\r\n\r\n").Failed(), "status code out of range");
    Expect(ParseWhole("HTTP/1.1 abc X\r\n\r\n").Failed(), "non-numeric status");
    Expect(ParseWhole("HTTP/1.1 200OK\r\n\r\n").Failed(), "status code needs a delimiter");
    Expect(ParseWhole("HTTP/1.1 200\tOK\r\n\r\n").Failed(), "and that delimiter is a space");
    Expect(ParseWhole("HTTP/1.1 200 OK\r\nnocolon\r\n\r\n").Failed(), "header with no colon");
  });

  AddTest(tests, "Http/EnforcesItsLimitsWhileParsingRatherThanAfter", [] {
    net::HttpLimits limits;
    limits.max_body = 8;
    limits.max_header_count = 2;

    ResponseParser big_body(limits);
    big_body.Consume(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));
    Expect(big_body.Failed(),
           "an oversized Content-Length must fail on the header, before a byte of body is "
           "read or reserved");

    ResponseParser many_headers(limits);
    many_headers.Consume(Bytes("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n"));
    Expect(many_headers.Failed(), "too many headers");

    ResponseParser big_chunk(limits);
    big_chunk.Consume(
        Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFF\r\n"));
    Expect(big_chunk.Failed(), "a chunk larger than the body limit fails on its size line");
  });

  AddTest(tests, "Http/StatusCodesWithNoBodyDoNotWaitForOne", [] {
    ResponseParser no_content = ParseWhole("HTTP/1.1 204 No Content\r\n\r\n");
    Expect(no_content.IsComplete(), "204 has no body by definition");
    ResponseParser not_modified = ParseWhole("HTTP/1.1 304 Not Modified\r\n\r\n");
    Expect(not_modified.IsComplete(), "and neither does 304");
  });

  AddTest(tests, "Http/ABodyDelimitedByCloseCompletesOnlyWhenClosed", [] {
    ResponseParser parser;
    parser.Consume(Bytes("HTTP/1.0 200 OK\r\n\r\nsome bytes"));
    Expect(!parser.IsComplete(), "without a length, the body runs until the connection closes");
    Expect(parser.Finish(), "and closing completes it");
    ExpectEqString(BodyOf(parser), "some bytes", "body");

    ResponseParser truncated;
    truncated.Consume(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort"));
    Expect(!truncated.Finish(),
           "but a length-delimited body cut short is a failure, not a short body");
  });

  AddTest(tests, "Http/HeadersRefuseInjection", [] {
    HttpHeaders headers;
    Expect(headers.Add("X-Ok", "value"), "an ordinary header");
    Expect(!headers.Add("X-Bad", "a\r\nInjected: yes"), "CRLF in a value is header injection");
    Expect(!headers.Add("X-Bad", "a\nInjected: yes"), "a bare LF is too");
    Expect(!headers.Add("Bad Name", "v"), "a space is not a tchar");
    Expect(!headers.Add("", "v"), "and a header must have a name");
    ExpectEqInt(static_cast<long long>(headers.Size()), 1,
                "a rejected header is not added in some sanitized form; sanitized is a "
                "judgment the next parser may not share");
  });

  AddTest(tests, "Http/SerializesARequest", [] {
    HttpHeaders headers;
    headers.Add("Host", "example.com");
    headers.Add("Accept", "*/*");
    ExpectEqString(net::SerializeRequest("GET", "/index.html", headers),
                   "GET /index.html HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n",
                   "request serialization");
  });

  // --- Cookies --------------------------------------------------------------

  AddTest(tests, "Cookie/ParsesNameValueAndAttributes", [] {
    const Url url = MustParse("https://example.com/a/b");
    const auto cookie = net::ParseSetCookie(
        "id=42; Path=/a; Secure; HttpOnly; SameSite=Strict; Max-Age=60", url, 1000);
    Expect(cookie.has_value(), "a well-formed cookie parses");
    ExpectEqString(cookie->name, "id", "name");
    ExpectEqString(cookie->value, "42", "value");
    ExpectEqString(cookie->path, "/a", "explicit path");
    Expect(cookie->secure && cookie->http_only, "flags");
    Expect(cookie->same_site == SameSite::Strict, "same-site");
    Expect(cookie->expires.has_value() && *cookie->expires == 1060, "Max-Age is relative to now");
  });

  AddTest(tests, "Cookie/DefaultsAreTheConservativeOnes", [] {
    const Url url = MustParse("https://example.com/a/b");
    const auto cookie = net::ParseSetCookie("id=42", url, 0);
    Expect(cookie.has_value(), "parses");
    ExpectEqString(cookie->path, "/a",
                   "the default path is the directory, not the whole site");
    Expect(cookie->host_only, "a cookie with no Domain is host-only");
    Expect(cookie->same_site == SameSite::Lax,
           "SameSite defaults to Lax; defaulting to None would hand a CSRF token to anyone "
           "who never thought about it");
    Expect(!cookie->expires.has_value(), "and with no Max-Age it is a session cookie");
  });

  AddTest(tests, "Cookie/RefusesSameSiteNoneWithoutSecure", [] {
    const Url url = MustParse("https://example.com/");
    Expect(!net::ParseSetCookie("id=1; SameSite=None", url, 0).has_value(),
           "SameSite=None without Secure asks to be sent cross-site over plain HTTP, which "
           "is a tracking cookie with the safety catch removed");
    Expect(net::ParseSetCookie("id=1; SameSite=None; Secure", url, 0).has_value(),
           "with Secure it is allowed");
  });

  AddTest(tests, "Cookie/EnforcesSecureAndHostPrefixes", [] {
    const Url url = MustParse("https://example.com/");

    Expect(!net::ParseSetCookie("__Secure-id=1", url, 0).has_value(),
           "__Secure- names require the Secure attribute");
    Expect(net::ParseSetCookie("__Secure-id=1; Secure", url, 0).has_value(),
           "and are accepted when Secure is present");

    Expect(!net::ParseSetCookie("__Host-id=1; Secure; Domain=example.com; Path=/", url, 0)
                .has_value(),
           "__Host- names must be host-only");
    Expect(!net::ParseSetCookie("__Host-id=1; Secure; Path=/account", url, 0).has_value(),
           "__Host- names must be scoped to /");
    Expect(!net::ParseSetCookie("__Host-id=1; Path=/", url, 0).has_value(),
           "__Host- names also require Secure");
    Expect(net::ParseSetCookie("__Host-id=1; Secure; Path=/", url, 0).has_value(),
           "the prefix is accepted only when every invariant holds");
  });

  AddTest(tests, "Cookie/DomainMatchingStopsAtALabelBoundary", [] {
    Expect(net::CookieDomainMatches("example.com", "example.com"), "exact");
    Expect(net::CookieDomainMatches("a.example.com", "example.com"), "subdomain");
    Expect(net::CookieDomainMatches("a.example.com.", "example.com"),
           "a root dot on the request host is DNS syntax, not a different cookie boundary");
    Expect(net::CookieDomainMatches("a.example.com", "example.com."),
           "and a root dot on the Domain attribute is normalized the same way");
    Expect(!net::CookieDomainMatches("notexample.com", "example.com"),
           "a suffix that does not fall on a label boundary must not match — this is the "
           "whole attack, and a naive string comparison says it does");
    Expect(!net::CookieDomainMatches("example.com", "a.example.com"), "and not upward");
  });

  AddTest(tests, "Cookie/PathMatchingIsNotAPrefixComparison", [] {
    Expect(net::CookiePathMatches("/foo", "/foo"), "exact");
    Expect(net::CookiePathMatches("/foo/bar", "/foo"), "a subdirectory");
    Expect(!net::CookiePathMatches("/foobar", "/foo"),
           "but not a path that merely starts with the same characters");
    Expect(net::CookiePathMatches("/foo/bar", "/foo/"), "a trailing slash still matches");
  });

  AddTest(tests, "Cookie/APageCannotSetACookieForADomainItDoesNotOwn", [] {
    CookieJar jar;
    const Url url = MustParse("https://a.example.com/");
    const PartitionKey key = KeyFor("https://a.example.com/");

    Expect(jar.StoreFromHeader(key, url, "id=1; Domain=example.com", 0),
           "widening to its own registrable domain is allowed");
    Expect(!jar.StoreFromHeader(key, url, "id=2; Domain=other.com", 0),
           "but not to an unrelated domain");
    Expect(!jar.StoreFromHeader(key, url, "id=3; Domain=com", 0),
           "and never to a public suffix, or any site could set a cookie every other site "
           "would read");
  });

  AddTest(tests, "Cookie/RootDotDoesNotBypassDomainMatching", [] {
    CookieJar jar;
    const Url absolute = MustParse("https://a.example.com./");
    const PartitionKey key = KeyFor("https://a.example.com./");

    Expect(jar.StoreFromHeader(key, absolute, "id=1; Domain=example.com", 0),
           "an absolute request host can still set a parent-domain cookie");
    ExpectEqString(jar.HeaderFor(key, absolute, true, true, 0), "id=1",
                   "and the same normalized domain match sends it back");

    Expect(!jar.StoreFromHeader(key, absolute, "registry=1; Domain=com.", 0),
           "a root dot must not hide a public-suffix cookie");
  });

  AddTest(tests, "Cookie/ASecureCookieCannotBeSetOverPlainHttp", [] {
    CookieJar jar;
    const Url insecure = MustParse("http://example.com/");
    Expect(!jar.StoreFromHeader(KeyFor("http://example.com/"), insecure, "id=1; Secure", 0),
           "otherwise a network attacker plants a cookie the https site then trusts");
  });

  // Total Cookie Protection, at the storage layer.
  AddTest(tests, "Cookie/PartitionsAreSeparateJars", [] {
    CookieJar jar;
    const Url tracker = MustParse("https://tracker.example/");
    const PartitionKey on_news = PartitionKey::ForEmbedded(
        ContainerId::Default(), Site::FromUrl(MustParse("https://news.example/")), tracker);
    const PartitionKey on_shop = PartitionKey::ForEmbedded(
        ContainerId::Default(), Site::FromUrl(MustParse("https://shop.example/")), tracker);

    Expect(jar.StoreFromHeader(on_news, tracker, "uid=abc", 0), "stored under one top-level site");
    ExpectEqString(jar.HeaderFor(on_news, tracker, true, false, 0), "uid=abc", "and read back");
    ExpectEqString(jar.HeaderFor(on_shop, tracker, true, false, 0), "",
                   "but invisible under another, so the same third party cannot correlate the "
                   "two visits");
  });

  AddTest(tests, "Cookie/ContainersAreSeparateJarsAndCanBeClearedWholesale", [] {
    CookieJar jar;
    const Url url = MustParse("https://example.com/");
    const PartitionKey work = KeyFor("https://example.com/", ContainerId{1});
    const PartitionKey personal = KeyFor("https://example.com/", ContainerId{2});

    jar.StoreFromHeader(work, url, "who=work", 0);
    jar.StoreFromHeader(personal, url, "who=personal", 0);
    ExpectEqString(jar.HeaderFor(work, url, true, true, 0), "who=work", "one container");
    ExpectEqString(jar.HeaderFor(personal, url, true, true, 0), "who=personal", "and the other");

    jar.ClearContainer(ContainerId{2});
    ExpectEqString(jar.HeaderFor(personal, url, true, true, 0), "",
                   "closing a private window is this call and nothing else");
    ExpectEqString(jar.HeaderFor(work, url, true, true, 0), "who=work",
                   "and must not touch any other container");
  });

  AddTest(tests, "Cookie/SameSiteControlsCrossSiteSending", [] {
    CookieJar jar;
    const Url url = MustParse("https://example.com/");
    const PartitionKey key = KeyFor("https://example.com/");
    jar.StoreFromHeader(key, url, "s=1; SameSite=Strict", 0);
    jar.StoreFromHeader(key, url, "l=1; SameSite=Lax", 0);
    jar.StoreFromHeader(key, url, "n=1; SameSite=None; Secure", 0);

    ExpectEqString(jar.HeaderFor(key, url, true, false, 0), "s=1; l=1; n=1",
                   "same-site sends everything");
    ExpectEqString(jar.HeaderFor(key, url, false, true, 0), "l=1; n=1",
                   "a cross-site top-level navigation sends Lax but not Strict");
    ExpectEqString(jar.HeaderFor(key, url, false, false, 0), "n=1",
                   "and a cross-site subresource sends only None, which is what makes Lax a "
                   "CSRF defence rather than a label");
  });

  AddTest(tests, "Cookie/FirstPartySameSiteOriginsShareJar", [] {
    // youtube Accept: document.cookie sets SOCS on www, then fetch POSTs
    // consent.youtube.com/save. Both are first-party under top-level youtube.
    CookieJar jar;
    const Url www = MustParse("https://www.youtube.com/");
    const Url consent = MustParse("https://consent.youtube.com/save");
    const Url apex = MustParse("https://youtube.com/");
    const Site youtube = Site::FromUrl(www);
    const PartitionKey www_key = PartitionKey::ForTopLevel(ContainerId::Default(), www);
    const PartitionKey consent_key =
        PartitionKey::ForEmbedded(ContainerId::Default(), youtube, consent);
    const PartitionKey apex_key =
        PartitionKey::ForEmbedded(ContainerId::Default(), youtube, apex);

    Expect(www_key.IsFirstParty() && consent_key.IsFirstParty() && apex_key.IsFirstParty(),
           "www, consent and apex are first-party under the youtube site");
    Expect(!(www_key == consent_key),
           "the keys still differ by origin — that is what used to empty the Cookie header");

    Expect(jar.StoreFromDocument(www_key, www,
                                 "SOCS=ok; Domain=.youtube.com; Path=/; SameSite=Lax", 0),
           "script sets a domain cookie on www");
    ExpectEqString(jar.HeaderFor(consent_key, consent, true, false, 0), "SOCS=ok",
                   "the same first-party jar sends it to consent.youtube.com");
    ExpectEqString(jar.HeaderFor(apex_key, apex, true, false, 0), "SOCS=ok",
                   "and to the apex host");
    ExpectEqString(jar.DocumentCookie(www_key, www, 0), "SOCS=ok",
                   "document.cookie still reads it");

    // Host-only cookies stay host-only: Domain matching, not the partition.
    Expect(jar.StoreFromDocument(www_key, www, "hostonly=1; Path=/", 0), "host-only on www");
    Expect(jar.HeaderFor(consent_key, consent, true, false, 0).find("hostonly") ==
               std::string::npos,
           "host-only www cookies do not travel to consent");

    // Third-party under a different top-level site stays isolated (TCP).
    const Url other_page = MustParse("https://news.ycombinator.com/");
    const PartitionKey tracker_on_hn = PartitionKey::ForEmbedded(
        ContainerId::Default(), Site::FromUrl(other_page), MustParse("https://www.youtube.com/pixel"));
    ExpectEqString(jar.HeaderFor(tracker_on_hn, www, false, false, 0), "",
                   "a youtube origin under HN's top-level site does not see youtube's jar");
  });

  AddTest(tests, "Cookie/ExpiryRemovesAndReplacementDoesNotDuplicate", [] {
    CookieJar jar;
    const Url url = MustParse("https://example.com/");
    const PartitionKey key = KeyFor("https://example.com/");

    jar.StoreFromHeader(key, url, "id=first; Max-Age=100", 0);
    jar.StoreFromHeader(key, url, "id=second; Max-Age=100", 0);
    ExpectEqInt(static_cast<long long>(jar.Size()), 1,
                "a cookie is identified by name, domain and path; a jar that appended would "
                "grow forever and send both");
    ExpectEqString(jar.HeaderFor(key, url, true, true, 0), "id=second", "the newer value wins");

    ExpectEqString(jar.HeaderFor(key, url, true, true, 200), "",
                   "and an expired cookie is not sent");

    jar.StoreFromHeader(key, url, "id=gone; Max-Age=0", 0);
    ExpectEqInt(static_cast<long long>(jar.Size()), 0,
                "setting an expired cookie is how a server deletes one");
  });

  AddTest(tests, "Cookie/MaxAgeAdditionSaturatesInsteadOfWrapping", [] {
    const Url url = MustParse("https://example.com/");
    const auto cookie =
        net::ParseSetCookie("id=long; Max-Age=9223372036854775807", url, 1000);
    Expect(cookie.has_value(), "the cookie parses");
    Expect(cookie->expires.has_value() &&
               *cookie->expires == std::numeric_limits<std::int64_t>::max(),
           "a huge Max-Age is clamped to the latest representable expiry rather than wrapping "
           "negative");
  });

  AddTest(tests, "Cookie/OrdersByPathLengthThenAge", [] {
    CookieJar jar;
    const Url deep = MustParse("https://example.com/a/b/");
    const PartitionKey key = KeyFor("https://example.com/");
    jar.StoreFromHeader(key, deep, "root=1; Path=/", 0);
    jar.StoreFromHeader(key, deep, "deep=1; Path=/a/b", 0);
    ExpectEqString(jar.HeaderFor(key, deep, true, true, 0), "deep=1; root=1",
                   "RFC 6265 orders longer paths first, and servers depend on it");
  });

  AddTest(tests, "Cookie/RejectsControlCharacters", [] {
    const Url url = MustParse("https://example.com/");
    Expect(!net::ParseSetCookie(std::string_view("a\rb=1", 5), url, 0).has_value(),
           "a control character in a name is injection aimed at whatever reads the jar out");
    Expect(!net::ParseSetCookie(std::string_view("a=1\x01", 4), url, 0).has_value(),
           "and in a value");
    Expect(!net::ParseSetCookie("=novalue", url, 0).has_value(), "a cookie must have a name");
  });

  AddTest(tests, "Cookie/DocumentCookieHidesHttpOnlyAndPartitions", [] {
    CookieJar jar;
    const Url url = MustParse("https://example.com/app");
    const PartitionKey key = KeyFor("https://example.com/");
    const PartitionKey other = KeyFor("https://other.example/");
    jar.StoreFromHeader(key, url, "visible=1", 0);
    jar.StoreFromHeader(key, url, "secret=2; HttpOnly", 0);
    jar.StoreFromHeader(other, url, "other=3", 0);

    ExpectEqString(jar.DocumentCookie(key, url, 0), "visible=1",
                   "script sees non-HttpOnly cookies for its partition only");
    ExpectEqString(jar.DocumentCookie(other, url, 0), "other=3",
                   "and not another partition's jar");
  });

  AddTest(tests, "Cookie/DocumentCookieSetterUpdatesAndIgnoresHttpOnly", [] {
    CookieJar jar;
    const Url url = MustParse("https://example.com/");
    const PartitionKey key = KeyFor("https://example.com/");
    Expect(jar.StoreFromDocument(key, url, "csrf=abc", 0), "a script write stores");
    ExpectEqString(jar.DocumentCookie(key, url, 0), "csrf=abc", "and reads back");
    Expect(jar.StoreFromDocument(key, url, "csrf=def; HttpOnly", 0),
           "HttpOnly in a script write is ignored, not a refusal");
    ExpectEqString(jar.DocumentCookie(key, url, 0), "csrf=def",
                   "and the cookie is still visible to script");
    Expect(jar.HeaderFor(key, url, true, true, 0).find("csrf=def") != std::string::npos,
           "requests still send it");
  });

  AddTest(tests, "Cookie/HttpDateExpiresDeletesLikeABrowser", [] {
    // youtube's consent probe writes TESTCOOKIESENABLED then clears it with a
    // past IMF-fix Expires. Digits-only parsing left the empty name in the
    // jar forever and consent reported "error saving your choice".
    CookieJar jar;
    const Url url = MustParse("https://www.youtube.com/watch?v=x");
    const PartitionKey key = KeyFor("https://www.youtube.com/");
    const std::int64_t now = 1'776'000'000;  // ~2026-04
    Expect(jar.StoreFromDocument(
               key, url, "TESTCOOKIESENABLED=1;expires=Sun, 09 Aug 2026 06:26:50 GMT", now),
           "future HTTP-date stores");
    ExpectEqString(jar.DocumentCookie(key, url, now), "TESTCOOKIESENABLED=1", "readable");
    Expect(jar.StoreFromDocument(
               key, url, "TESTCOOKIESENABLED=;expires=Sat, 31 Jan 1970 23:00:00 GMT", now),
           "past HTTP-date is accepted");
    ExpectEqString(jar.DocumentCookie(key, url, now), "", "and removes the cookie");
  });

  // --- Content coding -------------------------------------------------------

  AddTest(tests, "ContentEncoding/GzipIsDecodedAndTheHeadersStopDescribingTheWire", [] {
    net::HttpResponse response = Coded("gzip", kGzipHello, sizeof(kGzipHello));
    Expect(net::DecodeContentEncoding(response) == net::DecodeStatus::Decoded, "gzip decodes");
    ExpectEqString(BodyText(response), "hello hello hello world", "the body is the plain form");
    Expect(!response.headers.Has("content-encoding"),
           "a Content-Encoding left behind would make a second pass decode a plain body");
    Expect(!response.headers.Has("content-length"),
           "and a Content-Length that describes the wire form is a lie about the body");
  });

  AddTest(tests, "ContentEncoding/DeflateMeansZlibAndAlsoMeansRaw", [] {
    net::HttpResponse wrapped = Coded("deflate", kZlibHello, sizeof(kZlibHello));
    Expect(net::DecodeContentEncoding(wrapped) == net::DecodeStatus::Decoded,
           "the zlib wrapper is what the specification says deflate is");
    ExpectEqString(BodyText(wrapped), "hello hello hello world", "and it decodes");

    // The raw form, which a meaningful share of servers send under the same
    // name. Every browser accepts both, so a page cannot be built around us
    // rejecting one.
    net::HttpResponse raw = Coded("DEFLATE", kZlibHello + 2, sizeof(kZlibHello) - 6);
    Expect(net::DecodeContentEncoding(raw) == net::DecodeStatus::Decoded,
           "and the raw stream servers actually send under that name decodes too");
    ExpectEqString(BodyText(raw), "hello hello hello world", "case-insensitively");
  });

  AddTest(tests, "ContentEncoding/CodingsComeOffInTheOrderTheyWentOn", [] {
    net::HttpResponse response = Coded("gzip, gzip", kDoubleGzipHello, sizeof(kDoubleGzipHello));
    Expect(net::DecodeContentEncoding(response) == net::DecodeStatus::Decoded,
           "a chain is applied left to right and comes off right to left");
    ExpectEqString(BodyText(response), "hello hello hello world", "both layers");
  });

  AddTest(tests, "ContentEncoding/AnAbsentOrIdentityCodingLeavesTheBodyAlone", [] {
    net::HttpResponse none = Coded("", kGzipHello, sizeof(kGzipHello));
    Expect(net::DecodeContentEncoding(none) == net::DecodeStatus::Identity, "no coding, no change");
    ExpectEqInt(static_cast<long long>(none.body.size()), sizeof(kGzipHello),
                "the body is untouched");
    Expect(none.headers.Has("content-length"),
           "and a response that was not decoded keeps the headers that still describe it");

    net::HttpResponse identity = Coded("identity", kGzipHello, sizeof(kGzipHello));
    Expect(net::DecodeContentEncoding(identity) == net::DecodeStatus::Identity,
           "identity is spelled out by some servers and means the same thing");
  });

  // The bound is the whole reason this function exists, and the ratio is the
  // half of it that catches a bomb sized to sit under the ceiling.
  AddTest(tests, "ContentEncoding/AnExpansionRatioIsRefusedRatherThanTruncated", [] {
    net::HttpResponse response = Coded("gzip", kGzipBomb, sizeof(kGzipBomb));
    Expect(net::DecodeContentEncoding(response) == net::DecodeStatus::TooLarge,
           "132 bytes may not become 100,000 under a ratio bound of 100");
    Expect(response.body.size() != 65536,
           "and must not come back truncated at the bound: a short document is one a parser "
           "will happily misread");
  });

  AddTest(tests, "ContentEncoding/TheAbsoluteCeilingBindsIndependently", [] {
    net::DecodeLimits limits;
    limits.max_output = 1024;
    limits.min_output = 1024;
    net::HttpResponse response = Coded("gzip", kGzipBomb, sizeof(kGzipBomb));
    Expect(net::DecodeContentEncoding(response, limits) == net::DecodeStatus::TooLarge,
           "the ceiling applies whatever the ratio would have allowed");

    // And the ratio applies whatever the ceiling would have allowed.
    limits.max_output = 1024u * 1024u;
    limits.min_output = 1024;
    limits.max_ratio = 100;
    net::HttpResponse again = Coded("gzip", kGzipBomb, sizeof(kGzipBomb));
    Expect(net::DecodeContentEncoding(again, limits) == net::DecodeStatus::TooLarge,
           "13,200 is what 132 bytes are allowed to become, ceiling or no ceiling");
  });

  AddTest(tests, "ContentEncoding/ACodingWeNeverAskedForFailsTheResponse", [] {
    // **This assertion changed with ADR 0024's brotli (ledger session 20).** It
    // used to be `br` here, standing for "a coding we do not implement"; brotli is
    // implemented now, so the example is one that still is not. `compress` is the
    // right one: it is a real HTTP coding, it is LZW, and no browser has decoded
    // it in twenty years -- so a server sending it is a server that would send it
    // to nobody.
    net::HttpResponse unknown = Coded("compress", kGzipHello, sizeof(kGzipHello));
    Expect(net::DecodeContentEncoding(unknown) == net::DecodeStatus::UnsupportedCoding,
           "a coding not in Accept-Encoding cannot be handed to a parser");

    net::HttpResponse many = Coded("gzip, gzip, gzip, gzip, gzip", kGzipHello, sizeof(kGzipHello));
    Expect(net::DecodeContentEncoding(many) == net::DecodeStatus::UnsupportedCoding,
           "a coding list long enough to be a work request is not a response");
  });

  AddTest(tests, "ContentEncoding/AStreamThatDoesNotDecodeFailsTheResponse", [] {
    // Two bytes gone from the middle of the deflate stream, with the header and
    // the trailer intact, so this is the stream failing rather than the framing.
    std::vector<std::uint8_t> gap(kGzipHello, kGzipHello + sizeof(kGzipHello));
    gap.erase(gap.begin() + 12, gap.begin() + 14);
    net::HttpResponse response = Coded("gzip", gap.data(), gap.size());
    Expect(net::DecodeContentEncoding(response) == net::DecodeStatus::Malformed,
           "a member with a hole in it is not a shorter body");

    std::vector<std::uint8_t> corrupt(kGzipHello, kGzipHello + sizeof(kGzipHello));
    corrupt[sizeof(kGzipHello) - 5] ^= 0xFFu;
    net::HttpResponse checksum = Coded("gzip", corrupt.data(), corrupt.size());
    Expect(net::DecodeContentEncoding(checksum) == net::DecodeStatus::Malformed,
           "nor is one whose checksum does not hold");
  });

  AddTest(tests, "ContentEncoding/AcceptEncodingNamesExactlyWhatCanBeDecoded", [] {
    // The two constants are one constant on purpose: advertising a coding the
    // decoder does not implement turns every response using it into a failed
    // load, which is ADR 0012's stub problem seen from the network side.
    const std::string advertised = net::kAcceptedContentEncodings;
    Expect(advertised.find("gzip") != std::string::npos, "gzip is advertised");
    Expect(advertised.find("deflate") != std::string::npos, "and deflate");
    // **Changed with ADR 0024's brotli.** It used to assert that `br` was *absent*,
    // "because ADR 0010 leaves it to an ADR of its own" -- and 0024 is that ADR.
    // Brotli is first in the list on purpose: it is the coding almost every CDN has
    // already prepared, so asking for gzip ahead of it is asking for the
    // second-best artefact the server has on disk.
    Expect(advertised.find("br") == 0, "and brotli is first");
    Expect(advertised.find("compress") == std::string::npos,
           "and a coding nothing decodes is not advertised");
  });

  AddTest(tests, "ContentEncoding/ABrotliBodyIsDecoded", [] {
    // `br` over "hello", produced by brotli's own encoder:
    //   python3 -c "import brotli,sys; sys.stdout.buffer.write(brotli.compress(b'hello'))"
    static constexpr std::uint8_t kBrotliHello[] = {0x0b, 0x02, 0x80, 0x68, 0x65,
                                                   0x6c, 0x6c, 0x6f, 0x03};
    net::HttpResponse response = Coded("br", kBrotliHello, sizeof(kBrotliHello));
    Expect(net::DecodeContentEncoding(response) == net::DecodeStatus::Decoded,
           "a brotli body decodes");
    ExpectEqString(std::string(reinterpret_cast<const char*>(response.body.data()),
                               response.body.size()),
                   "hello", "to what the server compressed");
    // And the bound applies, with no declared size to refuse from -- which is the
    // difference between brotli and gzip that util::BrotliInflate exists to state.
    net::HttpResponse bounded = Coded("br", kBrotliHello, sizeof(kBrotliHello));
    net::DecodeLimits limits;
    limits.max_output = 2;
    Expect(net::DecodeContentEncoding(bounded, limits) != net::DecodeStatus::Decoded,
           "and a body over the ceiling fails rather than truncating");
  });

  // The resolver cache, and the one property that makes it safe to have at all.
  //
  // A name lookup is tens of milliseconds cold and microseconds warm, which is
  // a difference any page can time. Keyed by host alone this would answer "has
  // this browser been to that site?" for every site on the web -- the same
  // cross-site inference the connection pool is partitioned to prevent. So the
  // partition is asserted here rather than trusted: a key that quietly became
  // empty would still make every page faster and every test pass.
  AddTest(tests, "ResolverCache/IsKeyedByPartitionAndNotByHost", [] {
    net::ResolverCache cache;
    net::ResolvedAddress address;
    address.family = 2;
    address.address_length = 16;
    cache.Store("https://a.example", "cdn.example", 443, {address}, 0);

    Expect(cache.Lookup("https://a.example", "cdn.example", 443, 0) != nullptr,
           "the site that resolved a name gets it back");
    Expect(cache.Lookup("https://b.example", "cdn.example", 443, 0) == nullptr,
           "and another site sharing that host does not");
    Expect(cache.Lookup("https://a.example", "cdn.example", 80, 0) == nullptr,
           "nor does the same site on another port");
  });

  AddTest(tests, "ResolverCache/ForgetsAnEntryPastItsTtl", [] {
    net::ResolverCache cache;
    net::ResolvedAddress address;
    address.family = 2;
    address.address_length = 16;
    cache.Store("https://a.example", "cdn.example", 443, {address}, 1000);

    Expect(cache.Lookup("https://a.example", "cdn.example", 443,
                        1000 + net::kResolvedNameTtlMs - 1) != nullptr,
           "an entry inside the TTL answers");
    Expect(cache.Lookup("https://a.example", "cdn.example", 443,
                        1000 + net::kResolvedNameTtlMs) == nullptr,
           "and one at it does not -- a server that moved must be findable again");
  });

  AddTest(tests, "ResolverCache/IsBounded", [] {
    net::ResolverCache cache;
    net::ResolvedAddress address;
    address.family = 2;
    address.address_length = 16;
    // A page names as many hosts as it likes, so the bound is the browser's.
    for (std::size_t i = 0; i < net::kMaxResolvedNames * 3; ++i) {
      cache.Store("https://a.example", "host" + std::to_string(i) + ".example", 443, {address},
                  static_cast<std::int64_t>(i));
    }
    Expect(cache.Size() <= net::kMaxResolvedNames, "the cache does not grow with the page");
  });

  AddTest(tests, "ResolverCache/DoesNotCacheAFailure", [] {
    net::ResolverCache cache;
    // An empty answer is what a failed resolution produces, and storing it would
    // turn a transient outage into a page that stays broken for a minute.
    cache.Store("https://a.example", "cdn.example", 443, {}, 0);
    Expect(cache.Lookup("https://a.example", "cdn.example", 443, 0) == nullptr,
           "a failed resolution is not remembered");
  });
}

}  // namespace microbrowser::tests
