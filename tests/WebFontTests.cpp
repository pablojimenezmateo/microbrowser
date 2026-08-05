// `@font-face`, fetched and registered.
//
// ADR 0024. The parse is asserted in CssTests; this is the half that touches the
// network and the font database, and the assertions worth reading are about what
// is *not* fetched: a `format("woff2")` source, because brotli does not exist here
// yet, and a second copy of a face that has already been asked for.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

// A response carrying a real font file, so registration succeeds or fails for the
// reason under test rather than because the bytes were nonsense.
std::string FontResponse() {
  const std::vector<std::byte> bytes = BuildSyntheticFont();
  std::string body;
  body.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    body.push_back(static_cast<char>(byte));
  }
  return "HTTP/1.1 200 OK\r\nContent-Type: font/ttf\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  void Load(std::string_view html) {
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", html)});
    for (int i = 0; i < 4; ++i) {
      factory.script.push_back(ScriptedTransport::Exchange{"", 443, true, FontResponse()});
    }
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
  }

  std::size_t RequestsFor(std::string_view needle) const {
    std::size_t count = 0;
    for (const std::string& request : factory.log.requests) {
      count += request.find(needle) != std::string::npos ? 1u : 0u;
    }
    return count;
  }
};

}  // namespace

void RegisterWebFontTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WebFont/ADeclaredFaceIsFetchedAndRegistered", [] {
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom; src: url(/custom.ttf) format(\"truetype\") }"
        "p { font-family: Custom }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/custom.ttf")), 1, "fetched once");
    // Registered under the *descriptors*, which is what a `font-family` stack
    // names -- not under whatever the file calls itself.
    gfx::FontRequest request;
    request.families = {"Custom"};
    request.weight = 400;
    Expect(session.fonts.catalog.FontFor(request) != nullptr,
           "and the family a page names now resolves");
  });

  AddTest(tests, "WebFont/AWoff2SourceIsNotFetchedAtAll", [] {
    // The one that matters: WOFF2 is brotli inside a container and neither exists
    // here until ADR 0024's session 20. Fetching one to fail on it is a request
    // that buys nothing, so the format hint is used to skip it *before* the
    // network -- which is the only reason authors write the hint.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom;"
        " src: url(/custom.woff2) format(\"woff2\"), url(/custom.ttf) format(\"truetype\") }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/custom.woff2")), 0,
                "the woff2 was never requested");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/custom.ttf")), 1,
                "and the next source in the author's chain was");
  });

  AddTest(tests, "WebFont/OnlyTheFirstDecodableSourceIsFetched", [] {
    // The author's order is a fallback chain, not a list of things to download.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom;"
        " src: url(/first.ttf) format(\"truetype\"), url(/second.ttf) format(\"truetype\") }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/first.ttf")), 1, "the first");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/second.ttf")), 0,
                "and not the rest of the chain");
  });

  AddTest(tests, "WebFont/AFaceIsFetchedOnceEvenAsSheetsKeepArriving", [] {
    // The face list is rebuilt from the sheets every time one lands, so without a
    // record of what has been asked for, every stylesheet would re-request every
    // font on the page.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: A; src: url(/a.ttf) }"
        "</style><style>"
        "@font-face { font-family: B; src: url(/b.ttf) }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/a.ttf")), 1, "a once");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/b.ttf")), 1, "b once");
  });

  AddTest(tests, "WebFont/TwoWeightsOfOneFamilyAreTwoFaces", [] {
    // Same family, different weight: two registrations, because nearest-weight
    // matching is the whole reason a family has more than one file.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom; src: url(/regular.ttf); font-weight: 400 }"
        "@font-face { font-family: Custom; src: url(/bold.ttf); font-weight: 700 }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/regular.ttf")), 1, "regular");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/bold.ttf")), 1, "and bold");
    gfx::FontRequest bold;
    bold.families = {"Custom"};
    bold.weight = 700;
    Expect(session.fonts.catalog.FontFor(bold) != nullptr, "and bold resolves");
  });

  AddTest(tests, "WebFont/AFaceWithNoDecodableSourceIsNotFetched", [] {
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom; src: url(/only.woff2) format(\"woff2\") }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/only.woff2")), 0,
                "nothing to fetch, so nothing is fetched");
    // And the page still renders: it falls through to the next family in the
    // stack, which is what a font stack is for.
    Expect(!session.engine.IsLoading(), "and the load finished rather than waiting");
  });
}

}  // namespace microbrowser::tests
