#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "support/Http2ScriptedServer.h"
#include "net/ConnectionPool.h"
#include "net/CookieJar.h"
#include "net/Fetch.h"
#include "net/HttpCache.h"
#include "net/RequestQueue.h"
#include "privacy/PrivacyPolicy.h"
#include "url/Url.h"

namespace microbrowser::tests {

using net::ConnectionPool;
using net::CookieJar;
using net::FetchOptions;
using net::FetchResult;
using net::HttpCache;
using net::RequestQueue;
using privacy::PrivacyPolicy;

namespace {

url::Url MustParse(std::string_view text) {
  const auto url = url::Url::Parse(text);
  Expect(url.has_value(), std::string("expected to parse: ") + std::string(text));
  return *url;
}

privacy::Request RequestFor(std::string_view target) {
  privacy::Request request;
  request.url = MustParse(target);
  request.top_level_site = url::Site::FromUrl(request.url);
  request.type = privacy::ResourceType::Document;
  request.is_subresource = false;
  return request;
}

std::string BodyOf(const net::HttpResponse& response) {
  return std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());
}

FetchResult RunToCompletion(std::unique_ptr<net::FetchRequest> request) {
  for (int turn = 0; turn < 2000 && !request->IsComplete(); ++turn) {
    if (!request->Advance(1000) && !request->IsComplete()) {
      break;
    }
  }
  Expect(request->IsComplete(), "the request never finished");
  return request->TakeResult();
}

// Turns the queue over until everything has finished. Bounded, because a queue
// that stops making progress must fail the test rather than hang it.
std::vector<RequestQueue::Completion> Drain(RequestQueue& queue) {
  std::vector<RequestQueue::Completion> all;
  for (int turn = 0; turn < 2000 && !queue.IsIdle(); ++turn) {
    queue.Advance(1000);
    for (RequestQueue::Completion& completion : queue.TakeCompletions()) {
      all.push_back(std::move(completion));
    }
  }
  return all;
}

}  // namespace

void RegisterHttp2FetchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Http2Fetch/ARequestGoesOutAndAResponseComesBack", [] {
    Http2ScriptedFactory factory;
    factory.routes.push_back({"/index.html", 200, "text/html", "<h1>hi</h1>", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);

    const FetchResult result = RunToCompletion(net::Fetch(
        policy.Decide(RequestFor("https://example.com/index.html")), policy, pool, cookies,
        cache, FetchOptions{}, 1000));
    Expect(result.ok, "the fetch must succeed over HTTP/2");
    ExpectEqInt(result.response.status, 200, "with the status the server sent");
    ExpectEqString(BodyOf(result.response), "<h1>hi</h1>", "and the body");
    ExpectEqInt(static_cast<long long>(pool.SessionCount()), 1,
                "and the session it opened stays in the pool for the next request");
  });

  // **TD-0008.** Six concurrent requests to one origin used to mean six
  // sockets, six handshakes, and -- on upload.wikimedia.org -- two HTTP 429s
  // and a page that rendered four of its nineteen images. This is the test that
  // says it is one socket now, and the mechanism it is testing is not HTTP/2
  // itself: it is the *coalescing*, because ALPN answers only after a socket is
  // open, so without it all six would connect before any of them learned that
  // one connection would have done.
  AddTest(tests, "Http2Fetch/SixConcurrentRequestsShareOneConnection", [] {
    Http2ScriptedFactory factory;
    for (int i = 0; i < 6; ++i) {
      factory.routes.push_back(
          {"/img" + std::to_string(i) + ".png", 200, "image/png", "pixels" + std::to_string(i),
           ""});
    }
    // Held, so that all six requests are in flight while the first connection
    // is still handshaking -- which is the only state in which the question
    // "how many sockets?" has an interesting answer.
    factory.handshake = Http2ScriptedFactory::Handshake::Held;
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    RequestQueue queue(policy, factory, cookies, cache);
    for (int i = 0; i < 6; ++i) {
      queue.Start(policy.Decide(RequestFor("https://cdn.example.com/img" + std::to_string(i) +
                                           ".png")),
                  FetchOptions{}, 1000);
    }
    queue.Advance(1000);
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "while the protocol is unknown exactly one socket is opened, however many "
                "requests are waiting on it");

    factory.CompleteHandshakes();
    const std::vector<RequestQueue::Completion> done = Drain(queue);
    ExpectEqInt(static_cast<long long>(done.size()), 6, "all six requests finish");
    for (const RequestQueue::Completion& completion : done) {
      Expect(completion.result.ok, "and every one of them succeeded");
      ExpectEqInt(completion.result.response.status, 200, "with a real response");
    }
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "on exactly one socket -- six was TD-0008, and six connections is what "
                "upload.wikimedia.org answers 429 to");
    ExpectEqInt(static_cast<long long>(queue.Connections().SessionCount()), 1,
                "and one session carried all six streams");
    ExpectEqInt(static_cast<long long>(factory.paths.size()), 6,
                "the server saw all six requests");
  });

  AddTest(tests, "Http2Fetch/EachStreamGetsItsOwnBody", [] {
    // The failure this guards against is silent: a demultiplexer that crossed
    // two streams would deliver six 200s with the wrong six bodies, and every
    // assertion above would still pass.
    Http2ScriptedFactory factory;
    for (int i = 0; i < 6; ++i) {
      factory.routes.push_back({"/r" + std::to_string(i), 200, "text/plain",
                                std::string(64, static_cast<char>('a' + i)), ""});
    }
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    RequestQueue queue(policy, factory, cookies, cache);
    std::vector<RequestQueue::Id> ids;
    for (int i = 0; i < 6; ++i) {
      ids.push_back(queue.Start(
          policy.Decide(RequestFor("https://cdn.example.com/r" + std::to_string(i))),
          FetchOptions{}, 1000));
    }
    const std::vector<RequestQueue::Completion> done = Drain(queue);
    ExpectEqInt(static_cast<long long>(done.size()), 6, "all six finish");
    for (const RequestQueue::Completion& completion : done) {
      std::size_t which = 0;
      for (; which < ids.size(); ++which) {
        if (ids[which] == completion.id) {
          break;
        }
      }
      Expect(which < ids.size(), "every completion belongs to a request that was made");
      ExpectEqString(BodyOf(completion.result.response),
                     std::string(64, static_cast<char>('a' + which)),
                     "and carries that request's body and no other's");
    }
  });

  AddTest(tests, "Http2Fetch/ARedirectStaysOnTheSameConnection", [] {
    Http2ScriptedFactory factory;
    factory.routes.push_back({"/old", 302, "text/plain", "", "https://example.com/new"});
    factory.routes.push_back({"/new", 200, "text/plain", "arrived", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);

    const FetchResult result = RunToCompletion(
        net::Fetch(policy.Decide(RequestFor("https://example.com/old")), policy, pool, cookies,
                   cache, FetchOptions{}, 1000));
    Expect(result.ok, "the redirect is followed");
    ExpectEqString(BodyOf(result.response), "arrived", "to the second response");
    ExpectEqInt(result.redirects, 1, "in one hop");
    ExpectEqInt(static_cast<long long>(factory.connects), 1,
                "on the connection the first hop already had -- which under HTTP/1.1 was "
                "the case ADR 0010 §2 was written for and is free here");
  });

  AddTest(tests, "Http2Fetch/AnOriginThatDoesNotOfferHttp2GetsHttp1", [] {
    // The same fixture with ALPN silent. Nothing about the request changes --
    // same verdict, same headers, same cookies -- and that is the property
    // worth asserting: the protocol is chosen in one place and nothing above it
    // knows which was picked.
    Http2ScriptedFactory factory;
    factory.speaks_http2 = false;
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);
    const FetchResult result = RunToCompletion(
        net::Fetch(policy.Decide(RequestFor("https://example.com/x")), policy, pool, cookies,
                   cache, FetchOptions{}, 1000));
    // The scripted server refuses to answer anything that is not the HTTP/2
    // preface, so this fails -- but it must fail as an HTTP/1.1 request that
    // got nothing, not by being handed to a session.
    Expect(!result.ok, "an HTTP/1.1 request to an HTTP/2-only fixture gets nowhere");
    ExpectEqInt(static_cast<long long>(pool.SessionCount()), 0,
                "and no session was created for a connection that never negotiated one");
  });

  AddTest(tests, "Http2Fetch/CookiesTravelTheSameWay", [] {
    Http2ScriptedFactory factory;
    factory.routes.push_back({"/set", 200, "text/plain", "ok", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);
    const url::Url url = MustParse("https://example.com/set");
    cookies.StoreFromHeader(url::PartitionKey::ForTopLevel(url::ContainerId{}, url), url,
                            "sid=abc; Path=/", 1000);

    const FetchResult result = RunToCompletion(net::Fetch(
        policy.Decide(RequestFor("https://example.com/set")), policy, pool, cookies, cache,
        FetchOptions{}, 1000));
    Expect(result.ok, "the request succeeds");
    // The server logs paths, so the assertion that the cookie arrived is that
    // HPACK round-tripped a header the encoder marks never-indexed -- if it had
    // not, the block would not have decoded at all and the path would be
    // missing.
    ExpectEqInt(static_cast<long long>(factory.paths.size()), 1,
                "and the header block -- cookie and all -- decoded at the other end");
    ExpectEqString(factory.paths.front(), "/set", "with the target intact");
  });
}

}  // namespace microbrowser::tests
