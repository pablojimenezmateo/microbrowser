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

// A scripted connection. This is the entire reason `Transport` is a virtual
// boundary: without it none of the logic below — redirects, cookie round trips,
// cache behavior, policy re-entry — could be tested without a network.
class ScriptedTransport : public Transport {
 public:
  struct Exchange {
    std::string expected_host;
    std::uint16_t expected_port = 0;
    bool expected_secure = false;
    std::string response;
  };

  struct Log {
    std::vector<std::string> requests;
    std::vector<std::string> hosts;
    std::vector<bool> secure;
  };

  ScriptedTransport(std::vector<Exchange>& script, std::size_t& cursor, Log& log)
      : script_(script), cursor_(cursor), log_(log) {}

  bool Connect(std::string_view host, std::uint16_t port, bool secure) override {
    if (cursor_ >= script_.size()) {
      return false;
    }
    const Exchange& exchange = script_[cursor_];
    if (!exchange.expected_host.empty() && exchange.expected_host != host) {
      return false;
    }
    if (exchange.expected_port != 0 && exchange.expected_port != port) {
      return false;
    }
    if (exchange.expected_secure != secure) {
      return false;
    }
    log_.hosts.emplace_back(host);
    log_.secure.push_back(secure);
    pending_ = exchange.response;
    return true;
  }

  bool Send(std::span<const std::byte> data) override {
    request_.append(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
  }

  std::optional<std::size_t> Receive(std::span<std::byte> out) override {
    if (!sent_) {
      log_.requests.push_back(request_);
      sent_ = true;
    }
    if (pending_.empty()) {
      return std::size_t{0};  // peer closed
    }
    const std::size_t take = std::min(out.size(), pending_.size());
    std::memcpy(out.data(), pending_.data(), take);
    pending_.erase(0, take);
    return take;
  }

  void Close() override { ++cursor_; }

 private:
  std::vector<Exchange>& script_;
  std::size_t& cursor_;
  Log& log_;
  std::string request_;
  std::string pending_;
  bool sent_ = false;
};

class ScriptedFactory : public TransportFactory {
 public:
  std::unique_ptr<Transport> Create() override {
    return std::make_unique<ScriptedTransport>(script, cursor, log);
  }

  std::vector<ScriptedTransport::Exchange> script;
  std::size_t cursor = 0;
  ScriptedTransport::Log log;
};

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

constexpr std::string_view kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

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
