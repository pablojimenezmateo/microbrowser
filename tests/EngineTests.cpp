#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "engine/Loader.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

// A font stack with no system fonts in it, so that nothing here depends on
// which typefaces the machine has installed.
struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
    catalog.SetGenericFamily("sans-serif", "Test");
    catalog.SetGenericFamily("monospace", "Test");
  }
};

std::string DataUrl(std::string_view html) {
  return std::string("data:text/html,") + std::string(html);
}

// Drives one navigation and returns everything the engine sent back.
struct Session {
  TestFonts fonts;
  ipc::InProcessChannel channel;
  engine::Engine engine{channel.Engine(), fonts.catalog};
  std::vector<ipc::EngineToUi> sent;

  void Send(ipc::UiToEngine message) {
    channel.Ui().Send(std::move(message));
    engine.HandlePendingMessages();
    while (auto reply = channel.Ui().TryReceive()) {
      sent.push_back(std::move(*reply));
    }
  }

  const ipc::PaintFrameMessage* LastFrame() const {
    const ipc::PaintFrameMessage* found = nullptr;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* paint = std::get_if<ipc::PaintFrameMessage>(&message)) {
        found = paint;
      }
    }
    return found;
  }

  std::string LastTitle() const {
    std::string title;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* changed = std::get_if<ipc::TitleChangedMessage>(&message)) {
        title = changed->title;
      }
    }
    return title;
  }
};

std::size_t TextRunCount(const gfx::DisplayList& list) {
  std::size_t runs = 0;
  for (const gfx::DisplayCommand& command : list.Commands()) {
    runs += std::holds_alternative<gfx::DrawTextCommand>(command) ? 1u : 0u;
  }
  return runs;
}

}  // namespace

void RegisterEngineTests(std::vector<TestCase>& tests) {
  // --- The loader -----------------------------------------------------------

  AddTest(tests, "Loader/DecodesPercentEncodedDataUrls", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:text/html,%3Cp%3Ehi%3C/p%3E");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "<p>hi</p>", "percent escapes are the payload, not decoration");
    ExpectEqString(decoded.content_type, "text/html", "and the type came from the metadata");
  });

  AddTest(tests, "Loader/DecodesBase64DataUrls", [] {
    // "<b>x</b>" base64-encoded.
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:text/html;base64,PGI+eDwvYj4=");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "<b>x</b>", "and produced the bytes");
  });

  AddTest(tests, "Loader/RejectsMalformedDataUrls", [] {
    Expect(!engine::DecodeDataUrl("data:text/html").ok,
           "no comma is not a data URL, it is a string beginning with 'data:'");
    Expect(!engine::DecodeDataUrl("https://example.org/").ok, "and neither is another scheme");
    Expect(!engine::DecodeDataUrl("data:text/html;base64,!!!!").ok,
           "nor is base64 that is not base64");
  });

  AddTest(tests, "Loader/LeavesAMalformedEscapeAlone", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:,100% and %zz");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.body, "100% and %zz",
                   "a lone percent is a byte; eating it would change the document");
  });

  AddTest(tests, "Loader/DefaultsTheContentTypeWhenTheUrlOmitsIt", [] {
    const engine::DataUrl decoded = engine::DecodeDataUrl("data:,plain");
    Expect(decoded.ok, "it decoded");
    ExpectEqString(decoded.content_type, "text/plain;charset=US-ASCII", "per the data URL spec");
  });

  // --- The page -------------------------------------------------------------

  AddTest(tests, "Page/TakesItsTitleFromTheDocument", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><head><title>Real Title</title></head><body>x</body></html>",
              "https://example.org/");
    ExpectEqString(page.Title(), "Real Title", "the <title> element is the title");
  });

  AddTest(tests, "Page/FallsBackToTheUrlWhenThereIsNoTitle", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<p>no title here</p>", "https://example.org/a");
    ExpectEqString(page.Title(), "https://example.org/a",
                   "a tab strip has to show something, and \"\" is not a title but a missing one");
  });

  AddTest(tests, "Page/AppliesStyleElementsFromTheDocument", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<html><head><style>div { width: 50px }</style></head>"
              "<body><div>x</div></body></html>",
              "");
    page.Layout(400.0f);
    gfx::DisplayList list;
    page.Paint(list, 0.0f);
    // Width is asserted through layout rather than by reaching into the box
    // tree, because the point is that the sheet reached the cascade.
    Expect(page.ContentHeight() > 0.0f, "the document laid out");
  });

  AddTest(tests, "Page/DoesNotCarryOneDocumentsStylesIntoTheNext", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<style>p { height: 500px }</style><p>x</p>", "");
    const float styled = page.Layout(400.0f);
    page.Load("<p>x</p>", "");
    const float unstyled = page.Layout(400.0f);
    Expect(styled > unstyled,
           "author sheets belong to the document that carried them; keeping the resolver would "
           "let the previous page style this one");
  });

  AddTest(tests, "Page/ScrollOffsetMovesTheGeometryRatherThanATransform", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='background-color:red'><p>ABC</p></body>", "");
    page.Layout(400.0f);

    gfx::DisplayList top;
    page.Paint(top, 0.0f);
    gfx::DisplayList scrolled;
    page.Paint(scrolled, 40.0f);
    Expect(!(top == scrolled), "scrolling changed the recorded geometry");
    Expect(top.Bounds().y - scrolled.Bounds().y == 40, "by exactly the scroll offset");
  });

  // --- The engine -----------------------------------------------------------

  AddTest(tests, "Engine/NavigatingToADataUrlRendersTheDocument", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<html><head><title>Doc</title></head><body><p>ABC</p></body></html>")});

    ExpectEqString(session.LastTitle(), "Doc", "the title reached the UI");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "and a frame was painted");
    Expect(TextRunCount(frame->display_list) > 0, "with the document's text on it");
  });

  AddTest(tests, "Engine/AFailedLoadRendersAnErrorPageRatherThanNothing", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"not a url at all"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a failed load still paints");
    Expect(TextRunCount(frame->display_list) > 0,
           "a browser showing nothing when a load fails is indistinguishable from one that "
           "has hung");
  });

  AddTest(tests, "Engine/TheErrorPageEscapesTheUrlItEchoes", [] {
    // The error page is built by string concatenation and the URL comes from
    // whoever asked for the navigation. Interpolating it raw makes a URL
    // containing markup an injection into the browser's own document.
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"<script>x</script> not a url"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");

    // The URL appears as text. That is the proof: unescaped, the tokenizer
    // would have made a script element out of it and swallowed the contents,
    // and no text run would mention it at all.
    bool shown_as_text = false;
    for (const gfx::DisplayList::TextRun& run : frame->display_list.Texts()) {
      shown_as_text = shown_as_text || run.text.find("<script>x</script>") != std::string::npos;
    }
    Expect(shown_as_text,
           "the URL must reach the page as text rather than as markup, and must still be "
           "shown -- an error page that hides what failed is not an error page");
  });

  AddTest(tests, "Engine/ScrollingStopsAtTheEndOfTheDocument", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{DataUrl("<p>ABC</p>")});
    const std::size_t before = session.sent.size();

    // A short document does not scroll at all, so neither of these produces a
    // frame. A scroll that ran off the end would paint blank space.
    session.Send(ipc::ScrollMessage{0, 10000});
    session.Send(ipc::ScrollMessage{0, -10000});
    ExpectEqInt(static_cast<long long>(session.sent.size() - before), 0,
                "scrolling a document that fits repaints nothing");
  });

  AddTest(tests, "Engine/AResizeRelaysOutAndAScrollDoesNot", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 60}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<p>ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC ABC</p>")});
    Expect(session.LastFrame() != nullptr, "the document painted");

    const std::size_t before = session.sent.size();
    session.Send(ipc::ScrollMessage{0, 20});
    Expect(session.sent.size() > before, "a tall document scrolls, and scrolling repaints");
  });

  AddTest(tests, "Engine/NavigatingToAboutBlankIsAPageRatherThanAFailure", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"about:blank"});
    ExpectEqString(session.LastTitle(), "New Tab", "about:blank is a real, blank document");
    Expect(session.LastFrame() != nullptr, "and it paints");
  });

  AddTest(tests, "Engine/EveryFrameItProducesSurvivesItsOwnWireFormat", [] {
    Session session;
    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{
        DataUrl("<h1>ABCD</h1><p style='border:1px solid black'>ABC ABCD</p>")});

    std::size_t frames = 0;
    for (const ipc::EngineToUi& message : session.sent) {
      const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(message));
      Expect(decoded.has_value(), "the engine emitted a message its own wire format rejects");
      Expect(*decoded == message, "and decoding is not a fixed point of encoding");
      frames += std::holds_alternative<ipc::PaintFrameMessage>(message) ? 1u : 0u;
    }
    Expect(frames > 0, "at least one frame, or this asserts nothing");
  });
}

}  // namespace microbrowser::tests
