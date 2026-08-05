// `@font-face`, fetched and registered.
//
// ADR 0024. The parse is asserted in CssTests, the container in Woff2Tests; this is
// the half that touches the network and the font database, and the assertions worth
// reading are about what is *not* fetched: a source in a format nothing here can
// unwrap, everything after the first source that can be, and a second copy of a face
// that has already been asked for.

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

  AddTest(tests, "WebFont/AWoff2SourceIsFetchedAndTheChainStopsThere", [] {
    // **This assertion inverted when the WOFF2 container landed.** It used to be
    // "the woff2 was never requested", which was right while nothing could unwrap
    // one -- fetching a file to fail on it is a request that buys nothing. WOFF2 is
    // what the web actually ships, so it is now the *first* decodable source and
    // the rest of the author's chain is not fetched.
    //
    // What a WOFF2 contains -- a transformed `glyf`, an `hmtx` this browser will
    // not reconstruct -- is decided after the fetch, by the decoder. It is a fact
    // about the file rather than about its declared format, and the format hint
    // cannot say it.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom;"
        " src: url(/custom.woff2) format(\"woff2\"), url(/custom.ttf) format(\"truetype\") }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/custom.woff2")), 1,
                "the woff2 is requested");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/custom.ttf")), 0,
                "and the chain stops at the first decodable entry");
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
    // `woff` -- the older zlib container -- is the example now that `woff2` is
    // decodable. Nothing here unwraps it, so it is refused before the network for
    // the reason woff2 used to be.
    Session session;
    session.Load(
        "<html><head><style>"
        "@font-face { font-family: Custom; src: url(/only.woff) format(\"woff\") }"
        "</style></head><body><p>text</p></body></html>");
    ExpectEqInt(static_cast<long long>(session.RequestsFor("/only.woff")), 0,
                "nothing to fetch, so nothing is fetched");
    // And the page still renders: it falls through to the next family in the
    // stack, which is what a font stack is for.
    Expect(!session.engine.IsLoading(), "and the load finished rather than waiting");
  });
}

}  // namespace microbrowser::tests
