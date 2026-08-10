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
    ExpectEqInt(static_cast<long long>(factory.paths.size()), 1, "one request arrived");
    ExpectEqString(factory.paths.front(), "/index.html", "at the target that was asked for");
    // Every pseudo-header, not just the one the routing uses. A dangling view
    // in `:authority` made every real server reset the stream while this suite
    // stayed green, because nothing here read it.
    ExpectEqString(factory.authorities.front(), "example.com",
                   "addressed to the host the URL named");
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

  AddTest(tests, "Http2Fetch/AGetSurvivesADeadSharedSessionOnce", [] {
    // TD-0025: one fatal read fails every open stream. GET must retry on a
    // fresh connection; without that, youtube SPA loses player base.js with
    // whatever else shared the socket.
    Http2ScriptedFactory factory;
    factory.die_once_on_path = "/player/base.js";
    factory.routes.push_back({"/player/base.js", 200, "application/javascript", "ok", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);

    const FetchResult result = RunToCompletion(
        net::Fetch(policy.Decide(RequestFor("https://example.com/player/base.js")), policy, pool,
                   cookies, cache, FetchOptions{}, 1000));
    Expect(result.ok, result.error.empty() ? "GET retry after session death failed"
                                           : result.error.c_str());
    ExpectEqString(BodyOf(result.response), "ok", "the body came from the second connection");
    ExpectEqInt(static_cast<long long>(factory.connects), 2,
                "first socket died; second answered");
    ExpectEqInt(static_cast<long long>(factory.paths.size()), 2,
                "the path was asked twice — once on the dead session, once on the retry");
  });

  AddTest(tests, "Http2Fetch/APostSurvivesADeadSharedSessionOnce", [] {
    // TD-0041: SABR `videoplayback` is POST. Session death used to fail those
    // streams without retry while GET recovered — soft-nav then stamped MSE
    // buffers and never appended.
    Http2ScriptedFactory factory;
    factory.die_once_on_path = "/videoplayback";
    factory.routes.push_back({"/videoplayback", 200, "application/octet-stream", "media", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    ConnectionPool pool(factory);

    FetchOptions options;
    options.method = "POST";
    const char payload[] = "sabr";
    options.body.assign(reinterpret_cast<const std::byte*>(payload),
                        reinterpret_cast<const std::byte*>(payload) + sizeof(payload) - 1);
    const FetchResult result = RunToCompletion(
        net::Fetch(policy.Decide(RequestFor("https://example.com/videoplayback")), policy, pool,
                   cookies, cache, options, 1000));
    Expect(result.ok, result.error.empty() ? "POST retry after session death failed"
                                           : result.error.c_str());
    ExpectEqString(BodyOf(result.response), "media", "body from the second connection");
    ExpectEqInt(static_cast<long long>(factory.connects), 2,
                "first socket died; second answered");
    ExpectEqInt(static_cast<long long>(factory.paths.size()), 2,
                "asked twice — dead session then retry");
  });

  AddTest(tests, "Http2Fetch/CancelAllDropsPooledSessions", [] {
    // TD-0044: Navigate → CancelAll left H2 sessions pooled. Consent
    // location.reload() reused a socket the peer had closed after mass RST.
    Http2ScriptedFactory factory;
    factory.routes.push_back({"/a", 200, "text/plain", "a", ""});
    factory.routes.push_back({"/b", 200, "text/plain", "b", ""});
    PrivacyPolicy policy;
    CookieJar cookies;
    HttpCache cache;
    RequestQueue queue(policy, factory, cookies, cache);
    queue.Start(policy.Decide(RequestFor("https://example.com/a")), FetchOptions{}, 1000);
    const std::vector<RequestQueue::Completion> first = Drain(queue);
    ExpectEqInt(static_cast<long long>(first.size()), 1, "warm-up request finished");
    Expect(first[0].result.ok, first[0].result.error.c_str());
    Expect(queue.Connections().SessionCount() >= 1, "session was pooled after the first fetch");
    const std::size_t connects_before = factory.connects;
    queue.CancelAll();
    ExpectEqInt(static_cast<long long>(queue.Connections().SessionCount()), 0,
                "CancelAll drops pooled H2 sessions");
    queue.Start(policy.Decide(RequestFor("https://example.com/b")), FetchOptions{}, 2000);
    const std::vector<RequestQueue::Completion> second = Drain(queue);
    ExpectEqInt(static_cast<long long>(second.size()), 1, "post-CancelAll fetch finished");
    Expect(second[0].result.ok, second[0].result.error.c_str());
    ExpectEqString(BodyOf(second[0].result.response), "b", "fresh connection answered");
    Expect(factory.connects > connects_before,
           "post-CancelAll fetch opened a new socket rather than reviving the old session");
    // A third fetch to the same origin should reuse the ALPN memo (one connect
    // for /b, then reuse) — Clear() used to wipe protocols_ and force a storm.
    queue.Start(policy.Decide(RequestFor("https://example.com/a")), FetchOptions{}, 3000);
    const std::vector<RequestQueue::Completion> third = Drain(queue);
    ExpectEqInt(static_cast<long long>(third.size()), 1, "third fetch finished");
    Expect(third[0].result.ok, third[0].result.error.c_str());
    ExpectEqInt(static_cast<long long>(factory.connects), static_cast<long long>(connects_before + 1),
                "ALPN memo survived CancelAll so the third fetch reused the post-reload session");
  });
}

}  // namespace microbrowser::tests
