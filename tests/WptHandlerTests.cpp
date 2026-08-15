#include <string>
#include <vector>

#include "TestSupport.h"
#include "wpt/Handlers.h"

// The transcribed `.py` handlers, and the one property that makes them safe to have at all.
//
// ADR 0040 §2 refused to implement handlers because "approximating one makes a test pass for the
// wrong reason, which is worse than a failure". The amendment that allowed these named two rules,
// and the first one is asserted here: **the set is closed**. A path that is not on the list must
// still answer `handled == false`, which the server turns into the 501 it always gave -- so a test
// needing an unimplemented handler still fails visibly rather than getting a plausible 200.
//
// The rest of these are the parameter defaults, because a default is where a transcription silently
// diverges: `status.py` answering 200 instead of its `OMG` reason phrase, or `slow.py` waiting zero
// instead of two seconds, is a test that passes or hangs for a reason nobody would look for.

namespace microbrowser::tests {

namespace {

using wpt::HandlerRequest;
using wpt::HandlerResponse;

std::string HeaderOf(const HandlerResponse& response, std::string_view name) {
  for (const auto& [key, value] : response.headers) {
    if (key.size() == name.size()) {
      bool same = true;
      for (std::size_t i = 0; i < key.size(); ++i) {
        same = same && (std::tolower(static_cast<unsigned char>(key[i])) ==
                        std::tolower(static_cast<unsigned char>(name[i])));
      }
      if (same) {
        return value;
      }
    }
  }
  return {};
}

HandlerRequest Get(std::string path, std::string query) {
  HandlerRequest request;
  request.method = "GET";
  request.path = std::move(path);
  request.query = std::move(query);
  request.origin = "http://localhost:8000";
  return request;
}

}  // namespace

void RegisterWptHandlerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptHandlers/TheSetIsClosedAndEverythingElseIsStill501", [] {
    wpt::Stash stash;
    // A file that exists in the checkout and is *not* transcribed.
    const HandlerResponse missing = wpt::RunHandler(Get("fetch/api/resources/trickle.py", ""), stash);
    Expect(!missing.handled,
           "a handler that is not on the list is not handled, so the server still answers 501 -- "
           "which is the property ADR 0040 §2 was protecting");
    // And a name close enough to one that is, in a different directory. Three `redirect.py` files
    // exist with three different behaviours; dispatch is on the whole path for that reason.
    const HandlerResponse elsewhere =
        wpt::RunHandler(Get("workers/modules/resources/redirect.py", ""), stash);
    Expect(!elsewhere.handled,
           "and a same-named file in another directory is a different handler, not this one");
    Expect(wpt::RunHandler(Get("common/slow.py", ""), stash).handled,
           "while one that is on the list is handled");
  });

  AddTest(tests, "WptHandlers/EncodingHandlersServeTheCharsetTheyWereAsked", [] {
    // encoding/resources/text-plain-charset.py and single-byte-raw.py: the iframe
    // and XHR single-byte decoder tests fetch these, and a 501 is 168 failures
    // apiece rather than a missing handler.
    wpt::Stash stash;
    const HandlerResponse plain =
        wpt::RunHandler(Get("encoding/resources/text-plain-charset.py", "label=ibm866"), stash);
    Expect(plain.handled, "text-plain-charset.py is on the list");
    Expect(HeaderOf(plain, "Content-Type") == "text/plain;charset=ibm866",
           "and the charset is the label it was asked for, with no space after the semicolon");
    Expect(plain.body == "hello encoding", "and the body is the file's constant");

    const HandlerResponse raw =
        wpt::RunHandler(Get("encoding/resources/single-byte-raw.py", "label=windows-1252"), stash);
    Expect(raw.handled, "single-byte-raw.py is on the list");
    Expect(raw.body.size() == 255, "bytes(range(255)) is 0..254");
    Expect(static_cast<unsigned char>(raw.body.front()) == 0 &&
               static_cast<unsigned char>(raw.body.back()) == 254,
           "from 0 through 254");
    Expect(HeaderOf(raw, "Content-Type") == "text/plain;charset=windows-1252",
           "and the charset is the label");
  });

  AddTest(tests, "WptHandlers/DefaultsAreTheOnesTheFilesWrite", [] {
    wpt::Stash stash;
    // common/slow.py: `float(request.GET.first(b"delay", 2000))`.
    Expect(wpt::RunHandler(Get("common/slow.py", ""), stash).delay_ms == 2000,
           "slow.py with no `delay` waits two seconds, which is its own default");
    Expect(wpt::RunHandler(Get("common/slow.py", "delay=50"), stash).delay_ms == 50,
           "and honours the one it is given");

    // fetch/api/resources/status.py: code 200, text "OMG".
    const HandlerResponse status =
        wpt::RunHandler(Get("fetch/api/resources/status.py", "code=404"), stash);
    Expect(status.status == 404, "status.py answers the code it was asked for");
    Expect(status.status_text == "OMG",
           "and its reason phrase default is `OMG`, which is what the file says and what a test "
           "that reads `statusText` compares against");
    Expect(HeaderOf(status, "X-Request-Method") == "GET",
           "and it reports the method back, which is how a POST test tells them apart");
  });

  AddTest(tests, "WptHandlers/AnAbsentHeaderIsTheStringNO", [] {
    // `request.headers.get(b"Content-Type", b"NO")`. The literal is asserted against by the tests,
    // so an empty string here would fail them for a reason that has nothing to do with the browser.
    wpt::Stash stash;
    HandlerRequest request = Get("fetch/api/resources/echo-content.py", "");
    request.method = "POST";
    request.body = "hello";
    const HandlerResponse response = wpt::RunHandler(request, stash);
    Expect(response.body == "hello", "echo-content.py returns the body it was posted");
    Expect(HeaderOf(response, "X-Request-Content-Type") == "NO",
           "and an absent Content-Type is reported as the literal `NO`");
    request.headers.emplace_back("Content-Type", "text/plain");
    Expect(HeaderOf(wpt::RunHandler(request, stash), "X-Request-Content-Type") == "text/plain",
           "and a present one as itself");
  });

  AddTest(tests, "WptHandlers/TheStashOutlivesOneRequestAndTakeRemoves", [] {
    // `request.server.stash` is what makes `preflight.py` able to tell a preflighted request that a
    // preflight happened -- two requests, one token. A take that did not remove would report a
    // preflight on the *second* request too, which is the assertion those tests make.
    wpt::Stash stash;
    HandlerRequest preflight = Get("fetch/api/resources/preflight.py", "token=abc");
    preflight.method = "OPTIONS";
    preflight.headers.emplace_back("Access-Control-Request-Method", "GET");
    preflight.headers.emplace_back("Accept", "*/*");
    const HandlerResponse first = wpt::RunHandler(preflight, stash);
    Expect(first.status == 200, "a well-formed preflight is a 200");

    const HandlerResponse second =
        wpt::RunHandler(Get("fetch/api/resources/preflight.py", "token=abc"), stash);
    Expect(HeaderOf(second, "x-did-preflight") == "1",
           "and the request after it is told a preflight happened, through the stash");

    const HandlerResponse third =
        wpt::RunHandler(Get("fetch/api/resources/preflight.py", "token=other"), stash);
    Expect(HeaderOf(third, "x-did-preflight") == "0",
           "while a different token knows nothing");
  });

  AddTest(tests, "WptHandlers/APreflightWithoutItsHeadersIsA400", [] {
    // Both refusals are in the file and both are load-bearing: a browser that sent a preflight
    // without `Access-Control-Request-Method`, or without `Accept: */*`, is wrong in a way the test
    // is written to catch, and a handler that answered 200 anyway would hide it.
    wpt::Stash stash;
    HandlerRequest preflight = Get("fetch/api/resources/preflight.py", "");
    preflight.method = "OPTIONS";
    Expect(wpt::RunHandler(preflight, stash).status == 400,
           "no Access-Control-Request-Method is a 400");
    preflight.headers.emplace_back("Access-Control-Request-Method", "GET");
    preflight.headers.emplace_back("Accept", "text/html");
    Expect(wpt::RunHandler(preflight, stash).status == 400,
           "and an Accept that is not `*/*` is a 400 as well");
  });

  AddTest(tests, "WptHandlers/PipesChangeAResponseTheServerAlreadyBuilt", [] {
    // `?pipe=` is the other half of reading a checkout without Python: hundreds of tests ask for a
    // status or a header on an ordinary *static file*. A server that ignored the query served the
    // file as itself, which is a wrong answer rather than a missing one.
    HandlerResponse response;
    response.status = 200;
    response.body = "0123456789";
    response.headers.emplace_back("Content-Type", "text/plain");

    wpt::ApplyPipes("status(404)", response);
    Expect(response.status == 404, "status() sets the status");

    wpt::ApplyPipes("header(X-One,a)|header(X-One,b,True)", response);
    Expect(HeaderOf(response, "X-One") == "a",
           "header() with a truthy third argument *appends*, so the first value survives -- which "
           "is how a test asks for two Set-Cookies");
    int count = 0;
    for (const auto& [name, value] : response.headers) {
      count += name == "X-One" ? 1 : 0;
    }
    Expect(count == 2, "and there are two of them");

    wpt::ApplyPipes("slice(2,5)", response);
    Expect(response.body == "234", "slice() takes a half-open range of the body");

    const int before = response.delay_ms;
    wpt::ApplyPipes("nosuchpipe(1)", response);
    Expect(response.delay_ms == before && response.body == "234",
           "and an unimplemented pipe changes nothing rather than guessing");
  });
}

}  // namespace microbrowser::tests
