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
#include "net/Fetch.h"
#include "net/SocketTransport.h"
#include "net/Transport.h"
#include "privacy/PrivacyPolicy.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

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
  return net::Fetch(policy.Decide(request), policy, factory, cookies, cache, options, now);
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
  return net::Fetch(policy.Decide(request, &referrer_url), policy, factory, cookies, cache, {},
                    now);
}

constexpr std::string_view kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");
    ExpectEqInt(result.response.status, 200, "status");

    const std::string& request = factory.log.requests.at(0);
    Expect(request.rfind("GET /page HTTP/1.1\r\n", 0) == 0, "request line");
    Expect(request.find("Host: example.com\r\n") != std::string::npos, "Host header");
    Expect(request.find("Accept-Language: en-US\r\n") != std::string::npos,
           "Accept-Language is en-US regardless of the system locale; the locale is an "
           "identifying bit the user did not choose to reveal");
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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");
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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");
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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");
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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");

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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");

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
    Expect(result.ok, result.error != nullptr ? result.error : "fetch failed");

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
    Expect(request.find("Accept-Encoding: identity\r\n") != std::string::npos,
           "content coding stays explicit");
    Expect(request.find("Connection: close\r\n") != std::string::npos, "one connection policy");
    Expect(request.find("User-Agent:") == std::string::npos, "no caller-supplied user agent");
    Expect(request.find("X-Keep: yes\r\n") != std::string::npos,
           "ordinary application headers still travel");
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
    Expect(login.ok, login.error != nullptr ? login.error : "login failed");
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
    Expect(login.ok, login.error != nullptr ? login.error : "login failed");

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

    const Url resource = MustParse("https://cdn.example/lib.js");
    const auto fetch_under = [&](std::string_view top_level) {
      privacy::Request request;
      request.url = resource;
      request.top_level_site = url::Site::FromUrl(MustParse(top_level));
      request.type = privacy::ResourceType::Script;
      return net::Fetch(policy.Decide(request), policy, factory, cookies, cache, {}, 1000);
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
    // 127.0.0.1 on a port nothing is bound to: refused immediately, no network
    // required and no timeout waited on.
    Expect(!connection->Connect("127.0.0.1", 9, false),
           "connecting to a closed port fails cleanly");
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
