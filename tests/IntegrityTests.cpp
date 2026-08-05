// Subresource Integrity: the parser, and the resource that does or does not run.
//
// ADR 0020 §4. The digests here were computed independently:
//
//   import hashlib, base64
//   base64.b64encode(hashlib.sha384(b"console.log('ran')").digest())
//
// which is the point of an integrity check being checkable at all: if these were
// produced by the code under test, the test would only assert that it agrees
// with itself.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "csp/SubresourceIntegrity.h"
#include "engine/Engine.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/DriveLoop.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

using csp::CheckIntegrity;
using csp::IntegrityResult;

// The script every fixture below serves, and its three digests.
constexpr std::string_view kScript = "console.log('ran')";
constexpr std::string_view kSha256 = "sha256-qwQmtFAWfhoZmFFtfI5A/ayUGESTJnPhDUYpggSf/dQ=";
constexpr std::string_view kSha384 =
    "sha384-fi53qe5gNX7LQ9hxnmsqVOddOtYbyq/XdX7rOqbDqX9JVK/Bx4h1GfHf9/lix5y2";
constexpr std::string_view kSha512 =
    "sha512-DSfb2CRVkLIvE2rLOUcCdvKBiBcOCG+izdXFBiIUXmUwIGg7xZOxCBgZ2nR1FYeZ"
    "SdNj0f/5JJgpEAS8jM4GdA==";

// The all-zero digest of each length: a hash that is well-formed and wrong. The
// *length* is the load-bearing part -- a token of the wrong length for its
// algorithm is dropped, and a dropped token is NoMetadata, which allows the
// resource. A "wrong hash" that is really a malformed one would make every test
// below pass for the wrong reason.
constexpr std::string_view kWrong256 = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
constexpr std::string_view kWrong384 =
    "sha384-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr std::string_view kWrong512 =
    "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAA==";

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  ScriptedFactory factory;

  // The document, then eight answers for whatever it asks for -- each one the
  // script above, so that "did it run" is a question about the integrity check
  // and not about which fixture answered.
  void Load(std::string_view html) {
    factory.script.push_back(ScriptedTransport::Exchange{
        "page.example", 443, true, OkResponse("text/html", html)});
    for (int i = 0; i < 8; ++i) {
      factory.script.push_back(ScriptedTransport::Exchange{
          "", 443, true, OkResponse("text/javascript", std::string(kScript))});
    }
    engine.PageLoader().SetTransport(factory);
    channel.Ui().Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    engine.HandlePendingMessages();
    channel.Ui().Send(ipc::NavigateMessage{"https://page.example/"});
    engine.HandlePendingMessages();
    RunEngineToIdle(engine);
  }

  bool Requested(std::string_view needle) const {
    for (const std::string& request : factory.log.requests) {
      if (request.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  std::string Console() const {
    std::string joined;
    for (const std::string& line : engine.ConsoleOutput()) {
      joined += joined.empty() ? "" : "|";
      joined += line;
    }
    return joined;
  }
};

}  // namespace

void RegisterIntegrityTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Integrity/MatchesEachOfTheThreeAlgorithms", [] {
    Expect(CheckIntegrity(kSha256, kScript) == IntegrityResult::Match, "sha256");
    Expect(CheckIntegrity(kSha384, kScript) == IntegrityResult::Match, "sha384");
    Expect(CheckIntegrity(kSha512, kScript) == IntegrityResult::Match, "sha512");
    Expect(CheckIntegrity(kSha384, "console.log('other')") == IntegrityResult::Mismatch,
           "and refuses different bytes");
  });

  AddTest(tests, "Integrity/AnAbsentOrUnreadableAttributeIsNotStricterThanNone", [] {
    Expect(CheckIntegrity("", kScript) == IntegrityResult::NoMetadata, "absent");
    Expect(CheckIntegrity("   ", kScript) == IntegrityResult::NoMetadata, "whitespace");
    // A typo in an attribute must not take a site offline, which is why an
    // unreadable token is dropped rather than failing the resource.
    Expect(CheckIntegrity("sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=", kScript) ==
               IntegrityResult::NoMetadata,
           "an algorithm this browser will not accept");
    Expect(CheckIntegrity("sha256-not!base64", kScript) == IntegrityResult::NoMetadata,
           "a digest that is not base64");
    Expect(CheckIntegrity("sha256-YWJj", kScript) == IntegrityResult::NoMetadata,
           "a digest of the wrong length for its algorithm");
    Expect(CheckIntegrity("nonsense", kScript) == IntegrityResult::NoMetadata,
           "a token with no algorithm at all");
  });

  AddTest(tests, "Integrity/TheStrongestAlgorithmNamedIsTheOneThatDecides", [] {
    // A correct sha512 beside a *wrong* sha256 passes: only the strongest set is
    // consulted, which is the specification's rule and the one that lets a page
    // list a legacy digest without weakening itself.
    Expect(CheckIntegrity(kWrong256, kScript) == IntegrityResult::Mismatch,
           "the all-zero digests are well-formed and wrong, not malformed");
    Expect(CheckIntegrity(kWrong384, kScript) == IntegrityResult::Mismatch, "sha384");
    Expect(CheckIntegrity(kWrong512, kScript) == IntegrityResult::Mismatch, "sha512");
    const std::string mixed = std::string(kWrong256) + " " + std::string(kSha512);
    Expect(CheckIntegrity(mixed, kScript) == IntegrityResult::Match,
           "the weaker digest is not consulted at all");
    // And the other way round: a wrong sha512 fails even beside a correct
    // sha256.
    const std::string reversed = std::string(kSha256) + " " + std::string(kWrong512);
    Expect(CheckIntegrity(reversed, kScript) == IntegrityResult::Mismatch,
           "a wrong strongest digest is not rescued by a weaker right one");
  });

  AddTest(tests, "Integrity/SeveralDigestsOfTheSameStrengthAreAlternatives", [] {
    const std::string either = std::string(kWrong384) + " " + std::string(kSha384);
    Expect(CheckIntegrity(either, kScript) == IntegrityResult::Match,
           "a page that ships two acceptable builds names both");
  });

  AddTest(tests, "Integrity/OptionsAfterTheDigestAreIgnoredRatherThanFatal", [] {
    Expect(CheckIntegrity(std::string(kSha384) + "?foo=bar", kScript) == IntegrityResult::Match,
           "the grammar allows options and gives them no meaning yet");
  });

  AddTest(tests, "Integrity/HasMetadataAnswersBeforeTheBytesExist", [] {
    Expect(csp::HasIntegrityMetadata(kSha384), "a usable hash");
    Expect(!csp::HasIntegrityMetadata("sha1-abc"), "and nothing usable");
    Expect(!csp::HasIntegrityMetadata(""), "and nothing at all");
  });

  AddTest(tests, "Integrity/AMatchingScriptRunsAndAMismatchingOneDoesNot", [] {
    Session good;
    good.Load("<html><body><script src=\"/app.js\" integrity=\"" + std::string(kSha384) +
              "\"></script></body></html>");
    ExpectEqString(good.Console(), "ran", "the bytes were the ones the page named");

    Session bad;
    bad.Load("<html><body><script src=\"/app.js\" integrity=\"" + std::string(kWrong384) +
             "\"></script></body></html>");
    Expect(bad.Requested("/app.js"), "the script was fetched");
    ExpectEqString(bad.Console(), "",
                   "and refused to execute -- which is the only thing SRI does, "
                   "and the reason it protects the user from the site's own CDN");
  });

  AddTest(tests, "Integrity/ACrossOriginResourceWithIntegrityNeedsCrossorigin", [] {
    // Without `crossorigin` the response would be opaque in a browser with the
    // process split, and here it would be an oracle: one guess per reload at
    // bytes the page is not allowed to read. So it is not fetched at all.
    Session bare;
    bare.Load("<html><body><script src=\"https://cdn.example/app.js\" integrity=\"" +
              std::string(kSha384) + "\"></script></body></html>");
    Expect(!bare.Requested("/app.js"), "no crossorigin, no request");

    Session declared;
    declared.Load("<html><body><script src=\"https://cdn.example/app.js\" crossorigin "
                  "integrity=\"" + std::string(kSha384) + "\"></script></body></html>");
    Expect(declared.Requested("/app.js"), "with crossorigin it is a CORS request");
    // The scripted response carries no `Access-Control-Allow-Origin`, so CORS
    // refuses it -- which is the correct outcome and is what makes the request
    // above not an oracle.
    ExpectEqString(declared.Console(), "",
                   "and a CORS response with no grant is not readable");
  });

  AddTest(tests, "Integrity/ASameOriginResourceNeedsNoCrossorigin", [] {
    Session session;
    session.Load("<html><body><script src=\"/app.js\" integrity=\"" + std::string(kSha384) +
                 "\"></script></body></html>");
    Expect(session.Requested("/app.js"), "same origin, so the rule does not apply");
    ExpectEqString(session.Console(), "ran", "and it ran");
  });
}

}  // namespace microbrowser::tests
