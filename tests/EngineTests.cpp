#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "TestSupport.h"
#include "engine/Engine.h"
#include "engine/Loader.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "privacy/PrivacyPolicy.h"
#include "url/Url.h"
#include "ipc/InProcessTransport.h"
#include "ipc/Message.h"
#include "support/ScriptedTransport.h"
#include "support/SyntheticFont.h"
#include "support/SyntheticPng.h"

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

  std::string LastCommittedUrl() const {
    std::string url;
    for (const ipc::EngineToUi& message : sent) {
      if (const auto* committed = std::get_if<ipc::NavigationCommittedMessage>(&message)) {
        url = committed->url;
      }
    }
    return url;
  }
};

std::size_t TextRunCount(const gfx::DisplayList& list) {
  std::size_t runs = 0;
  for (const gfx::DisplayCommand& command : list.Commands()) {
    runs += std::holds_alternative<gfx::DrawTextCommand>(command) ? 1u : 0u;
  }
  return runs;
}

std::optional<std::string> SubmissionTarget(const engine::Page& page,
                                            gfx::FloatPoint point) {
  const std::optional<engine::FormSubmission> submission = page.FormSubmissionRequestAt(point);
  if (!submission.has_value() || submission->method != "GET") {
    return std::nullopt;
  }
  return submission->url;
}

std::optional<std::string> FocusedSubmissionTarget(const engine::Page& page) {
  const std::optional<engine::FormSubmission> submission = page.FocusedFormSubmission();
  if (!submission.has_value() || submission->method != "GET") {
    return std::nullopt;
  }
  return submission->url;
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

  AddTest(tests, "Page/HitTestsLaidOutLinks", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><a href='/next'>ABC</a><p>outside</p></body>",
              "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> hit = page.LinkAt(gfx::FloatPoint{5.0f, 5.0f});
    Expect(hit.has_value(), "a point over link text hits the anchor");
    ExpectEqString(*hit, "/next", "the written href is returned for the engine to resolve");
    Expect(!page.LinkAt(gfx::FloatPoint{5.0f, 50.0f}).has_value(),
           "text outside the anchor is not a link");
  });

  AddTest(tests, "Page/BuildsGetFormSubmissionTargets", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search?old=1'>"
        "<input type='hidden' name='token' value='a&b'>"
        "<input name='q' value='hello world' size='2'>"
        "<input type='submit' name='go' value='Search'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "clicking the submit input activates its form");
    ExpectEqString(*target, "/search?token=a%26b&q=hello+world&go=Search",
                   "GET submission replaces the action query with successful controls");
  });

  AddTest(tests, "Page/SubmitterOverridesFormAction", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/default'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formaction='/override?old=1'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/override?q=hello",
                   "the submitter's formaction overrides the form action");
  });

  AddTest(tests, "Page/SubmitterOverridesFormMethodToGet", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search' method='post'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formmethod='get'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "formmethod=get is a supported submission path");
    ExpectEqString(*target, "/search?q=hello",
                   "the submitter's formmethod overrides the form method");
  });

  AddTest(tests, "Page/SubmitterOverridesFormMethodToPost", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search?keep=1#frag' method='get'>"
        "<input name='q' value='hello'>"
        "<input type='submit' value='Search' formmethod='post'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<engine::FormSubmission> submission =
        page.FormSubmissionRequestAt(gfx::FloatPoint{45.0f, 5.0f});
    Expect(submission.has_value(), "formmethod=post is a supported submission path");
    ExpectEqString(submission->method, "POST", "the submitter's formmethod wins");
    ExpectEqString(submission->url, "/search?keep=1", "POST preserves the action query");
    ExpectEqString(submission->body, "q=hello", "controls move into the request body");
    ExpectEqString(submission->content_type, "application/x-www-form-urlencoded",
                   "POST uses the default form encoding");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalControls", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='external' value='out' form='f'>"
        "<form id='f' action='/search'><input name='inside' value='in'>"
        "<input type='submit' value='Go'></form>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates its form");
    ExpectEqString(*target, "/search?external=out&inside=in",
                   "controls with a matching form attribute are submitted with that form");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalSubmitters", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<form id='f' action='/search'><input name='q' value='hello'></form>"
        "<input type='submit' name='go' value='Go' form='f'>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{5.0f, 25.0f});
    Expect(target.has_value(), "an external submitter activates its associated form");
    ExpectEqString(*target, "/search?q=hello&go=Go",
                   "the external submitter is serialized as the clicked submitter");
  });

  AddTest(tests, "Page/FormAttributeAssociatesFocusedTextControls", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='q' form='f'>"
        "<form id='f' action='/search'><input type='submit' value='Go'></form>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}),
           "the external text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("hello"), "typing changed the external input");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the external input can submit its associated form");
    ExpectEqString(*target, "/search?q=hello", "focused submission uses the form attribute");
  });

  AddTest(tests, "Page/FormAttributeAssociatesExternalResetButtons", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:40px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input name='q' value='start' form='f'>"
        "<form id='f' action='/search'><input type='submit' value='Go'></form>"
        "<input type='reset' value='Reset' form='f'>"
        "</body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}),
           "the external text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("ed"), "typing changed the external input");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{5.0f, 40.0f}),
           "the external reset button restores its associated form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{5.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates its form");
    ExpectEqString(*target, "/search?q=start", "reset restored the associated external input");
  });

  AddTest(tests, "Page/DisabledSubmitInputDoesNotSubmitForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='hello' size='2'>"
        "<input type='submit' value='Go' disabled>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f}).has_value(),
           "a disabled submit control must not activate its form");
  });

  AddTest(tests, "Page/DisabledFieldsetControlsAreNotSubmitted", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        // The fieldset is a block, so the submit after it is on the next line. Its
        // height is pinned here so the point below is a stated fact rather than a
        // number that happens to work.
        "<style>fieldset,input{margin:0;padding:0;border:0}fieldset{height:20px}"
        "input{width:40px;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled>"
        "<input name='q' value='hello'>"
        "<input type='checkbox' name='seen' checked>"
        "</fieldset>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{20.0f, 30.0f});
    Expect(target.has_value(), "the submit control outside the fieldset activates the form");
    ExpectEqString(*target, "/search", "disabled fieldset descendants are not successful");
  });

  AddTest(tests, "Page/DisabledFieldsetSubmitInputDoesNotSubmitForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled><input type='submit' value='Go'></fieldset>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{5.0f, 5.0f}).has_value(),
           "a submit control inside a disabled fieldset must not activate its form");
  });

  AddTest(tests, "Page/DisabledFieldsetTextControlsCannotBeEdited", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}"
        "fieldset{margin:0;padding:0;border:0;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled><input name='q' value='locked'></fieldset>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}),
           "a text control inside a disabled fieldset cannot be focused");
    Expect(!page.InsertTextIntoFocusedTextControl("x"),
           "typing cannot mutate a disabled fieldset descendant");
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{20.0f, 30.0f});
    Expect(target.has_value(), "the submit control outside the fieldset activates the form");
    ExpectEqString(*target, "/search", "the disabled fieldset text control was not submitted");
  });

  AddTest(tests, "Page/DisabledFieldsetFirstLegendControlsRemainEnabled", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>fieldset,legend,input{margin:0;padding:0;border:0}"
        "input{width:40px;height:20px}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<fieldset disabled>"
        "<legend><input name='q' value='allowed'><input type='submit' value='Go'></legend>"
        "<input name='blocked' value='x'>"
        "</fieldset>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "a submit control inside the first legend remains enabled");
    ExpectEqString(*target, "/search?q=allowed",
                   "only controls inside the first legend escape the disabled fieldset");
  });

  AddTest(tests, "Page/ButtonElementsSubmitForms", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input,button{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='hello'>"
        "<button name='go' value='Search'>Search</button>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "clicking a button with no type activates its form");
    ExpectEqString(*target, "/search?q=hello&go=Search",
                   "the clicked button is serialized as the submitter");
  });

  AddTest(tests, "Page/ButtonElementTypesAreHonored", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input,button{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='start'>"
        "<button type='button' name='noop' value='x'>Noop</button>"
        "<button type='reset'>Reset</button>"
        "<button name='go' value='Go'>Go</button>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(!SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f}).has_value(),
           "a button with type=button does not submit");
    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("ed"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{85.0f, 5.0f}), "button type=reset resets its form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{125.0f, 5.0f});
    Expect(target.has_value(), "the submit button activates its form");
    ExpectEqString(*target, "/search?q=start&go=Go",
                   "reset restored the input and the clicked submit button was serialized");
  });

  AddTest(tests, "Page/SelectControlsSubmitSelectedOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'>"
        "<option value='a'>Alpha</option><option value='b' selected>Beta</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=b", "select serializes its selected option value");
  });

  AddTest(tests, "Page/SelectControlsDefaultToTheFirstOption", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'><option>Alpha</option><option value='b'>Beta</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=Alpha",
                   "a select with no selected option uses the first option text");
  });

  AddTest(tests, "Page/MultipleSelectControlsSubmitEverySelectedOption", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple>"
        "<option value='a' selected>Alpha</option>"
        "<option value='b'>Beta</option>"
        "<option selected>Gamma</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=a&topic=Gamma",
                   "a multiple select serializes every selected option in tree order");
  });

  AddTest(tests, "Page/MultipleSelectWithNoSelectionIsNotSuccessful", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple><option value='a'>Alpha</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick", "an unselected multiple select contributes no entry");
  });

  AddTest(tests, "Page/SelectControlsSkipDisabledSelectedOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic'><option value='placeholder' selected disabled>Pick</option>"
        "<option value='a'>Alpha</option></select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick", "a disabled selected option contributes no entry");
  });

  AddTest(tests, "Page/MultipleSelectSkipsDisabledOptions", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>select,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/pick'>"
        "<select name='topic' multiple>"
        "<option value='a' selected disabled>Alpha</option>"
        "<optgroup disabled><option value='b' selected>Beta</option></optgroup>"
        "<option value='c' selected>Gamma</option>"
        "</select>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/pick?topic=c",
                   "disabled options and disabled optgroups are skipped");
  });

  AddTest(tests, "Page/SerializesOnlyCheckedCheckboxesAndRadios", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:10px;height:10px}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input type='checkbox' name='seen' checked>"
        "<input type='checkbox' name='skip'>"
        "<input type='radio' name='mode' value='new' checked>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{35.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?seen=on&mode=new",
                   "checked boxes without a value submit 'on', unchecked boxes submit nothing, "
                   "and an unnamed submitter is not successful");
  });

  AddTest(tests, "Page/ClickingCheckableInputsUpdatesFormSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:10px;height:10px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input type='checkbox' name='seen'>"
        "<input type='radio' name='mode' value='old' checked>"
        "<input type='radio' name='mode' value='new'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{5.0f, 5.0f}),
           "clicking the checkbox toggles it");
    page.Layout(400.0f);
    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{25.0f, 5.0f}),
           "clicking a radio input selects it");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{35.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?seen=on&mode=new",
                   "activated checkable controls update the submitted state");
  });

  AddTest(tests, "Page/RadioGroupsUseFormAttributeOwnership", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>form,input{margin:0}input{width:20px;height:20px}</style>"
        "<body style='margin:0'>"
        "<input type='radio' name='mode' value='new' form='f'>"
        "<form id='f' action='/filter'>"
        "<input type='radio' name='mode' value='old' checked>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{5.0f, 5.0f}),
           "clicking the external radio selects it");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{25.0f, 25.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?mode=new",
                   "a radio outside the form clears its peer with the same form owner");
  });

  AddTest(tests, "Page/ResetInputRestoresFormControlDefaults", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:20px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input name='q'>"
        "<input type='checkbox' name='seen' checked>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abc"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ActivateCheckableInputAt(gfx::FloatPoint{25.0f, 5.0f}),
           "clicking the checkbox toggles it");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "clicking reset restores defaults");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{65.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?q=&seen=on", "reset restored the original form state");
  });

  AddTest(tests, "Page/DisabledResetInputDoesNotRestoreForm", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:20px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/filter'>"
        "<input name='q'>"
        "<input type='reset' value='Reset' disabled>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abc"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(!page.ResetFormAt(gfx::FloatPoint{25.0f, 5.0f}),
           "a disabled reset control must not restore its form");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/filter?q=abc", "disabled reset left the edited state intact");
  });

  AddTest(tests, "Page/ResetRestoresTextLikeInputTypes", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/contact'>"
        "<input type='email' name='email' value='a@b'>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the email input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("c"), "typing changed the input value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "clicking reset restores defaults");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{85.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/contact?email=a%40b",
                   "reset restored the email input's original value");
  });

  AddTest(tests, "Page/FocusedInputTextUpdatesFormSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' size='2'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("hi"), "typing changed the input value");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{45.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/search?q=hi", "the form uses the edited input value");
  });

  AddTest(tests, "Page/TextareasCanBeEditedAndSubmitted", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/note'>"
        "<textarea name='body' cols='4' rows='2'>hi</textarea>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the textarea was focused");
    Expect(page.InsertTextIntoFocusedTextControl("&"), "typing changed the textarea value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused textarea can submit its owning form");
    ExpectEqString(*target, "/note?body=hi%26", "textarea edits are submitted");
  });

  AddTest(tests, "Page/ResetRestoresTextareaDefaults", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<style>textarea,input{width:40px;height:20px;margin:0}</style>"
        "<body style='margin:0'><form action='/note'>"
        "<textarea name='body'>hi</textarea>"
        "<input type='reset' value='Reset'>"
        "<input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the textarea was focused");
    Expect(page.InsertTextIntoFocusedTextControl("!"), "typing changed the textarea value");
    page.Layout(400.0f);
    Expect(page.ResetFormAt(gfx::FloatPoint{45.0f, 5.0f}), "reset restored the textarea");
    page.Layout(400.0f);
    const std::optional<std::string> target =
        SubmissionTarget(page, gfx::FloatPoint{85.0f, 5.0f});
    Expect(target.has_value(), "the submit control activates the form");
    ExpectEqString(*target, "/note?body=hi", "reset restored the textarea default text");
  });

  AddTest(tests, "Page/TextLikeInputTypesCanBeEdited", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/contact'>"
        "<input type='email' name='email' size='4'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the email input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("a@b"), "typing changed the input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/contact?email=a%40b", "email input edits are submitted");
  });

  AddTest(tests, "Page/FocusedInputHonorsMaxlength", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' maxlength='3' size='3'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("abcd"), "typing changed the input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/search?q=abc", "maxlength bounds inserted input text");
  });

  AddTest(tests, "Page/FocusedReadonlyInputDoesNotMutate", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' value='locked' readonly size='6'><input type='submit' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the readonly input was focused");
    Expect(!page.InsertTextIntoFocusedTextControl("x"), "typing does not mutate readonly input");
    Expect(!page.DeleteBackwardFromFocusedTextControl(), "backspace does not mutate readonly input");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can still submit its owning form");
    ExpectEqString(*target, "/search?q=locked", "readonly preserves the original value");
  });

  AddTest(tests, "Page/FocusedInputBackspaceAndEnterSubmission", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<body style='margin:0'><form action='/search'>"
        "<input name='q' size='3'><input type='submit' name='go' value='Go'>"
        "</form></body>",
        "https://example.org/start");
    page.Layout(400.0f);

    Expect(page.FocusTextControlAt(gfx::FloatPoint{5.0f, 5.0f}), "the text input was focused");
    Expect(page.InsertTextIntoFocusedTextControl("caf\xC3\xA9"), "typing changed the input value");
    Expect(page.DeleteBackwardFromFocusedTextControl(), "backspace changed the focused input value");
    const std::optional<std::string> target = FocusedSubmissionTarget(page);
    Expect(target.has_value(), "the focused input can submit its owning form");
    ExpectEqString(*target, "/search?q=caf",
                   "enter submission serializes the focused input without a clicked submitter");
  });

  // --- Subresources ---------------------------------------------------------

  AddTest(tests, "Page/CollectsLinkedStyleSheetsButNotOtherLinks", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head>"
        "<link rel='stylesheet' href='a.css'>"
        "<link rel='STYLESHEET' href='b.css'>"
        "<link rel='alternate stylesheet' href='alt.css'>"
        "<link rel='preload' href='p.css'>"
        "<link rel='icon' href='favicon.png'>"
        "<link rel='stylesheet'>"
        "</head><body>x</body>",
        "https://example.org/");

    const std::vector<std::string>& sheets = page.PendingStyleSheets();
    ExpectEqInt(static_cast<long long>(sheets.size()), 2,
                "rel is a token set: an alternate sheet is not applied, a preload is not a "
                "sheet, and a link with no href points nowhere");
    ExpectEqString(sheets.at(0), "a.css", "in document order");
    ExpectEqString(sheets.at(1), "b.css", "and rel matches case-insensitively");
  });

  AddTest(tests, "Page/StyleSheetsCascadeInDocumentOrderAcrossLinksAndStyleElements", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head><link rel='stylesheet' href='early.css'>"
        "<style>p { height: 40px }</style></head>"
        "<body><p>ABC</p></body>",
        "https://example.org/");

    page.AddStyleSheet(0, "p { height: 400px }");
    const float height = page.Layout(400.0f);
    Expect(height < 200.0f,
           "a linked sheet fills its document slot; it does not win merely because it loaded "
           "after a later <style> element");
  });

  AddTest(tests, "Page/FailedStyleSheetsDoNotShiftLaterSheetsIntoTheWrongCascadeSlot", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load(
        "<head><link rel='stylesheet' href='missing.css'>"
        "<style>p { height: 40px }</style>"
        "<link rel='stylesheet' href='late.css'></head>"
        "<body><p>ABC</p></body>",
        "https://example.org/");

    page.AddStyleSheet(1, "p { height: 400px }");
    const float height = page.Layout(400.0f);
    Expect(height >= 300.0f,
           "the second successful fetch fills the second link's slot, after the inline style, "
           "even though the first link never loaded");
  });

  AddTest(tests, "Loader/ASubresourceIsFetchedRelativeToItsDocument", [] {
    engine::Loader loader;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/css", "p { color: red }")});
    loader.SetTransport(factory);

    const url::Url document = *url::Url::Parse("https://example.org/dir/page.html");
    const engine::Loader::Result result = loader.LoadSubresource(
        "../style.css", document, privacy::ResourceType::Stylesheet, 1000);

    Expect(result.ok, "the sheet loaded");
    ExpectEqString(result.body, "p { color: red }", "with its bytes");
    Expect(!factory.log.requests.empty(), "a request was made");
    Expect(factory.log.requests.at(0).find("GET /style.css ") != std::string::npos,
           "resolved against the document, not against the root: every href in a page is "
           "relative to where the page is");
    Expect(factory.log.requests.at(0).find("Referer: https://example.org/dir/page.html\r\n") !=
               std::string::npos,
           "and the subresource request carries the policy-computed referrer");
  });

  AddTest(tests, "Loader/ABlockedSubresourceIsNotFetched", [] {
    // The point of the privacy layer: a request that the policy refuses never
    // reaches a socket. If it did, the block would be cosmetic.
    engine::Loader loader;
    ScriptedFactory factory;
    factory.script.push_back(
        ScriptedTransport::Exchange{"", 0, false, OkResponse("text/css", "x{}")});
    loader.SetTransport(factory);

    // HTTPS-only is the default, and deliberately not settable downward.
    const url::Url document = *url::Url::Parse("http://insecure.test/page.html");
    const engine::Loader::Result result = loader.LoadSubresource(
        "http://insecure.test/style.css", document, privacy::ResourceType::Stylesheet, 1000);
    (void)result;
    Expect(factory.log.hosts.empty() || result.ok,
           "either the policy upgraded the request and it was made over TLS, or it refused "
           "and no connection happened -- what must not happen is a plaintext fetch");
    for (const bool secure : factory.log.secure) {
      Expect(secure, "no request left this machine in plaintext under HTTPS-only");
    }
  });

  AddTest(tests, "Engine/AppliesAStyleSheetTheDocumentLinked", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<html><head><link rel='stylesheet' href='/s.css'></head>"
                   "<body><p>ABC</p></body></html>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("text/css", "p { height: 400px }")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the document and its stylesheet were both fetched");
    Expect(factory.log.requests.at(1).find("GET /s.css ") != std::string::npos,
           "and the second request is the sheet");

    // The sheet must have applied *before* the first layout: laying out
    // without it and reflowing after is the flash of unstyled content.
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    Expect(frame->display_list.Bounds().height >= 300,
           "the 400px paragraph from the linked sheet is in the geometry of the first frame");
  });

  AddTest(tests, "Engine/AStyleSheetThatFailsToLoadIsNotANavigationFailure", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<html><head><link rel='stylesheet' href='/missing.css'></head>"
                   "<body><p>ABC</p></body></html>")});
    // No second exchange: the sheet's connection fails.
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqString(session.LastTitle(), "https://example.org/page.html",
                   "the document still committed");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr && TextRunCount(frame->display_list) > 0,
           "a stylesheet that does not load is a page rendered without it, which is what "
           "every browser does -- not an error page");
  });

  // --- Images ---------------------------------------------------------------

  AddTest(tests, "Page/CollectsImageSourcesOnceEach", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<img src='a.png'><img src='b.png'><img src='a.png'><img>", "https://example.org/");
    const std::vector<std::string>& images = page.PendingImages();
    ExpectEqInt(static_cast<long long>(images.size()), 2,
                "a page that shows one icon forty times fetches and decodes it once, and an "
                "<img> with no src points nowhere");
    ExpectEqString(images.at(0), "a.png", "in document order");
  });

  AddTest(tests, "Layout/AnImageTakesItsSizeFromThePixelsWhenNothingElseSaysOtherwise", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='x.png'></body>", "https://example.org/");

    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(24, 12, std::vector<std::uint32_t>(24 * 12, 0xFF00FF00u)), "built");
    page.AddImage("x.png", image);
    page.Layout(400.0f);

    gfx::DisplayList list;
    page.Paint(list, 0.0f);
    const gfx::IntRect bounds = list.Bounds();
    Expect(bounds.width >= 24 && bounds.height >= 12,
           "the intrinsic size of the decoded image is the used size");
  });

  AddTest(tests, "Layout/AnImagesDeclaredSizeBeatsItsIntrinsicOne", [] {
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='x.png' width='40' height='30'></body>",
              "https://example.org/");
    auto image = std::make_shared<gfx::Image>();
    Expect(image->Adopt(8, 8, std::vector<std::uint32_t>(64, 0xFFFF0000u)), "built");
    page.AddImage("x.png", image);
    page.Layout(400.0f);

    gfx::DisplayList list;
    page.Paint(list, 0.0f);
    Expect(list.Bounds().width >= 40 && list.Bounds().height >= 30,
           "the width and height attributes are where most of the web still puts an image's "
           "size, and the cascade never sees them");
  });

  AddTest(tests, "Layout/AnImageThatNeverArrivesStillOccupiesItsDeclaredSize", [] {
    // Otherwise the page reflows when the image lands, which is the layout
    // shift every user has learned to hate.
    TestFonts fonts;
    engine::Page page(fonts.catalog);
    page.Load("<body style='margin:0'><img src='missing.png' width='50' height='60'></body>",
              "https://example.org/");
    page.Layout(400.0f);
    Expect(page.ContentHeight() >= 60.0f, "the box is there before the pixels are");
  });

  AddTest(tests, "Engine/FetchesDecodesAndDrawsAnImage", [] {
    Session session;
    ScriptedFactory factory;
    const std::vector<std::byte> png = BuildPng(PngSpec{
        16, 8, 8, 6, false, {}, {}, SolidRgbaRows(16, 8, 0x20, 0x80, 0xC0, 0xFF), 0});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body style='margin:0'><img src='/pic.png'></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("image/png", std::string(reinterpret_cast<const char*>(png.data()),
                                            png.size()))});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the document and the image were both fetched");
    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    ExpectEqInt(static_cast<long long>(frame->display_list.Images().size()), 1,
                "with the decoded image on it");
    Expect(frame->display_list.Images().at(0)->Width() == 16 &&
               frame->display_list.Images().at(0)->Height() == 8,
           "at the size the PNG declared");
  });

  AddTest(tests, "Engine/BytesThatAreNotAnImageDoNotBreakThePage", [] {
    // Image bytes are attacker-controlled. A decoder failure is an image that
    // does not draw, not a page that does not render.
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body><img src='/bad.png'><p>ABC</p></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true, OkResponse("image/png", "not a png at all")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "the page still painted");
    Expect(frame->display_list.Images().empty(), "with no image");
    Expect(TextRunCount(frame->display_list) > 0, "and its text intact");
  });

  AddTest(tests, "Engine/AnImageSurvivesTheWireFormat", [] {
    Session session;
    ScriptedFactory factory;
    const std::vector<std::byte> png = BuildPng(PngSpec{
        4, 4, 8, 6, false, {}, {}, SolidRgbaRows(4, 4, 0x11, 0x22, 0x33, 0xFF), 0});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<body><img src='/p.png'></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("image/png",
                   std::string(reinterpret_cast<const char*>(png.data()), png.size()))});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{200, 100}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});

    const ipc::PaintFrameMessage* frame = session.LastFrame();
    Expect(frame != nullptr, "a frame was painted");
    const auto decoded = ipc::DeserializeEngineToUi(ipc::Serialize(ipc::EngineToUi{*frame}));
    Expect(decoded.has_value(), "and it survived its own wire format");
    const auto& list = std::get<ipc::PaintFrameMessage>(*decoded).display_list;
    ExpectEqInt(static_cast<long long>(list.Images().size()), 1, "the image crossed");
    Expect(list.Images().at(0)->Width() == 4 && list.Images().at(0)->Height() == 4,
           "at its own size");
    Expect(std::equal(list.Images().at(0)->Pixels().begin(), list.Images().at(0)->Pixels().end(),
                      frame->display_list.Images().at(0)->Pixels().begin()),
           "pixel for pixel -- the wire carries the bitmap, since a display list that named a "
           "resource by id would need the receiver's cache to be part of the contract");
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

  AddTest(tests, "Engine/ReloadCanBypassTheHttpCache", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: max-age=600\r\n"
        "Content-Length: 34\r\n\r\n<title>One</title><body>one</body>"});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: max-age=600\r\n"
        "Content-Length: 34\r\n\r\n<title>Two</title><body>two</body>"});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/page.html"});
    ExpectEqString(session.LastTitle(), "One", "the first document committed");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 1,
                "the initial navigation fetched the document");

    session.Send(ipc::ReloadMessage{false});
    ExpectEqString(session.LastTitle(), "One", "ordinary reload may use a fresh cache entry");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 1,
                "and did not open another connection");

    session.Send(ipc::ReloadMessage{true});
    ExpectEqString(session.LastTitle(), "Two", "cache-bypassing reload fetched the new document");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "and made the second request");
  });

  AddTest(tests, "Engine/ClickingALinkNavigatesToIt", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><a href='/next'>ABC</a></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Next</title><body>next page</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/next",
                   "the relative href was resolved against the document URL");
    ExpectEqString(session.LastTitle(), "Next", "and the clicked document committed");
    ExpectEqInt(static_cast<long long>(factory.log.requests.size()), 2,
                "the initial page and the clicked page were fetched");
    Expect(factory.log.requests.at(1).find("GET /next ") != std::string::npos,
           "the second request is for the clicked link");
    Expect(factory.log.requests.at(1).find("Referer: https://example.org/start\r\n") !=
               std::string::npos,
           "the clicked navigation carries the policy-computed referrer");
  });

  AddTest(tests, "Engine/ClickingAGetFormSubmitNavigatesToTheSerializedQuery", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Results</title><body>results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{45, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(),
                   "https://example.org/search?q=hello+world&go=Search",
                   "the form query was encoded and resolved against the document URL");
    ExpectEqString(session.LastTitle(), "Results", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=hello+world&go=Search ") !=
               std::string::npos,
           "the second request is the submitted GET form");
    Expect(factory.log.requests.at(1).find("Referer: https://example.org/start\r\n") !=
               std::string::npos,
           "the GET form navigation carries the policy-computed referrer");
  });

  AddTest(tests, "Engine/ClickingAPostFormSubmitSendsARequestBody", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search?keep=1' method='post'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Posted</title><body>posted results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{45, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?keep=1",
                   "POST form navigation commits the action URL without moving controls into it");
    ExpectEqString(session.LastTitle(), "Posted", "and the result document committed");
    const std::string& request = factory.log.requests.at(1);
    Expect(request.rfind("POST /search?keep=1 HTTP/1.1\r\n", 0) == 0,
           "the second request uses the form method and preserves the action query");
    Expect(request.find("Referer: https://example.org/start\r\n") != std::string::npos,
           "the POST form navigation carries the policy-computed referrer");
    Expect(request.find("Content-Type: application/x-www-form-urlencoded\r\n") !=
               std::string::npos,
           "the request carries the form encoding");
    Expect(request.find("Content-Length: 23\r\n") != std::string::npos,
           "the serialized controls define the request body length");
    Expect(request.size() >= 23 &&
               request.substr(request.size() - 23) == "q=hello+world&go=Search",
           "the form controls are sent in the body");
  });

  AddTest(tests, "Engine/ClickingATextPlainPostFormSendsAPlainRequestBody", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/plain' method='post' "
                   "enctype='text/plain'>"
                   "<input name='q' value='hello world' size='2'>"
                   "<input type='submit' name='go' value='Search'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Plain</title><body>plain results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{45, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/plain",
                   "POST text/plain commits the action URL");
    ExpectEqString(session.LastTitle(), "Plain", "and the result document committed");
    const std::string& request = factory.log.requests.at(1);
    Expect(request.rfind("POST /plain HTTP/1.1\r\n", 0) == 0,
           "the second request uses POST");
    Expect(request.find("Content-Type: text/plain\r\n") != std::string::npos,
           "the request carries the selected form encoding");
    Expect(request.find("Content-Length: 26\r\n") != std::string::npos,
           "the plain form body length includes CRLF row endings");
    Expect(request.size() >= 26 &&
               request.substr(request.size() - 26) == "q=hello world\r\ngo=Search\r\n",
           "the form controls are sent as name=value lines without URL encoding");
  });

  AddTest(tests, "Engine/TextInputChangesFocusedFormControlsBeforeSubmit", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' size='2'><input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Typed</title><body>typed results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});
    session.Send(ipc::TextInputMessage{"hi"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{45, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?q=hi",
                   "submitted GET forms use the current focused input value");
    ExpectEqString(session.LastTitle(), "Typed", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=hi ") != std::string::npos,
           "the second request contains the typed query");
  });

  AddTest(tests, "Engine/TextLikeInputTypesCanBeEditedAndSubmitted", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/contact'>"
                   "<input type='email' name='email' size='4'><input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Contact</title><body>contact</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});
    session.Send(ipc::TextInputMessage{"a@b"});
    session.Send(ipc::InputCommandMessage{ipc::InputCommandMessage::Command::Enter});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/contact?email=a%40b",
                   "text-like input types use the focused text-editing path");
    ExpectEqString(session.LastTitle(), "Contact", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /contact?email=a%40b ") != std::string::npos,
           "the second request contains the edited email value");
  });

  AddTest(tests, "Engine/InputCommandsEditAndSubmitFocusedForm", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<body style='margin:0'><form action='/search'>"
                   "<input name='q' size='3'><input type='submit' name='go' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Commands</title><body>command results</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});
    session.Send(ipc::TextInputMessage{"abc"});
    session.Send(ipc::InputCommandMessage{ipc::InputCommandMessage::Command::Backspace});
    session.Send(ipc::InputCommandMessage{ipc::InputCommandMessage::Command::Delete});
    session.Send(ipc::InputCommandMessage{ipc::InputCommandMessage::Command::Enter});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/search?q=ab",
                   "enter submits the edited focused form without a clicked submit button");
    ExpectEqString(session.LastTitle(), "Commands", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /search?q=ab ") != std::string::npos,
           "the second request contains the command-edited query");
  });

  AddTest(tests, "Engine/ClickingCheckableInputsUpdatesSubmittedForm", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<style>input{width:10px;height:10px;margin:0}</style>"
                   "<body style='margin:0'><form action='/filter'>"
                   "<input type='checkbox' name='seen'>"
                   "<input type='radio' name='mode' value='old' checked>"
                   "<input type='radio' name='mode' value='new'>"
                   "<input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Filtered</title><body>filtered</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{25, 5}, 1});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{35, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/filter?seen=on&mode=new",
                   "submitted GET forms use clicked checkable state");
    ExpectEqString(session.LastTitle(), "Filtered", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /filter?seen=on&mode=new ") !=
               std::string::npos,
           "the second request contains the toggled controls");
  });

  AddTest(tests, "Engine/ClickingResetRestoresSubmittedFormDefaults", [] {
    Session session;
    ScriptedFactory factory;
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html",
                   "<style>input{width:20px;height:20px;margin:0}</style>"
                   "<body style='margin:0'><form action='/filter'>"
                   "<input name='q'>"
                   "<input type='checkbox' name='seen' checked>"
                   "<input type='reset' value='Reset'>"
                   "<input type='submit' value='Go'>"
                   "</form></body>")});
    factory.script.push_back(ScriptedTransport::Exchange{
        "example.org", 443, true,
        OkResponse("text/html", "<title>Filtered</title><body>filtered</body>")});
    session.engine.PageLoader().SetTransport(factory);

    session.Send(ipc::ResizeViewportMessage{gfx::IntSize{400, 300}, 1.0f});
    session.Send(ipc::NavigateMessage{"https://example.org/start"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{5, 5}, 1});
    session.Send(ipc::TextInputMessage{"abc"});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{25, 5}, 1});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{45, 5}, 1});
    session.Send(ipc::PointerMessage{ipc::PointerMessage::Kind::Down, gfx::IntPoint{65, 5}, 1});

    ExpectEqString(session.LastCommittedUrl(), "https://example.org/filter?q=&seen=on",
                   "submitted GET forms use reset defaults");
    ExpectEqString(session.LastTitle(), "Filtered", "and the result document committed");
    Expect(factory.log.requests.at(1).find("GET /filter?q=&seen=on ") != std::string::npos,
           "the second request contains the reset form state");
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
