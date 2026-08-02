#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "net/CookieJar.h"
#include "net/HttpMessage.h"
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

}  // namespace

void RegisterNetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Http/ParsesASimpleResponse", [] {
    ResponseParser parser = ParseWhole(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nhello");
    Expect(parser.IsComplete(), "a complete response parses");
    ExpectEqInt(parser.Response().status, 200, "status");
    ExpectEqString(parser.Response().reason, "OK", "reason phrase");
    ExpectEqString(std::string(*parser.Response().headers.Get("content-type")), "text/html",
                   "header lookup is case-insensitive");
    ExpectEqString(BodyOf(parser), "hello", "body");
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
  });

  AddTest(tests, "Http/RejectsMalformedStartAndHeaders", [] {
    Expect(ParseWhole("garbage\r\n\r\n").Failed(), "not a status line");
    Expect(ParseWhole("HTTP/9.9 200 OK\r\n\r\n").Failed(), "unsupported version");
    Expect(ParseWhole("HTTP/1.1 999 X\r\n\r\n").Failed(), "status code out of range");
    Expect(ParseWhole("HTTP/1.1 abc X\r\n\r\n").Failed(), "non-numeric status");
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

  AddTest(tests, "Cookie/DomainMatchingStopsAtALabelBoundary", [] {
    Expect(net::CookieDomainMatches("example.com", "example.com"), "exact");
    Expect(net::CookieDomainMatches("a.example.com", "example.com"), "subdomain");
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
}

}  // namespace microbrowser::tests
