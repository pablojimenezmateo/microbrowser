#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "wpt/Handlers.h"

// The web-platform-tests `.py` handlers this server answers natively.
//
// **These had no tests at all, and the cost of that is the reason this file exists.** A handler is
// only reachable by running the suite, and the suite is the better part of a day; a transcription
// that got a header name or a default wrong showed up as a mysterious failure in an unrelated test,
// hours later, with nothing pointing at the server. Every handler is a pure function of
// (request -> response), so the whole table is checkable in milliseconds, and there was no reason
// for it not to be.
//
// What is asserted here is *the original's* behaviour, not this implementation's. Where the two
// differ the original wins -- a test was written against it -- so each case below names the
// behaviour it is pinning and, where that behaviour looks like a mistake, why it is not.
//
// The cases that earn their place most are the ones where a plausible implementation is wrong:
// "NO" for a missing header rather than omitting it, an empty `Content-Type` being a state rather
// than an absence, and a handler name that means two different handlers depending on its directory.

namespace microbrowser::tests {

namespace {

using wpt::HandlerRequest;
using wpt::HandlerResponse;
using wpt::Stash;

struct Call {
  std::string method = "GET";
  std::string path;
  std::string query;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string origin = "http://localhost:8000";
  std::string wpt_root;
};

// A GET, a POST. Factories rather than designated initializers because this build treats a
// partially-designated aggregate as an error, and because naming the two shapes reads better than
// spelling out the defaults at thirty call sites.
Call Get(std::string path, std::string query = {}) {
  Call call;
  call.path = std::move(path);
  call.query = std::move(query);
  return call;
}

Call Post(std::string path, std::string query, std::string body) {
  Call call;
  call.method = "POST";
  call.path = std::move(path);
  call.query = std::move(query);
  call.body = std::move(body);
  return call;
}

// Invokes a handler and answers whether one was found, with the response in `out`.
bool Invoke(const Call& call, Stash& stash, HandlerResponse* out) {
  HandlerRequest request;
  request.method = call.method;
  request.path = call.path;
  request.query = call.query;
  request.headers = &call.headers;
  request.body = call.body;
  request.origin = call.origin;
  request.wpt_root = call.wpt_root;
  return wpt::InvokeHandler(request, stash, out);
}

// The value of one response header, or a sentinel that cannot be confused with a real one.
std::string HeaderValue(const HandlerResponse& response, std::string_view name) {
  const std::string prefix = std::string(name) + ": ";
  for (const std::string& line : response.headers) {
    if (line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0) {
      return line.substr(prefix.size());
    }
  }
  return "<absent>";
}

bool HasHeader(const HandlerResponse& response, std::string_view name) {
  return HeaderValue(response, name) != "<absent>";
}

}  // namespace

void RegisterWptHandlerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptHandlers/AnUnknownHandlerIsRefusedRatherThanInvented", [] {
    // The opening argument of Handlers.h, as a test. An unknown handler must not be answered with an
    // empty 200: that turns "nobody has written this" into "this test fails for an unknown reason",
    // and the ranked report is how the next handler gets chosen.
    Stash stash;
    HandlerResponse response;
    Expect(!Invoke(Get("fetch/api/resources/nobody-wrote-this.py"), stash, &response),
           "an unimplemented handler is not answered");
  });

  AddTest(tests, "WptHandlers/OneNameCanMeanTwoHandlers", [] {
    // `status-code.py` exists twice in the checkout with two unrelated bodies, and dispatching on
    // the file name alone served one of them for both paths. This is the regression test for that,
    // and the pattern the rest of TD-0061 has to follow.
    Stash stash;

    // fetch/h1-parsing/: a raw response, because the malformed status line *is* the test. Bare LF
    // and no framing, both deliberate.
    HandlerResponse raw;
    Expect(Invoke(Get("fetch/h1-parsing/resources/status-code.py", "input=200 OK"),
                  stash, &raw),
           "the h1-parsing copy answers");
    Expect(raw.has_raw, "and answers with raw bytes");
    ExpectEqString(raw.raw, "HTTP/1.1 200 OK\nheader-parsing: is sad\n",
                   "which are the original's, bare LF included");

    // resource-timing/: an ordinary response whose subject is two headers.
    HandlerResponse ordinary;
    Expect(Invoke(Get("resource-timing/resources/status-code.py",
                      "status=404&tao_value=*&allow_origin=*"),
                  stash, &ordinary),
           "the resource-timing copy answers");
    Expect(!ordinary.has_raw, "and is not raw");
    ExpectEqInt(ordinary.status, 404, "with the status the test asked for");
    ExpectEqString(HeaderValue(ordinary, "Timing-Allow-Origin"), "*", "and Timing-Allow-Origin");
    ExpectEqString(HeaderValue(ordinary, "Access-Control-Allow-Origin"), "*", "and the CORS header");

    // And a third directory gets neither, rather than whichever was written first.
    HandlerResponse elsewhere;
    Expect(!Invoke(Get("made/up/resources/status-code.py", "status=200"), stash,
                   &elsewhere),
           "a copy nobody transcribed stays a 501 rather than borrowing one of these");
  });

  AddTest(tests, "WptHandlers/ImageIsScopedToTheCopyThatWasTranscribed", [] {
    // Four `image.py` files, four different handlers: a PNG with nosniff, a different PNG with
    // Cross-Origin-Resource-Policy, a BMP generated in Python, and one alternating green and red to
    // test an ETag. Nothing subsets anything, so three of them must stay refused.
    Stash stash;
    HandlerResponse served;
    Expect(Invoke(Get("fetch/nosniff/resources/image.py", "type=image/png"), stash,
                  &served),
           "the nosniff copy answers");
    ExpectEqString(HeaderValue(served, "X-Content-Type-Options"), "nosniff",
                   "with the header the test is about");
    // No checkout given, so the body is empty rather than invented -- the test then fails on the
    // image, which is honest, where made-up bytes would fail on the decoder.
    Expect(served.body.empty(), "and an empty body when there is no checkout to read from");

    for (const char* path : {"fetch/cross-origin-resource-policy/resources/image.py",
                             "common/security-features/subresource/image.py",
                             "html/dom/elements/images/image.py"}) {
      HandlerResponse other;
      Expect(!Invoke(Get(path), stash, &other),
             std::string("a different image.py is refused rather than answered: ") + path);
    }
  });

  AddTest(tests, "WptHandlers/AMissingRequestHeaderIsReportedAsNoRatherThanOmitted", [] {
    // `method.py` reports what arrived as `x-request-*` headers, and reports the string "NO" when a
    // header was absent. Omitting the header instead would look tidier and would break the tests:
    // they distinguish "the browser sent no Content-Type" from "the report never arrived", and only
    // a present-but-"NO" value can say the first.
    Stash stash;
    HandlerResponse response;
    Call put = Get("fetch/api/resources/method.py");
    put.method = "PUT";
    put.body = "payload";
    Expect(Invoke(put, stash, &response),
           "method.py answers");
    ExpectEqString(HeaderValue(response, "x-request-method"), "PUT", "reporting the method");
    ExpectEqString(HeaderValue(response, "x-request-content-type"), "NO",
                   "and NO for a header that did not arrive");
    ExpectEqString(response.body, "payload", "and echoing the body");
    // No `cors` parameter, so no CORS headers at all -- a handler that always sent them would make
    // every no-cors test pass for the wrong reason.
    Expect(!HasHeader(response, "Access-Control-Allow-Origin"),
           "and no CORS header unless the test asked for one");

    HandlerResponse cors;
    Expect(Invoke(Get("fetch/api/resources/method.py", "cors"), stash, &cors),
           "with cors it answers too");
    ExpectEqString(HeaderValue(cors, "Access-Control-Allow-Origin"), "*", "and allows the origin");
    ExpectEqString(HeaderValue(cors, "Access-Control-Expose-Headers"), "x-request-method",
                   "and exposes what the test reads");
  });

  AddTest(tests, "WptHandlers/RecordHeadersKeepsTheFirstRecordingAndAnswers204WhenEmpty", [] {
    // The whole of how fetch/metadata/ is written: one request records its headers under a key, a
    // later one retrieves them as JSON.
    Stash stash;
    const char* path = "fetch/metadata/resources/record-headers.py";

    // Nothing recorded yet. 204 rather than an empty 200, which is how the test tells "the request
    // never arrived" from "it arrived carrying nothing".
    HandlerResponse empty;
    Expect(Invoke(Get(path, "retrieve&key=abc"), stash, &empty), "retrieve answers");
    ExpectEqInt(empty.status, 204, "with 204 when nothing was recorded");

    HandlerResponse recorded;
    Call record = Get(path, "key=abc");
    record.headers.emplace_back("Sec-Fetch-Mode", "navigate");
    Expect(Invoke(record, stash, &recorded), "recording answers");
    ExpectEqString(HeaderValue(recorded, "Access-Control-Allow-Origin"), "*", "allowing CORS");

    // The original's `except KeyError: pass` means a second record under one key is ignored, so the
    // *first* wins. A handler that overwrote would silently record a later request's headers.
    HandlerResponse second;
    Call again = Get(path, "key=abc");
    again.headers.emplace_back("Sec-Fetch-Mode", "cors");
    Expect(Invoke(again, stash, &second), "a second record answers");

    HandlerResponse retrieved;
    Expect(Invoke(Get(path, "retrieve&key=abc"), stash, &retrieved),
           "and the retrieval answers");
    ExpectEqInt(retrieved.status, 200, "with 200 now that something is there");
    Expect(retrieved.body.find("\"Sec-Fetch-Mode\": \"navigate\"") != std::string::npos,
           "carrying the first recording as JSON: " + retrieved.body);
    Expect(retrieved.body.find("cors") == std::string::npos,
           "and not the second, which the original discards: " + retrieved.body);

    // Taken, so it is gone -- the stash delivers once.
    HandlerResponse drained;
    Expect(Invoke(Get(path, "retrieve&key=abc"), stash, &drained), "a third answers");
    ExpectEqInt(drained.status, 204, "with 204 again, because a retrieval consumes");
  });

  AddTest(tests, "WptHandlers/RecordHeadersIgnoresEverythingButOptionsWhenAsked", [] {
    // `requireOPTIONS` exists to avoid a false positive from a CORS preflight, where the request
    // under test is immediately preceded by an OPTIONS to the same URL. Without it the recorded
    // headers would be the preflight's, and the assertion would be about the wrong request.
    Stash stash;
    const char* path = "fetch/metadata/resources/record-headers.py";
    Call get = Get(path, "requireOPTIONS&key=k");
    get.headers.emplace_back("Sec-Fetch-Mode", "cors");
    HandlerResponse ignored;
    Expect(Invoke(get, stash, &ignored), "a GET still answers");
    HandlerResponse after;
    Expect(Invoke(Get(path, "retrieve&key=k"), stash, &after), "retrieve answers");
    ExpectEqInt(after.status, 204, "but the GET recorded nothing");

    Call options = Get(path, "requireOPTIONS&key=k");
    options.method = "OPTIONS";
    options.headers.emplace_back("Sec-Fetch-Mode", "cors");
    HandlerResponse taken;
    Expect(Invoke(options, stash, &taken), "an OPTIONS answers");
    HandlerResponse then;
    Expect(Invoke(Get(path, "retrieve&key=k"), stash, &then), "retrieve answers");
    ExpectEqInt(then.status, 200, "and the OPTIONS is what got recorded");
  });

  AddTest(tests, "WptHandlers/DispatcherIsAFifoPerUuid", [] {
    // The transport under `dispatcher.js`, and by request count the most-asked-for handler in the
    // tree. A queue, not a slot: two messages posted before either is read must both arrive, in
    // order, or a test that sends a pair silently loses one.
    Stash stash;
    const char* path = "common/dispatcher/dispatcher.py";

    HandlerResponse empty;
    Expect(Invoke(Get(path, "uuid=one"), stash, &empty), "a read answers");
    ExpectEqString(empty.body, "not ready",
                   "with the string dispatcher.js polls for, not an empty body");

    HandlerResponse first;
    Expect(Invoke(Post(path, "uuid=one", "alpha"), stash,
                  &first),
           "a post answers");
    ExpectEqString(first.body, "done", "with done");
    HandlerResponse second;
    Expect(Invoke(Post(path, "uuid=one", "beta"), stash,
                  &second),
           "a second post answers");

    HandlerResponse read_one;
    Expect(Invoke(Get(path, "uuid=one"), stash, &read_one), "the first read answers");
    ExpectEqString(read_one.body, "alpha", "with the first message");
    HandlerResponse read_two;
    Expect(Invoke(Get(path, "uuid=one"), stash, &read_two), "the second read answers");
    ExpectEqString(read_two.body, "beta", "with the second, in order");
    HandlerResponse drained;
    Expect(Invoke(Get(path, "uuid=one"), stash, &drained), "a third read answers");
    ExpectEqString(drained.body, "not ready", "and the queue is empty again");
  });

  AddTest(tests, "WptHandlers/DispatcherQueuesSurviveABodyWithANewlineInIt", [] {
    // The reason the queue is length-prefixed rather than newline-delimited. A posted message is an
    // arbitrary body -- `dispatcher.js` sends JSON and HTML through it -- and any separator that can
    // occur in a payload splits one message into two, which reads as an extra message arriving
    // rather than as corruption.
    Stash stash;
    const char* path = "common/dispatcher/dispatcher.py";
    const std::string payload = "line one\nline two\n\nline three";
    HandlerResponse posted;
    Expect(Invoke(Post(path, "uuid=nl", payload), stash,
                  &posted),
           "a multi-line post answers");
    HandlerResponse read;
    Expect(Invoke(Get(path, "uuid=nl"), stash, &read), "the read answers");
    ExpectEqString(read.body, payload, "with the body intact, newlines and all");
    HandlerResponse after;
    Expect(Invoke(Get(path, "uuid=nl"), stash, &after), "and the next read answers");
    ExpectEqString(after.body, "not ready", "with nothing left -- one message, not three");
  });

  AddTest(tests, "WptHandlers/DispatcherQueuesAreIndependentAndReflectTheOrigin", [] {
    Stash stash;
    const char* path = "common/dispatcher/dispatcher.py";
    HandlerResponse seeded;
    (void)Invoke(Post(path, "uuid=a", "for-a"), stash, &seeded);
    HandlerResponse other;
    Expect(Invoke(Get(path, "uuid=b"), stash, &other), "a read on another uuid answers");
    ExpectEqString(other.body, "not ready", "and does not see the first uuid's message");

    // Credentials are allowed, so the origin must be reflected rather than `*`: every CORS
    // implementation, including this browser's, rejects `*` with credentials.
    Call call = Get(path, "uuid=c");
    call.headers.emplace_back("Origin", "http://elsewhere.localhost:8001");
    HandlerResponse response;
    Expect(Invoke(call, stash, &response), "a read with an Origin answers");
    ExpectEqString(HeaderValue(response, "Access-Control-Allow-Origin"),
                   "http://elsewhere.localhost:8001", "reflecting the origin");
    ExpectEqString(HeaderValue(response, "Access-Control-Allow-Credentials"), "true",
                   "which is why it cannot be a star");
  });

  AddTest(tests, "WptHandlers/ARawResponseIsExactlyWhatTheTestAskedFor", [] {
    // These handlers exist to hand this browser's HTTP parser something no well-formed server would
    // send. The assertion is byte-for-byte because every byte is the subject: a `Content-Length` the
    // server helpfully corrected, or a `Connection` header it added, would repair the malformation
    // under test.
    Stash stash;

    HandlerResponse length;
    Expect(Invoke(Get("fetch/content-length/resources/content-length.py",
                      "length=Content-Length: 30, 42"),
                  stash, &length),
           "content-length.py answers");
    Expect(length.has_raw, "with raw bytes");
    Expect(length.raw.find("Content-Length: 30, 42\r\n") != std::string::npos,
           "carrying the deliberately wrong header the test supplied: " + length.raw);
    Expect(length.raw.find("Fact: this is really forty-two bytes long.") != std::string::npos,
           "and the forty-two byte body");

    HandlerResponse nosniff;
    Expect(Invoke(Get("fetch/nosniff/resources/nosniff.py",
                      "nosniff=X-Content-Type-Options: nosniff"),
                  stash, &nosniff),
           "nosniff.py answers");
    Expect(nosniff.raw.find("HTTP/1.1 220 YOU HAVE NO POWER HERE\r\n") == 0,
           "with status 220, which is deliberate: " + nosniff.raw);
    Expect(nosniff.raw.find("Content-Type: x/x\r\n") != std::string::npos,
           "and the unsniffable type the test is about");

    HandlerResponse expose;
    Expect(Invoke(Get("cors/resources/expose-headers.py",
                      "expose=Access-Control-Expose-Headers: BB-8"),
                  stash, &expose),
           "expose-headers.py answers");
    Expect(expose.raw.find("HTTP/1.1 221 ALL YOUR BASE BELONG TO H1\r\n") == 0,
           "with status 221: " + expose.raw);
    Expect(expose.raw.find("BB-8: hey\r\n") != std::string::npos,
           "and the header the test then checks is exposed");
  });

  AddTest(tests, "WptHandlers/AnEmptyContentTypeIsAStateRatherThanAnAbsence", [] {
    // `status.py?type=` is how a test asks for an *empty* `Content-Type`, and a test asserts on it.
    // Folding a present-but-empty parameter into the default would silently send `text/plain` and
    // the test would be checking the wrong thing -- which is why `Query::First` distinguishes them.
    Stash stash;
    HandlerResponse empty;
    Expect(Invoke(Get("xhr/resources/status.py", "code=200&type="), stash, &empty),
           "status.py answers");
    Expect(empty.send_content_type, "sending a Content-Type");
    ExpectEqString(empty.content_type, "", "and it is empty, deliberately");

    HandlerResponse absent;
    Expect(Invoke(Get("xhr/resources/status.py", "code=200"), stash, &absent),
           "and with no type parameter it answers too");
    ExpectEqInt(absent.status, 200, "with the status asked for");
  });

  AddTest(tests, "WptHandlers/CorsEnabledReportsTheRequestAndIsScopedAwayFromTheAuthCopies", [] {
    Stash stash;
    Call call = Post("xhr/resources/corsenabled.py", "safelist_content_type", "sent");
    call.headers.emplace_back("Content-Type", "text/plain");
    HandlerResponse response;
    Expect(Invoke(call, stash, &response), "corsenabled.py answers");
    ExpectEqString(response.body, "Test", "with the original's body");
    ExpectEqString(HeaderValue(response, "X-Request-Method"), "POST", "reporting the method");
    ExpectEqString(HeaderValue(response, "X-Request-Data"), "sent", "and the body");
    ExpectEqString(HeaderValue(response, "X-Request-Query"), "safelist_content_type",
                   "and the query as it arrived");
    // A query that is genuinely absent reports "NO", not an empty string.
    HandlerResponse bare;
    Expect(Invoke(Get("xhr/resources/corsenabled.py"), stash, &bare), "and with no query");
    ExpectEqString(HeaderValue(bare, "X-Request-Query"), "NO", "it reports NO");

    // The two under auth*/ delegate to authentication.py and are unrelated handlers.
    for (const char* path : {"xhr/resources/auth2/corsenabled.py", "xhr/resources/auth7/corsenabled.py"}) {
      HandlerResponse auth;
      Expect(!Invoke(Get(path), stash, &auth),
             std::string("the auth copy is refused rather than answered: ") + path);
    }
  });

  AddTest(tests, "WptHandlers/TheStashDeliversOnceAndIsScopedByDirectory", [] {
    // `stash.take` is a read *and* a remove, which is what makes "ask twice and the second answer is
    // null" a thing a test can assert. And the scope is the directory, exactly as wptserve scopes it:
    // two tests in different directories using the same key must not collide.
    Stash stash;
    stash.Put("fetch/api/resources/", "shared", "first");
    stash.Put("xhr/resources/", "shared", "second");
    std::string value;
    Expect(stash.Take("fetch/api/resources/", "shared", &value), "the first is there");
    ExpectEqString(value, "first", "with its own value");
    Expect(stash.Take("xhr/resources/", "shared", &value), "and so is the second");
    ExpectEqString(value, "second", "undisturbed by the first");
    Expect(!stash.Take("fetch/api/resources/", "shared", &value),
           "and a second take finds nothing, because a take removes");
  });

  AddTest(tests, "WptHandlers/EveryImplementedNameIsReachable", [] {
    // `ImplementedHandlers()` is what a diagnostic prints, so a name on that list that no dispatch
    // arm answers would be a lie told to whoever is choosing the next handler to write. The three
    // directory-scoped names are the interesting case: they are reachable, but only from the path
    // they were transcribed from, so this walks the list with a plausible path for each.
    Stash stash;
    for (const std::string_view name : wpt::ImplementedHandlers()) {
      // The directory each scoped name needs. Everything else is name-dispatched and any directory
      // will do.
      std::string directory = "fetch/api/resources/";
      if (name == "status-code.py" ) {
        directory = "fetch/h1-parsing/resources/";
      } else if (name == "image.py") {
        directory = "fetch/nosniff/resources/";
      } else if (name == "corsenabled.py") {
        directory = "xhr/resources/";
      }
      HandlerResponse response;
      // Enough parameters to satisfy the ones that read a required key, since a handler that threw
      // on a missing parameter would be a crash in the server.
      const Call call = Get(directory + std::string(name),
                            "key=k&uuid=u&status=200&code=200&input=200 OK&location=/&token=t");
      Expect(Invoke(call, stash, &response),
             std::string("a name ImplementedHandlers() reports is answered: ") + std::string(name));
    }
  });
}

}  // namespace microbrowser::tests
