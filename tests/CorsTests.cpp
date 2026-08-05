// CORS, at the seam ADR 0020 §2 puts it: inside `net`, on the response, with
// the response *discarded* rather than marked.
//
// Every test here asserts on what came out of `net`, never on what a binding
// would have done with it, because that is the whole claim: a refused response
// does not exist by the time anything above this line could look at one.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "net/ConnectionPool.h"
#include "net/Cors.h"
#include "net/Fetch.h"
#include "net/HttpCache.h"
#include "net/RequestQueue.h"
#include "privacy/PrivacyPolicy.h"
#include "support/ScriptedTransport.h"
#include "url/Origin.h"
#include "url/Url.h"

namespace microbrowser::tests {

namespace {

using net::CookieJar;
using net::CorsParams;
using net::CredentialsMode;
using net::FetchOptions;
using net::FetchResult;
using net::HttpCache;
using net::RequestMode;
using net::RequestQueue;
using privacy::PrivacyPolicy;
using url::Origin;
using url::Url;

Url MustParse(std::string_view text) {
  const auto url = Url::Parse(text);
  Expect(url.has_value(), std::string("expected to parse: ") + std::string(text));
  return *url;
}

Origin OriginOf(std::string_view text) { return Origin::FromUrl(MustParse(text)); }

// The document that is doing the fetching in every test here.
constexpr std::string_view kPage = "https://page.example/";

privacy::Request RequestFor(std::string_view target) {
  privacy::Request request;
  request.url = MustParse(target);
  request.initiator = OriginOf(kPage);
  request.top_level_site = url::Site::FromUrl(MustParse(kPage));
  request.type = privacy::ResourceType::Xhr;
  request.is_subresource = true;
  return request;
}

// A `fetch`'s options: cors mode, this page's origin, and whatever else the
// test is about.
FetchOptions CorsOptions(RequestMode mode = RequestMode::Cors) {
  FetchOptions options;
  options.cors.mode = mode;
  options.cors.origin = OriginOf(kPage);
  return options;
}

// Runs one request through the queue, which is where preflights live. Bounded
// rather than `while (!done)`: a request that stopped moving must fail the test
// rather than hang it.
std::vector<RequestQueue::Completion> RunQueue(RequestQueue& queue, int turns = 200) {
  std::vector<RequestQueue::Completion> out;
  for (int turn = 0; turn < turns && out.empty(); ++turn) {
    queue.Advance(1000 + turn);
    for (RequestQueue::Completion& completion : queue.TakeCompletions()) {
      out.push_back(std::move(completion));
    }
  }
  return out;
}

std::string BodyOf(const FetchResult& result) {
  return std::string(reinterpret_cast<const char*>(result.response.body.data()),
                     result.response.body.size());
}

std::string Response(std::string_view extra_headers, std::string_view body) {
  std::string out = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
  out += std::to_string(body.size());
  out += "\r\n";
  out += extra_headers;
  out += "\r\n";
  out += body;
  return out;
}

// The whole fixture for a queue-level test, so a test body is the case rather
// than the setup.
struct Fixture {
  PrivacyPolicy policy;
  ScriptedFactory factory;
  CookieJar cookies;
  HttpCache cache;
  RequestQueue queue{policy, factory, cookies, cache};

  RequestQueue::Id Start(std::string_view target, const FetchOptions& options,
                         std::int64_t now = 1000) {
    return queue.Start(policy.Decide(RequestFor(target)), options, now);
  }

  // Stores a cookie under the partition key the request itself will be made
  // in, rather than under one the test computed -- which is the difference
  // between testing the credentials mode and testing whether two call sites
  // derived the same key.
  //
  // `SameSite=None; Secure` because everything here is third-party: a Lax
  // cookie does not travel on a cross-site subresource at all, so a test that
  // stored one would pass for the wrong reason.
  void StoreThirdPartyCookie(std::string_view target, std::string_view cookie) {
    const privacy::Verdict verdict = policy.Decide(RequestFor(target));
    cookies.StoreFromHeader(verdict.Partition(), verdict.FinalUrl(), cookie, 1000);
  }
};

}  // namespace

void RegisterCorsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Cors/SameOriginNeedsNoHeader", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"page.example", 443, true, Response("", "same origin")});
    fixture.Start("https://page.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    ExpectEqInt(static_cast<long long>(done.size()), 1, "one completion");
    Expect(done[0].result.ok, "a same-origin response needs no Access-Control-Allow-Origin");
    ExpectEqString(BodyOf(done[0].result), "same origin", "the body arrived");
    Expect(fixture.factory.log.requests.at(0).find("Origin: https://page.example\r\n") !=
               std::string::npos,
           "a cors request says who is asking whatever its target, because a server that "
           "answers Access-Control-Allow-Origin has to have something to answer against");
  });

  AddTest(tests, "Cors/CrossOriginWithoutAllowOriginIsDiscarded", [] {
    Fixture fixture;
    fixture.factory.script.push_back({"api.example", 443, true, Response("", "secret")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    ExpectEqInt(static_cast<long long>(done.size()), 1, "one completion");
    Expect(!done[0].result.ok, "a cross-origin response with no CORS header is refused");
    Expect(done[0].result.response.body.empty(),
           "and the bytes are gone, not hidden: this is the whole of ADR 0020 §2");
    ExpectEqInt(done[0].result.response.status, 0, "no status survives a refusal either");
  });

  AddTest(tests, "Cors/AllowOriginMatchingLetsTheBodyThrough", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "{}")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(done.at(0).result.ok, "the server named this origin");
    ExpectEqString(BodyOf(done[0].result), "{}", "so the body is readable");
    Expect(fixture.factory.log.requests.at(0).find("Origin: https://page.example\r\n") !=
               std::string::npos,
           "and the request said who was asking");
  });

  AddTest(tests, "Cors/AllowOriginNamingSomebodyElseIsRefused", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://other.example\r\n", "secret")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok, "a header naming a different origin permits nothing");
    Expect(done[0].result.response.body.empty(), "and the body is discarded");
  });

  AddTest(tests, "Cors/TwoAllowOriginHeadersAreRefused", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Origin: *\r\n",
                  "secret")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok,
           "two Access-Control-Allow-Origin headers is a misconfiguration or a splitting "
           "attempt, and the specification makes both a failure");
  });

  AddTest(tests, "Cors/SetCookieNeverReachesTheCaller", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: *\r\n"
                  "Set-Cookie: session=abc\r\n"
                  "X-Custom: visible\r\n"
                  "Access-Control-Expose-Headers: X-Custom\r\n",
                  "body")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(done.at(0).result.ok, "the wildcard allows the read");
    Expect(!done[0].result.response.headers.Has("set-cookie"),
           "but Set-Cookie is removed inside net, so the renderer never had it");
    Expect(done[0].result.response.headers.Has("x-custom"),
           "an exposed header survives");
    Expect(done[0].result.response.headers.Has("content-type"),
           "and so does a safelisted one");
  });

  AddTest(tests, "Cors/NoCorsAnswersOpaque", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"cdn.example", 443, true, Response("X-Secret: 1\r\n", "pixels")});
    fixture.Start("https://cdn.example/i.png", CorsOptions(RequestMode::NoCors));
    const auto done = RunQueue(fixture.queue);
    Expect(done.at(0).result.ok, "no-cors succeeds");
    Expect(done[0].result.opaque, "and is opaque");
    ExpectEqInt(done[0].result.response.status, 0, "status 0");
    Expect(done[0].result.response.body.empty(), "no body");
    ExpectEqInt(static_cast<long long>(done[0].result.response.headers.Size()), 0, "no headers");
  });

  AddTest(tests, "Cors/SameOriginModeRefusesACrossOriginUrl", [] {
    Fixture fixture;
    fixture.factory.script.push_back({"api.example", 443, true, Response("", "body")});
    fixture.Start("https://api.example/data", CorsOptions(RequestMode::SameOrigin));
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok, "mode: same-origin refuses a different origin");
  });

  AddTest(tests, "Cors/CredentialsAreOmittedCrossOriginByDefault", [] {
    Fixture fixture;
    fixture.StoreThirdPartyCookie("https://api.example/data",
                                  "sid=secret; Path=/; SameSite=None; Secure");
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "ok")});
    fixture.Start("https://api.example/data", CorsOptions());
    RunQueue(fixture.queue);
    Expect(fixture.factory.log.requests.at(0).find("Cookie:") == std::string::npos,
           "credentials: same-origin -- the default -- sends nothing cross-origin");
  });

  AddTest(tests, "Cors/IncludeNeedsAllowCredentialsAndNoWildcard", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.cors.credentials = CredentialsMode::Include;
    fixture.factory.script.push_back(
        {"api.example", 443, true, Response("Access-Control-Allow-Origin: *\r\n", "ok")});
    fixture.Start("https://api.example/data", options);
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok,
           "a wildcard means 'anyone may read this', which cannot be true of a response "
           "that depended on who asked");
  });

  AddTest(tests, "Cors/IncludeIsAllowedWhenTheServerSaysSo", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.cors.credentials = CredentialsMode::Include;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Credentials: true\r\n",
                  "ok")});
    fixture.Start("https://api.example/data", options);
    const auto done = RunQueue(fixture.queue);
    Expect(done.at(0).result.ok, "named origin plus allow-credentials is the one way through");
  });

  AddTest(tests, "Cors/PreflightPrecedesANonSimpleRequest", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.method = "PUT";
    options.headers.Add("Content-Type", "application/json");
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Methods: PUT\r\n"
                  "Access-Control-Allow-Headers: content-type\r\n",
                  "")});
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "done")});
    fixture.Start("https://api.example/data", options);
    const auto done = RunQueue(fixture.queue);
    ExpectEqInt(static_cast<long long>(done.size()), 1,
                "two exchanges, one completion: the caller never learns it cost a preflight");
    Expect(done[0].result.ok, "and the request went through");
    ExpectEqString(BodyOf(done[0].result), "done", "with the real response");
    const std::string& preflight = fixture.factory.log.requests.at(0);
    Expect(preflight.rfind("OPTIONS /data", 0) == 0, "the first exchange is the OPTIONS");
    Expect(preflight.find("Access-Control-Request-Method: PUT\r\n") != std::string::npos,
           "naming the method");
    Expect(preflight.find("Access-Control-Request-Headers: content-type\r\n") !=
               std::string::npos,
           "and the headers");
    Expect(fixture.factory.log.requests.at(1).rfind("PUT /data", 0) == 0,
           "the second exchange is the request itself");
  });

  AddTest(tests, "Cors/PreflightRefusalStopsTheRequest", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.method = "DELETE";
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Methods: GET\r\n",
                  "")});
    fixture.Start("https://api.example/data", options);
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok, "the preflight did not allow DELETE");
    ExpectEqInt(static_cast<long long>(fixture.factory.log.requests.size()), 1,
                "and the DELETE was never sent, which is the point of asking first");
  });

  AddTest(tests, "Cors/PreflightIsRememberedForItsMaxAge", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.method = "PUT";
    const std::string grant =
        Response("Access-Control-Allow-Origin: https://page.example\r\n"
                 "Access-Control-Allow-Methods: PUT\r\n"
                 "Access-Control-Max-Age: 600\r\n",
                 "");
    fixture.factory.script.push_back({"api.example", 443, true, grant});
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "one")});
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "two")});
    fixture.Start("https://api.example/data", options);
    Expect(RunQueue(fixture.queue).at(0).result.ok, "the first request went through");
    fixture.Start("https://api.example/data", options, 1200);
    Expect(RunQueue(fixture.queue).at(0).result.ok, "and so did the second");
    ExpectEqInt(static_cast<long long>(fixture.factory.log.requests.size()), 3,
                "three exchanges: one preflight and two requests, not two preflights");
    Expect(fixture.factory.log.requests.at(2).rfind("PUT /data", 0) == 0,
           "the second request skipped straight to the PUT");
  });

  AddTest(tests, "Cors/PreflightWithoutMaxAgeIsNotCached", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.method = "PUT";
    const std::string grant =
        Response("Access-Control-Allow-Origin: https://page.example\r\n"
                 "Access-Control-Allow-Methods: PUT\r\n",
                 "");
    const std::string ok =
        Response("Access-Control-Allow-Origin: https://page.example\r\n", "ok");
    for (int i = 0; i < 2; ++i) {
      fixture.factory.script.push_back({"api.example", 443, true, grant});
      fixture.factory.script.push_back({"api.example", 443, true, ok});
    }
    fixture.Start("https://api.example/data", options);
    Expect(RunQueue(fixture.queue).at(0).result.ok, "first");
    fixture.Start("https://api.example/data", options, 1001);
    Expect(RunQueue(fixture.queue).at(0).result.ok, "second");
    ExpectEqInt(static_cast<long long>(fixture.factory.log.requests.size()), 4,
                "a server that did not ask to be remembered is asked again");
  });

  AddTest(tests, "Cors/PreflightCarriesNoCookies", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.method = "PUT";
    options.cors.credentials = CredentialsMode::Include;
    fixture.StoreThirdPartyCookie("https://api.example/data",
                                  "sid=secret; Path=/; SameSite=None; Secure");
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Credentials: true\r\n"
                  "Access-Control-Allow-Methods: PUT\r\n",
                  "")});
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n"
                  "Access-Control-Allow-Credentials: true\r\n",
                  "ok")});
    fixture.Start("https://api.example/data", options);
    Expect(RunQueue(fixture.queue).at(0).result.ok, "the request went through");
    Expect(fixture.factory.log.requests.at(0).find("Cookie:") == std::string::npos,
           "the OPTIONS asks whether credentials are allowed; sending them would answer "
           "the question in advance");
    Expect(fixture.factory.log.requests.at(1).find("Cookie: sid=secret") != std::string::npos,
           "and the request that was permitted carries them");
  });

  AddTest(tests, "Cors/ACrossOriginRedirectTaintsTheOrigin", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         "HTTP/1.1 302 Found\r\nLocation: https://other.example/x\r\n"
         "Access-Control-Allow-Origin: https://page.example\r\nContent-Length: 0\r\n\r\n"});
    fixture.factory.script.push_back(
        {"other.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "body")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok,
           "after a cross-origin hop the origin is null, so a header naming the page's real "
           "origin permits nothing -- otherwise a third party could spend a request it was "
           "only asked to forward");
    Expect(fixture.factory.log.requests.at(1).find("Origin: null\r\n") != std::string::npos,
           "and the second hop said so on the wire");
  });

  AddTest(tests, "Cors/ARedirectWithoutAllowOriginIsRefusedAtTheHop", [] {
    Fixture fixture;
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         "HTTP/1.1 302 Found\r\nLocation: https://api.example/x\r\nContent-Length: 0\r\n\r\n"});
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "body")});
    fixture.Start("https://api.example/data", CorsOptions());
    const auto done = RunQueue(fixture.queue);
    Expect(!done.at(0).result.ok,
           "a redirect is a response too, and a cross-origin one has to permit this origin "
           "before it is allowed to send the request somewhere else");
  });

  AddTest(tests, "Cors/APageCannotForgeItsOwnOriginHeader", [] {
    Fixture fixture;
    FetchOptions options = CorsOptions();
    options.headers.Add("Origin", "https://bank.example");
    options.headers.Add("Access-Control-Request-Method", "DELETE");
    fixture.factory.script.push_back(
        {"api.example", 443, true,
         Response("Access-Control-Allow-Origin: https://page.example\r\n", "ok")});
    fixture.Start("https://api.example/data", options);
    RunQueue(fixture.queue);
    const std::string& request = fixture.factory.log.requests.at(0);
    Expect(request.find("Origin: https://bank.example") == std::string::npos,
           "a page that could write Origin could name an origin it does not have");
    Expect(request.find("Access-Control-Request-Method") == std::string::npos,
           "and one that could write Access-Control-Request-* could claim a preflight it "
           "never made");
    Expect(request.find("Origin: https://page.example\r\n") != std::string::npos,
           "the header that goes out is the one net computed");
  });

  AddTest(tests, "Cors/BrowserModeIsUnchanged", [] {
    Fixture fixture;
    fixture.factory.script.push_back({"cdn.example", 443, true, Response("", "pixels")});
    FetchOptions options;  // The default: RequestMode::Browser.
    fixture.Start("https://cdn.example/i.png", options);
    const auto done = RunQueue(fixture.queue);
    Expect(done.at(0).result.ok,
           "an image, a stylesheet and a navigation are not no-cors fetches that happen to "
           "be readable: they are the browser's own requests and CORS does not apply");
    ExpectEqString(BodyOf(done[0].result), "pixels", "and their bytes arrive whole");
    Expect(fixture.factory.log.requests.at(0).find("Origin:") == std::string::npos,
           "with no Origin header, exactly as before ADR 0020");
  });

  AddTest(tests, "Cors/CancelDropsARequestWithoutACompletion", [] {
    Fixture fixture;
    fixture.factory.delivery = ScriptedFactory::Delivery::Held;
    fixture.factory.script.push_back({"api.example", 443, true, Response("", "late")});
    const RequestQueue::Id id = fixture.Start("https://api.example/slow", CorsOptions());
    fixture.queue.Advance(1000);
    Expect(fixture.queue.InFlight() == 1, "the request is on the wire");
    Expect(fixture.queue.Cancel(id), "and it can be cancelled");
    fixture.factory.ReleaseAll();
    fixture.queue.Advance(1001);
    Expect(fixture.queue.TakeCompletions().empty(),
           "an aborted request produces no completion, so its `then` cannot run");
    Expect(fixture.queue.IsIdle(), "and nothing is left of it");
  });

  // --- The pure functions, without a socket ---------------------------------

  AddTest(tests, "Cors/SafelistedRequestHeaders", [] {
    Expect(net::IsCorsSafelistedRequestHeader("Accept", "text/html"), "accept is safelisted");
    Expect(net::IsCorsSafelistedRequestHeader("Content-Type", "text/plain; charset=utf-8"),
           "text/plain is, parameters and all");
    Expect(!net::IsCorsSafelistedRequestHeader("Content-Type", "application/json"),
           "application/json is not, which is why nearly every JSON API costs a preflight");
    Expect(!net::IsCorsSafelistedRequestHeader("Authorization", "Bearer x"),
           "and neither is anything a form could not have sent");
    Expect(!net::IsCorsSafelistedRequestHeader("Accept", std::string(200, 'a')),
           "a 200-byte Accept is not a header a form could have sent either");
  });

  AddTest(tests, "Cors/PreflightHeaderListIsSortedAndFolded", [] {
    net::HttpHeaders headers;
    headers.Add("X-Zulu", "1");
    headers.Add("Content-Type", "application/json");
    headers.Add("X-Alpha", "2");
    ExpectEqString(net::PreflightRequestHeaderList(headers),
                   "content-type,x-alpha,x-zulu",
                   "sorted and folded, because a server that matches the string rather than "
                   "the set is common enough to decide whether the request works");
  });

  AddTest(tests, "Cors/MaxAgeIsBoundedAndUnparseable", [] {
    const auto grant_for = [](std::string_view max_age) {
      net::HttpResponse response;
      response.status = 204;
      response.headers.Add("Access-Control-Allow-Origin", "https://page.example");
      response.headers.Add("Access-Control-Max-Age", max_age);
      CorsParams params;
      params.mode = RequestMode::Cors;
      params.origin = OriginOf(kPage);
      net::PreflightGrant grant;
      net::HttpHeaders none;
      const net::CorsResult decision = net::CheckPreflight(
          params, "GET", none, MustParse("https://api.example/"), response, grant);
      Expect(decision.allowed, "the preflight itself is fine");
      return grant.max_age_seconds;
    };
    ExpectEqInt(grant_for("600"), 600, "an ordinary value is kept");
    ExpectEqInt(grant_for("99999999999999999999"), 86400,
                "and one that would overflow saturates: a permission that never expired "
                "because its expiry wrapped is one the server can no longer revoke");
    ExpectEqInt(grant_for("-1"), 0, "a value that is not digits caches nothing");
    ExpectEqInt(grant_for("6e3"), 0, "and neither does one that only starts with digits");
  });

  AddTest(tests, "Cors/PreflightCacheIsKeyedByPartition", [] {
    net::PreflightCache cache;
    CorsParams params;
    params.mode = RequestMode::Cors;
    params.origin = OriginOf(kPage);
    net::PreflightGrant grant;
    grant.methods = {"PUT"};
    grant.max_age_seconds = 600;
    const Url api = MustParse("https://api.example/data");
    net::HttpHeaders none;
    cache.Store("site-a", params, api, grant, 1000);
    Expect(cache.Allows("site-a", params, api, "PUT", none, 1100),
           "the site that paid for the preflight has it");
    Expect(!cache.Allows("site-b", params, api, "PUT", none, 1100),
           "and the next site does not -- a preflight cache is a cross-site linkage like "
           "any other cache, measurable as the absence of an OPTIONS on the wire");
    Expect(!cache.Allows("site-a", params, api, "PUT", none, 2000),
           "an expired grant permits nothing");
  });
}

}  // namespace microbrowser::tests
