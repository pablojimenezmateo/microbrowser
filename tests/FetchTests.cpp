#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "support/ScriptedTransport.h"
#include "net/ConnectionPool.h"
#include "net/Fetch.h"
#include "net/SocketTransport.h"
#include "net/Transport.h"
#include "privacy/PrivacyPolicy.h"
#include "url/PartitionKey.h"
#include "url/Url.h"
#include "util/UserAgent.h"

namespace microbrowser::tests {

using net::CookieJar;
using net::FetchOptions;
using net::FetchResult;
using net::HttpCache;
using net::Transport;
using net::TransportFactory;
using privacy::PrivacyPolicy;
using privacy::Verdict;
using url::ContainerId;
using url::Url;

namespace {

Url MustParse(std::string_view text) {
  const auto url = Url::Parse(text);
  Expect(url.has_value(), std::string("expected to parse: ") + std::string(text));
  return *url;
}

// Drives a started request until it stops. Since ADR 0011 a fetch is an object
// that is advanced rather than a call that returns, and a canned transport is
// always runnable -- so a test can turn the crank itself. Bounded rather than
// `while (!complete)`: a request that stopped making progress must fail the
// test rather than hang it.
FetchResult RunToCompletion(std::unique_ptr<net::FetchRequest> request,
                            std::int64_t now_ms = 1000) {
  for (int turn = 0; turn < 1000 && !request->IsComplete(); ++turn) {
    if (!request->Advance(now_ms) && !request->IsComplete()) {
      break;
    }
  }
  Expect(request->IsComplete(), "the request never finished");
  return request->TakeResult();
}

// Runs a request the way the engine will: build the privacy Request, get a
// Verdict, hand it to Fetch. There is no shorter path, which is the point.
FetchResult Run(const PrivacyPolicy& policy, ScriptedFactory& factory, CookieJar& cookies,
                HttpCache& cache, std::string_view target, const FetchOptions& options = {},
                std::int64_t now = 1000) {
  privacy::Request request;
  request.url = MustParse(target);
  request.top_level_site = url::Site::FromUrl(request.url);
  request.type = privacy::ResourceType::Document;
  request.is_subresource = false;
  // A pool per call, so a helper that a test uses twice does not accidentally
  // make those two requests share a connection. The tests that are *about*
  // reuse build one pool and keep it.
  net::ConnectionPool pool(factory);
  return RunToCompletion(
      net::Fetch(policy.Decide(request), policy, pool, cookies, cache, options, now));
}

FetchResult RunWithReferrer(const PrivacyPolicy& policy, ScriptedFactory& factory,
                            CookieJar& cookies, HttpCache& cache, std::string_view target,
                            std::string_view top_level, std::string_view referrer,
                            std::int64_t now = 1000) {
  privacy::Request request;
  request.url = MustParse(target);
  const Url top = MustParse(top_level);
  request.top_level_site = url::Site::FromUrl(top);
  request.initiator = url::Origin::FromUrl(top);
  request.type = privacy::ResourceType::Script;
  const Url referrer_url = MustParse(referrer);
  net::ConnectionPool pool(factory);
  return RunToCompletion(net::Fetch(policy.Decide(request, &referrer_url), policy, pool,
                                    cookies, cache, {}, now));
}

constexpr std::string_view kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

// gzip("hello hello hello world"), and 132 bytes that claim to be 100,000.
// Both written by zlib; see NetTests for how they are regenerated.
constexpr std::uint8_t kGzipHello[] = {
    0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0xCB, 0x48,
    0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x22, 0xCB, 0xF3, 0x8B, 0x72, 0x52,
    0x00, 0x26, 0xE6, 0x5A, 0x81, 0x17, 0x00, 0x00, 0x00};

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

// A 200 whose body is those bytes and whose headers say so. Written with an
// explicit length rather than close-delimited, because that is what a server
// sending a coded body does.
template <std::size_t N>
std::string GzipResponse(const std::uint8_t (&body)[N]) {
  std::string text = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Encoding: gzip\r\n";
  text += "Content-Length: " + std::to_string(N) + "\r\n\r\n";
  for (const std::uint8_t byte : body) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string BodyString(const net::HttpResponse& response) {
  return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

std::size_t CountOccurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (true) {
    const std::size_t found = text.find(needle, offset);
    if (found == std::string_view::npos) {
      return count;
    }
    ++count;
    offset = found + needle.size();
  }
}

}  // namespace

void RegisterFetchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Fetch/PerformsARequestAndReturnsTheResponse", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://example.com/page");
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());
    ExpectEqInt(result.response.status, 200, "status");

    const std::string& request = factory.log.requests.at(0);
    Expect(request.rfind("GET /page HTTP/1.1\r\n", 0) == 0, "request line");
    Expect(request.find("Host: example.com\r\n") != std::string::npos, "Host header");
    Expect(request.find("Accept-Language: en-US\r\n") != std::string::npos,
           "Accept-Language is en-US regardless of the system locale; the locale is an "
           "identifying bit the user did not choose to reveal");
    Expect(request.find("User-Agent: microbrowser\r\n") != std::string::npos,
           "User-Agent names the browser and nothing about the machine");
  });

  // A request with no User-Agent at all is what old.reddit.com's edge blocks --
  // it served a 'Blocked' page until this header existed, which is how ADR
  // 0007's second compatibility target began. The value is asserted against the
  // shared constant rather than a literal so that changing the string cannot
  // silently desynchronize the header from `navigator.userAgent`.
  AddTest(tests, "Fetch/SendsTheSameUserAgentScriptIsTold", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    Run(policy, factory, cookies, cache, "https://example.com/page");

    const std::string& request = factory.log.requests.at(0);
    const std::string expected = "User-Agent: " + std::string(util::kUserAgent) + "\r\n";
    Expect(request.find(expected) != std::string::npos,
           "the header carries util::kUserAgent verbatim");
    Expect(request.find("Linux") == std::string::npos &&
               request.find("X11") == std::string::npos &&
               request.find("Mozilla") == std::string::npos,
           "and says nothing about the machine, and does not claim to be another browser");
  });

  AddTest(tests, "Fetch/RefusesToTouchTheNetworkWhenThePolicyRefused", [] {
    PrivacyPolicy policy;
    policy.Engine().AddRules("||tracker.example^");
    ScriptedFactory factory;  // empty script: any connection attempt fails the test
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result =
        Run(policy, factory, cookies, cache, "https://tracker.example/pixel");
    Expect(!result.ok, "a blocked request must not be made");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 0,
                "and must not have opened a connection");
  });

  AddTest(tests, "Fetch/UpgradesBeforeConnecting", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    // The script asserts the connection is to port 443 with TLS. If Fetch used
    // the requested http URL rather than the verdict's final URL, Connect would
    // be told 80 and plaintext, and this fails.
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "http://example.com/p");
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());
    Expect(factory.log.secure.at(0), "the connection was made over TLS");
    ExpectEqString(result.final_url.Serialize(), "https://example.com/p",
                   "and to the upgraded URL");
  });

  // The case that makes passing the policy into Fetch necessary rather than
  // redundant: a redirect is a request to a URL the page never chose.
  AddTest(tests, "Fetch/PutsRedirectsBackThroughThePolicy", [] {
    PrivacyPolicy policy;
    policy.Engine().AddRules("||evil.example^");
    ScriptedFactory factory;
    factory.script.push_back({"start.example", 443, true,
                              "HTTP/1.1 302 Found\r\nLocation: https://evil.example/x\r\n"
                              "Content-Length: 0\r\n\r\n"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://start.example/");
    Expect(!result.ok, "a redirect into a blocked host must be refused");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 1,
                "and the blocked host must never have been connected to");
  });

  AddTest(tests, "Fetch/RedirectPolicyKeepsTheOriginalResourceType", [] {
    PrivacyPolicy policy;
    policy.Engine().AddRules("||cdn.example^$script");
    ScriptedFactory factory;
    factory.script.push_back({"start.example", 443, true,
                              "HTTP/1.1 302 Found\r\n"
                              "Location: https://cdn.example/script.js\r\n"
                              "Content-Length: 0\r\n\r\n"});
    factory.script.push_back({"cdn.example", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result =
        RunWithReferrer(policy, factory, cookies, cache, "https://start.example/script.js",
                        "https://page.example/", "https://page.example/index.html");
    Expect(!result.ok, "a redirected script is still matched as a script");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 1,
                "and the type-blocked redirect target was not connected to");
  });

  AddTest(tests, "Fetch/ARedirectCannotDowngradeToHttp", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"secure.example", 443, true,
                              "HTTP/1.1 302 Found\r\nLocation: http://secure.example/plain\r\n"
                              "Content-Length: 0\r\n\r\n"});
    factory.script.push_back({"secure.example", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://secure.example/");
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());
    Expect(factory.log.secure.at(1),
           "the redirect target was upgraded before it was connected to, so a server cannot "
           "downgrade a user by answering with a 302");
  });

  AddTest(tests, "Fetch/FollowsRedirectsAndBoundsTheChain", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    for (int i = 0; i < 3; ++i) {
      factory.script.push_back({"a.example", 443, true,
                                "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n"});
    }
    factory.script.push_back({"a.example", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://a.example/");
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());
    ExpectEqInt(result.redirects, 3, "three hops were followed");

    // Now an endless chain.
    ScriptedFactory looping;
    for (int i = 0; i < 50; ++i) {
      looping.script.push_back({"a.example", 443, true,
                                "HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n"});
    }
    CookieJar other_cookies;
    HttpCache other_cache;
    const FetchResult looped =
        Run(policy, looping, other_cookies, other_cache, "https://a.example/");
    Expect(!looped.ok, "an unbounded redirect chain must terminate as a failure");
  });

  AddTest(tests, "Fetch/PostRedirectToGetDropsBodyAndBodyHeaders", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 303 See Other\r\nLocation: /done\r\n"
                              "Content-Length: 0\r\n\r\n"});
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    FetchOptions options;
    options.method = "POST";
    options.headers.Add("Content-Type", "application/x-www-form-urlencoded");
    options.headers.Add("X-Keep", "yes");
    options.body = Bytes("q=hello");
    const FetchResult result =
        Run(policy, factory, cookies, cache, "https://example.com/form", options);
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());

    const std::string& redirected = factory.log.requests.at(1);
    Expect(redirected.rfind("GET /done HTTP/1.1\r\n", 0) == 0,
           "303 rewrites POST to GET");
    Expect(redirected.find("Content-Type:") == std::string::npos,
           "a redirected GET does not keep the form entity header");
    Expect(redirected.find("Content-Length:") == std::string::npos,
           "and it has no body length");
    Expect(redirected.find("X-Keep: yes\r\n") != std::string::npos,
           "headers unrelated to the request body are kept across the redirect");
    Expect(redirected.size() < 7 || redirected.substr(redirected.size() - 7) != "q=hello",
           "and the original body is not sent again");
  });

  AddTest(tests, "Fetch/OwnsRequestBodyFramingHeaders", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    FetchOptions options;
    options.method = "POST";
    options.headers.Add("Content-Length", "999");
    options.headers.Add("Transfer-Encoding", "chunked");
    options.body = Bytes("q=hello");
    const FetchResult result =
        Run(policy, factory, cookies, cache, "https://example.com/form", options);
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());

    const std::string& request = factory.log.requests.at(0);
    Expect(request.find("Content-Length: 7\r\n") != std::string::npos,
           "Fetch computes the body length from the body it will actually send");
    Expect(request.find("Content-Length: 999\r\n") == std::string::npos,
           "caller-provided Content-Length is not forwarded");
    Expect(request.find("Transfer-Encoding:") == std::string::npos,
           "and neither is caller-provided transfer coding");
  });

  AddTest(tests, "Fetch/OwnsPrivacyAndConnectionHeaders", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    FetchOptions options;
    options.headers.Add("Host", "evil.example");
    options.headers.Add("Cookie", "sid=attacker");
    options.headers.Add("Referer", "https://secret.example/path");
    options.headers.Add("Accept-Language", "fr-FR");
    options.headers.Add("Accept-Encoding", "gzip");
    options.headers.Add("Connection", "keep-alive");
    options.headers.Add("User-Agent", "fingerprint");
    options.headers.Add("X-Keep", "yes");

    const FetchResult result =
        Run(policy, factory, cookies, cache, "https://example.com/page", options);
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());

    const std::string& request = factory.log.requests.at(0);
    ExpectEqInt(static_cast<long long>(CountOccurrences(request, "Host: ")), 1,
                "Fetch owns the Host header");
    Expect(request.find("Host: example.com\r\n") != std::string::npos, "canonical Host");
    Expect(request.find("Cookie:") == std::string::npos,
           "caller-provided cookies do not bypass the jar");
    Expect(request.find("Referer:") == std::string::npos,
           "caller-provided referrers do not bypass the privacy verdict");
    Expect(request.find("Accept-Language: en-US\r\n") != std::string::npos,
           "locale exposure stays fixed");
    Expect(request.find("Accept-Language: fr-FR\r\n") == std::string::npos,
           "and the caller cannot replace it");
    // **Changed with ADR 0024's brotli (ledger session 20).** It read
    // `gzip, deflate` before; brotli is implemented now and is asked for first,
    // because it is the coding almost every CDN has already prepared.
    Expect(request.find("Accept-Encoding: br, gzip, deflate\r\n") != std::string::npos,
           "content coding stays explicit, and names exactly what can be decoded");
    ExpectEqInt(static_cast<long long>(CountOccurrences(request, "Accept-Encoding:")), 1,
                "the caller's own Accept-Encoding does not join it");
    // No Connection header at all, in either direction. HTTP/1.1 is persistent
    // by default so `keep-alive` would say what the version already says, and
    // the caller's `keep-alive` must not travel either: whether a connection is
    // kept is the pool's decision, not a page's.
    Expect(request.find("Connection:") == std::string::npos,
           "connection persistence is not something a caller states");
    Expect(request.find("User-Agent: fingerprint\r\n") == std::string::npos,
           "no caller-supplied user agent");
    ExpectEqInt(static_cast<long long>(CountOccurrences(request, "User-Agent:")), 1,
                "exactly one, and it is the one Fetch owns");
    Expect(request.find("X-Keep: yes\r\n") != std::string::npos,
           "ordinary application headers still travel");
  });

  AddTest(tests, "Fetch/DecodesAGzipBodyBeforeAnybodySeesIt", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, GzipResponse(kGzipHello)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://example.com/page");
    Expect(result.ok, result.error.empty() ? "fetch failed" : result.error.c_str());
    ExpectEqString(BodyString(result.response), "hello hello hello world",
                   "the caller gets the plain body, never the coded one");
    Expect(!result.response.headers.Has("content-encoding"),
           "and no header claiming it is still coded");
  });

  AddTest(tests, "Fetch/ADecompressionBombFailsTheRequestRatherThanTheProcess", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, GzipResponse(kGzipBomb)});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult result = Run(policy, factory, cookies, cache, "https://example.com/page");
    Expect(!result.ok, "132 bytes that claim to be 100,000 do not become a response");
    Expect(result.error.find("bound") != std::string::npos,
           std::string("the failure should name the bound, and said: ") + result.error);
    Expect(result.response.body.empty(), "and nothing partial comes back with it");
  });

  // --- Connection reuse, ADR 0010 §2 ----------------------------------------

  AddTest(tests, "Fetch/ASecondRequestToTheSameOriginReusesTheConnection", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    factory.script.push_back({"example.com", 443, true, OkResponse("text/css", "body{}")});
    CookieJar cookies;
    HttpCache cache;
    net::ConnectionPool pool(factory);

    const auto fetch = [&](std::string_view target) {
      privacy::Request request;
      request.url = MustParse(target);
      request.top_level_site = url::Site::FromUrl(MustParse("https://example.com/"));
      request.type = privacy::ResourceType::Script;
      return RunToCompletion(
          net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000));
    };

    Expect(fetch("https://example.com/one").ok, "the first request succeeds");
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 1,
                "and its connection goes back to the pool rather than being closed");
    Expect(fetch("https://example.com/two").ok, "the second request succeeds");
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "on the same connection: two exchanges, one connect");
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 1, "and it is idle again afterwards");
    Expect(factory.log.requests.at(1).rfind("GET /two", 0) == 0,
           "the second request really did go out on it");
  });

  // The privacy content of the whole change. A reused connection is a linkage
  // two requests share, visible to the server; pooling by host would create the
  // cross-site correlation the ADR 0005 key exists to prevent.
  AddTest(tests, "Fetch/TwoTopLevelSitesDoNotShareAConnectionToTheSameHost", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"cdn.example", 443, true, std::string(kOk)});
    factory.script.push_back({"cdn.example", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;
    net::ConnectionPool pool(factory);

    const auto fetch_under = [&](std::string_view top_level) {
      privacy::Request request;
      request.url = MustParse("https://cdn.example/lib.js");
      request.top_level_site = url::Site::FromUrl(MustParse(top_level));
      request.type = privacy::ResourceType::Script;
      return RunToCompletion(
          net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000));
    };

    Expect(fetch_under("https://news.example/").ok, "the first site loads it");
    Expect(fetch_under("https://shop.example/").ok, "and so does the second");
    ExpectEqInt(static_cast<long long>(factory.connects), 2,
                "same host, two partitions, two connections -- pooling by host would be the "
                "cross-site linkage ADR 0005 exists to prevent");
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 2, "and both are held separately");
  });

  AddTest(tests, "Fetch/AConnectionWhoseEndNobodyIsSureOfIsNotKept", [] {
    CookieJar cookies;
    HttpCache cache;
    const auto keeps = [&](std::string_view response) {
      PrivacyPolicy policy;
      ScriptedFactory factory;
      factory.script.push_back({"example.com", 443, true, std::string(response)});
      net::ConnectionPool pool(factory);
      privacy::Request request;
      request.url = MustParse("https://example.com/page");
      request.top_level_site = url::Site::FromUrl(request.url);
      request.type = privacy::ResourceType::Document;
      RunToCompletion(net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000));
      return pool.IdleCount() == 1;
    };

    Expect(keeps("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"),
           "a self-delimited response leaves a connection worth keeping");
    Expect(!keeps("HTTP/1.1 200 OK\r\n\r\nhi"),
           "a body that ends when the socket does has no other terminator, so the socket is "
           "the message and cannot carry a second one");
    Expect(!keeps("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi"),
           "a server that says it is about to close is not argued with");
    Expect(!keeps("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi"),
           "HTTP/1.0 is not persistent unless it says so");
    Expect(keeps("HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nhi"),
           "and when it says so, it is");
    Expect(!keeps("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi_EXTRA"),
           "bytes past the end of the message mean the next request would start reading at an "
           "unknown offset, which is request smuggling with one browser playing both parsers");
  });

  AddTest(tests, "Fetch/ARedirectToTheSameHostKeepsTheConnection", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 302 Found\r\nLocation: /moved\r\nContent-Length: 0\r\n\r\n"});
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;
    net::ConnectionPool pool(factory);

    const FetchResult result = Run(policy, factory, cookies, cache, "https://example.com/start");
    Expect(result.ok, result.error.empty() ? "the redirect chain failed" : result.error.c_str());
    ExpectEqInt(result.redirects, 1, "one redirect was followed");
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "and the second request went out on the connection the first left behind");
  });

  AddTest(tests, "Fetch/IdleConnectionsExpireAndScheduleExactlyOneWakeup", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;
    net::ConnectionPool pool(factory);

    Expect(!pool.NextDeadlineMs(0).has_value(),
           "an empty pool schedules nothing at all, which is the zero-idle-CPU invariant");

    privacy::Request request;
    request.url = MustParse("https://example.com/page");
    request.top_level_site = url::Site::FromUrl(request.url);
    request.type = privacy::ResourceType::Document;
    RunToCompletion(net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000),
                    /*now_ms=*/5000);
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 1, "one connection is held");
    ExpectEqInt(pool.NextDeadlineMs(5000).value_or(0), net::kIdleConnectionTimeoutMs,
                "and it schedules one wakeup, at its timeout");

    pool.CloseExpired(5000 + net::kIdleConnectionTimeoutMs - 1);
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 1, "not a millisecond early");
    pool.CloseExpired(5000 + net::kIdleConnectionTimeoutMs);
    ExpectEqInt(static_cast<long long>(pool.IdleCount()), 0,
                "and a socket the user did not ask to keep open does not outlive its timeout");
    Expect(!pool.NextDeadlineMs(5000 + net::kIdleConnectionTimeoutMs).has_value(),
           "after which the loop is asked to wake for nothing again");
  });

  AddTest(tests, "Fetch/APooledConnectionTheServerClosedIsRetriedOnce", [] {
    // The race every connection pool has: the server closed it while it sat
    // idle, and the browser finds out by writing into it. Nothing was asked, so
    // asking again repeats nothing.
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    factory.script.push_back({"example.com", 443, true, ""});  // the dead one: no bytes at all
    factory.script.push_back({"example.com", 443, true, OkResponse("text/css", "body{}")});
    CookieJar cookies;
    HttpCache cache;
    net::ConnectionPool pool(factory);

    const auto fetch = [&](std::string_view target) {
      privacy::Request request;
      request.url = MustParse(target);
      request.top_level_site = url::Site::FromUrl(MustParse("https://example.com/"));
      request.type = privacy::ResourceType::Script;
      return RunToCompletion(
          net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000));
    };

    Expect(fetch("https://example.com/one").ok, "the first request succeeds");
    const FetchResult second = fetch("https://example.com/two");
    Expect(second.ok, second.error.empty() ? "the retry failed" : second.error.c_str());
    ExpectEqString(BodyString(second.response), "body{}",
                   "the answer came from the fresh connection the retry opened");
    ExpectEqInt(static_cast<long long>(factory.connects), 2,
                "one connect for the first request, one for the retry -- and the dead pooled "
                "connection cost no third");
  });

  AddTest(tests, "Fetch/StoresAndSendsCookiesInTheRequestsOwnPartition", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc\r\nContent-Length: 0\r\n\r\n"});
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;

    Run(policy, factory, cookies, cache, "https://example.com/one");
    ExpectEqInt(static_cast<long long>(cookies.Size()), 1, "the Set-Cookie was stored");

    Run(policy, factory, cookies, cache, "https://example.com/two");
    Expect(factory.log.requests.at(1).find("Cookie: sid=abc\r\n") != std::string::npos,
           "and travels on the next request to the same partition");
  });

  AddTest(tests, "Fetch/ServesFromTheCacheWithoutConnecting", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(first.ok && !first.from_cache, "the first fetch goes to the network");

    const FetchResult second = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(second.ok && second.from_cache, "the second is served from the cache");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 1,
                "having opened no second connection");

    const FetchResult stale =
        Run(policy, factory, cookies, cache, "https://example.com/x", {}, 100000);
    Expect(!stale.ok || !stale.from_cache,
           "and a stale entry is not served; the script has no second response, so this "
           "failing to connect is the correct outcome");
  });

  AddTest(tests, "Fetch/HugeMaxAgeDoesNotWrapTheCacheEntryStale", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=9223372036854775807\r\n"
         "Content-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(first.ok && !first.from_cache, "the first fetch stores the response");

    const FetchResult second = Run(policy, factory, cookies, cache, "https://example.com/x", {},
                                   2000);
    Expect(second.ok && second.from_cache,
           "a huge max-age saturates instead of wrapping to an already-stale expiry");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 1,
                "so the second request does not reach the transport");
  });

  AddTest(tests, "Fetch/BypassCacheReloadFetchesAndReplacesFreshEntries", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\none"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\ntwo"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(first.ok && !first.from_cache, "the first response came from the network");

    FetchOptions options;
    options.bypass_cache = true;
    const FetchResult reloaded =
        Run(policy, factory, cookies, cache, "https://example.com/x", options);
    Expect(reloaded.ok && !reloaded.from_cache,
           "explicit cache bypass must not serve the fresh entry");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 2,
                "it opened a second connection");

    const FetchResult cached = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(cached.ok && cached.from_cache,
           "the bypassed response still replaces the cache for later ordinary loads");
    ExpectEqString(BodyString(cached.response), "two",
                   "the later cached value is the reloaded response");
  });

  AddTest(tests, "Fetch/ClearingTheHttpCacheResetsByteAccounting", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(first.ok && !first.from_cache, "the response was fetched");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1, "and stored");
    Expect(cache.Bytes() > 0, "with byte accounting");

    cache.Clear();
    ExpectEqInt(static_cast<long long>(cache.Size()), 0, "no entries remain");
    ExpectEqInt(static_cast<long long>(cache.Bytes()), 0,
                "and no cleared entry is still counted against the budget");
  });

  AddTest(tests, "Fetch/ShrinkingTheHttpCacheBudgetEvictsImmediately", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(first.ok && !first.from_cache, "the response was fetched");
    Expect(cache.Bytes() > 0, "there is something to evict");

    cache.SetByteBudget(0);
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "a zero byte budget must empty the HTTP cache immediately");
    ExpectEqInt(static_cast<long long>(cache.Bytes()), 0,
                "and its byte accounting follows the eviction");
  });

  AddTest(tests, "Fetch/DoesNotUseTheUrlCacheForRequestsWithBodies", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\nget"});
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\n"
                              "Content-Length: 4\r\n\r\npost"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult get = Run(policy, factory, cookies, cache, "https://example.com/form");
    Expect(get.ok && !get.from_cache, "the GET response was fetched and stored");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1, "the GET response is cacheable");

    FetchOptions options;
    options.method = "POST";
    options.body = Bytes("q=hello");
    const FetchResult post =
        Run(policy, factory, cookies, cache, "https://example.com/form", options);
    Expect(post.ok && !post.from_cache,
           "a body-carrying request must not reuse a response cached under the same URL");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 2,
                "the POST reached the transport instead of being answered from cache");

    const std::string& request = factory.log.requests.at(1);
    Expect(request.rfind("POST /form HTTP/1.1\r\n", 0) == 0, "the method was preserved");
    Expect(request.find("Content-Length: 7\r\n") != std::string::npos,
           "the body length is sent with the request");
    Expect(request.size() >= 7 && request.substr(request.size() - 7) == "q=hello",
           "and the request body follows the headers");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1,
                "the POST response is not inserted into a URL-only cache");
  });

  AddTest(tests, "Fetch/DoesNotUseTheUrlCacheForRequestsWithCustomHeaders", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\none"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\ntwo"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult ordinary = Run(policy, factory, cookies, cache, "https://example.com/data");
    Expect(ordinary.ok && !ordinary.from_cache, "the first ordinary GET is cacheable");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1, "it was stored");

    FetchOptions options;
    options.headers.Add("X-Mode", "alternate");
    const FetchResult custom =
        Run(policy, factory, cookies, cache, "https://example.com/data", options);
    Expect(custom.ok && !custom.from_cache,
           "a request header can affect the response, and this cache key does not include it");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 2,
                "so the custom-header request reaches the transport");
    Expect(factory.log.requests.at(1).find("X-Mode: alternate\r\n") != std::string::npos,
           "with the custom header still present");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1,
                "and the custom-header response is not stored under the URL-only key");
  });

  AddTest(tests, "Fetch/DoesNotUseTheUrlCacheForRequestsWithReferrers", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"cdn.example", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\none"});
    factory.script.push_back(
        {"cdn.example", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\ntwo"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first =
        RunWithReferrer(policy, factory, cookies, cache, "https://cdn.example/data",
                        "https://news.example/", "https://news.example/secret/article");
    Expect(first.ok && !first.from_cache, "the first referrer-bearing request reaches the network");
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "and is not stored under a URL-only key");
    Expect(factory.log.requests.at(0).find("Referer: https://news.example/\r\n") !=
               std::string::npos,
           "with the trimmed cross-origin referrer");

    const FetchResult second =
        RunWithReferrer(policy, factory, cookies, cache, "https://cdn.example/data",
                        "https://news.example/", "https://news.example/secret/article");
    Expect(second.ok && !second.from_cache,
           "a referrer-bearing request must not reuse a URL-only cache entry");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 2,
                "the second request reached the transport");
    ExpectEqString(BodyString(second.response), "two", "second response body");
  });

  AddTest(tests, "Fetch/DoesNotUseCachedPublicResponseOnceCookiesApply", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\npub"});
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc\r\n"
                              "Content-Length: 0\r\n\r\n"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\nprv"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult public_response =
        Run(policy, factory, cookies, cache, "https://example.com/account");
    Expect(public_response.ok && !public_response.from_cache,
           "the public response came from the network");
    ExpectEqInt(static_cast<long long>(cache.Size()), 1, "and was cached");

    const FetchResult login = Run(policy, factory, cookies, cache, "https://example.com/login");
    Expect(login.ok, login.error.empty() ? "login failed" : login.error.c_str());
    ExpectEqInt(static_cast<long long>(cookies.Size()), 1, "the login cookie was stored");

    const FetchResult private_response =
        Run(policy, factory, cookies, cache, "https://example.com/account");
    Expect(private_response.ok && !private_response.from_cache,
           "a cookie-bearing request must not reuse a response cached before login");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 3,
                "the private request reached the transport");
    Expect(factory.log.requests.at(2).find("Cookie: sid=abc\r\n") != std::string::npos,
           "with the cookie that makes the cached public response invalid");
    ExpectEqString(BodyString(private_response.response), "prv", "private response body");
  });

  AddTest(tests, "Fetch/DoesNotStoreResponsesToCookieBearingRequests", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc\r\n"
                              "Content-Length: 0\r\n\r\n"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\none"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 3\r\n\r\ntwo"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult login = Run(policy, factory, cookies, cache, "https://example.com/login");
    Expect(login.ok, login.error.empty() ? "login failed" : login.error.c_str());

    const FetchResult first_account =
        Run(policy, factory, cookies, cache, "https://example.com/account");
    Expect(first_account.ok && !first_account.from_cache,
           "the first cookie-bearing request came from the network");
    ExpectEqString(BodyString(first_account.response), "one", "first account response");
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "a response to a cookie-bearing request is not stored under the URL-only key");

    const FetchResult second_account =
        Run(policy, factory, cookies, cache, "https://example.com/account");
    Expect(second_account.ok && !second_account.from_cache,
           "the second cookie-bearing request also reaches the network");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 3,
                "no cache hit hid the second account request");
    ExpectEqString(BodyString(second_account.response), "two", "second account response");
  });

  AddTest(tests, "Fetch/ACachedResponseIsNotSharedAcrossPartitions", [] {
    // Cache *timing* is a read oracle across partitions: a third party that can
    // tell its resource loaded quickly on one site learns the user visited
    // another.
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"cdn.example", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;

    // One pool across both loads on purpose: the connection is partitioned by
    // the same key the cache is, so this test says both at once.
    net::ConnectionPool pool(factory);
    const Url resource = MustParse("https://cdn.example/lib.js");
    const auto fetch_under = [&](std::string_view top_level) {
      privacy::Request request;
      request.url = resource;
      request.top_level_site = url::Site::FromUrl(MustParse(top_level));
      request.type = privacy::ResourceType::Script;
      return RunToCompletion(
          net::Fetch(policy.Decide(request), policy, pool, cookies, cache, {}, 1000));
    };

    const FetchResult on_news = fetch_under("https://news.example/");
    Expect(on_news.ok, "the first load succeeds");

    const FetchResult on_shop = fetch_under("https://shop.example/");
    Expect(!on_shop.ok || !on_shop.from_cache,
           "the same resource under a different top-level site must not hit the cache; the "
           "script has no second response, so failing to connect is the correct outcome");
  });

  AddTest(tests, "Fetch/DoesNotCacheWhatTheServerSaidNotTo", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 2\r\n\r\nhi"});
    CookieJar cookies;
    HttpCache cache;
    Run(policy, factory, cookies, cache, "https://example.com/x");
    ExpectEqInt(static_cast<long long>(cache.Size()), 0, "no-store is honored");

    ScriptedFactory with_cookie;
    with_cookie.script.push_back({"example.com", 443, true,
                                  "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\n"
                                  "Set-Cookie: a=b\r\nContent-Length: 2\r\n\r\nhi"});
    HttpCache other;
    Run(policy, with_cookie, cookies, other, "https://example.com/y");
    ExpectEqInt(static_cast<long long>(other.Size()), 0,
                "a response carrying Set-Cookie is not cached, or it would replay somebody's "
                "cookie into a later load");

    ScriptedFactory no_freshness;
    no_freshness.script.push_back({"example.com", 443, true, std::string(kOk)});
    HttpCache third;
    Run(policy, no_freshness, cookies, third, "https://example.com/z");
    ExpectEqInt(static_cast<long long>(third.Size()), 0,
                "and a response with no freshness information is not cached on a heuristic");
  });

  AddTest(tests, "Fetch/DoesNotCacheVaryResponsesUnderAUrlOnlyKey", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nVary: Accept-Language\r\n"
         "Content-Length: 3\r\n\r\none"});
    factory.script.push_back(
        {"example.com", 443, true,
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nVary: Accept-Language\r\n"
         "Content-Length: 3\r\n\r\ntwo"});
    CookieJar cookies;
    HttpCache cache;

    const FetchResult first = Run(policy, factory, cookies, cache, "https://example.com/vary");
    Expect(first.ok && !first.from_cache, "the first response reached the network");
    ExpectEqInt(static_cast<long long>(cache.Size()), 0,
                "and a Vary response was not stored under a key that only names the URL");

    const FetchResult second = Run(policy, factory, cookies, cache, "https://example.com/vary");
    Expect(second.ok && !second.from_cache,
           "the second request also reaches the network without a vary-aware cache key");
    ExpectEqInt(static_cast<long long>(factory.log.hosts.size()), 2,
                "no URL-only cache entry answered it");
    ExpectEqString(BodyString(second.response), "two", "second response body");
  });

  AddTest(tests, "Fetch/AMalformedResponseIsAFailureRatherThanAPartialPage", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true,
                              "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                              "Transfer-Encoding: chunked\r\n\r\nhello"});
    CookieJar cookies;
    HttpCache cache;
    const FetchResult result = Run(policy, factory, cookies, cache, "https://example.com/x");
    Expect(!result.ok, "an ambiguously framed response is refused, not interpreted");
  });

  // The real transport is not exercised against a network here — a test that
  // needs the internet is a test that fails on a train. What is asserted is
  // that it exists, that TLS was actually compiled in, and that a connection to
  // an address nothing listens on fails rather than hanging or crashing.
  AddTest(tests, "Fetch/TheRealTransportIsPresentAndTlsIsCompiledIn", [] {
    Expect(net::TlsIsAvailable(),
           "a build that silently lost OpenSSL would refuse every https URL at runtime while "
           "passing every test that uses the scripted transport");

    net::SocketTransportFactory factory;
    const std::unique_ptr<Transport> connection = factory.Create();
    Expect(connection != nullptr, "the factory produces a transport");
    // 127.0.0.1 on a port nothing is bound to. On a non-blocking socket the
    // connect is *started* and refused a moment later, so the failure shows up
    // in Advance rather than in StartConnect -- which is the whole shape of
    // ADR 0011 in three lines, and the reason this test is worth keeping.
    const bool started = connection->StartConnect("127.0.0.1", 9, false);
    net::IoStatus status = started ? connection->Advance() : net::IoStatus::Failed;
    for (int turn = 0; turn < 1000 && status == net::IoStatus::Blocked; ++turn) {
      status = connection->Advance();
    }
    Expect(status == net::IoStatus::Failed, "connecting to a closed port fails cleanly");
  });

  AddTest(tests, "Fetch/SendsNoFragmentToTheServer", [] {
    PrivacyPolicy policy;
    ScriptedFactory factory;
    factory.script.push_back({"example.com", 443, true, std::string(kOk)});
    CookieJar cookies;
    HttpCache cache;
    Run(policy, factory, cookies, cache, "https://example.com/p?q=1#secret");
    const std::string& request = factory.log.requests.at(0);
    Expect(request.rfind("GET /p?q=1 HTTP/1.1\r\n", 0) == 0,
           "a fragment is client-side; sending one tells a server something it has no "
           "business knowing");
  });
}

}  // namespace microbrowser::tests
