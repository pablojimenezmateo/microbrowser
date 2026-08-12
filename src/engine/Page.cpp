#include "engine/Page.h"

#include <algorithm>
#include <utility>

#include "css/StyleSheet.h"
#include "dom/FlatTree.h"
#include "gfx/SvgDecoder.h"
#include "engine/FormAlgorithms.h"
#include "engine/ImageSelection.h"
#include "html/Focus.h"
#include "html/FormControl.h"
#include "html/Encoding.h"
#include "html/TreeBuilder.h"
#include "util/Parse.h"
#include "util/Env.h"
#include "util/LoadTimeline.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/PerformanceTrace.h"

#include <cstdio>

namespace microbrowser::engine {

namespace {

std::size_t CountBoxes(const layout::Box& box) {
  std::size_t count = 1;
  for (const std::unique_ptr<layout::Box>& child : box.Children()) {
    count += CountBoxes(*child);
  }
  return count;
}

using util::AddPerformanceCounter;
using util::PerfCounterId;

// The text of an element's direct text children, concatenated. Enough for
// <title>, which is a text-only element by definition.
std::string DirectText(const dom::Element& element) {
  std::string text;
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsText()) {
      text += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return text;
}

// Composed-tree parent for click / focus / activation. A shadow root has no
// parent by design (ADR 0019 §2); crossing to the host is how a click on an
// <img> inside yt-image reaches a#thumbnail (youtube search). Matches
// EventDispatch's propagation walk.
dom::Element* ComposedParentElement(dom::Element* element) {
  if (element == nullptr) {
    return nullptr;
  }
  if (dom::Node* parent = element->Parent(); parent != nullptr && parent->IsElement()) {
    return static_cast<dom::Element*>(parent);
  }
  if (const dom::Element* host = dom::ShadowHostOf(*element)) {
    return const_cast<dom::Element*>(host);
  }
  return nullptr;
}

bool IsInComposedDocument(const dom::Element* element, const dom::Document* document) {
  if (element == nullptr || document == nullptr) {
    return false;
  }
  for (const dom::Node* node = element; node != nullptr;) {
    if (node == document) {
      return true;
    }
    if (node->Parent() != nullptr) {
      node = node->Parent();
      continue;
    }
    if (const dom::Element* host = dom::ShadowHostOf(*node)) {
      node = host;
      continue;
    }
    break;
  }
  return false;
}

bool IsValueResettableControl(const dom::Element& element) {
  return html::IsTextControl(element);
}

bool IsRadioGroupPeer(const dom::Element& candidate,
                      const dom::Element& activated,
                      const dom::Document& document) {
  if (&candidate == &activated || !html::IsRadioInput(candidate)) {
    return false;
  }
  const std::string* candidate_name = candidate.GetAttribute("name");
  const std::string* activated_name = activated.GetAttribute("name");
  if (candidate_name == nullptr || activated_name == nullptr || candidate_name->empty() ||
      *candidate_name != *activated_name) {
    return false;
  }
  return html::FormOwner(candidate, document) == html::FormOwner(activated, document);
}

}  // namespace

Page::Page(gfx::FontProvider& fonts)
    : text_ctx_(fonts), canvases_(text_ctx_.Text()), video_(media_) {
  // The binding layer asks its geometry questions here. Handed over in the
  // constructor rather than per navigation because it is this object for the
  // life of the page, and a source that arrived later would leave the first
  // script of a document without one.
  script_->SetGeometrySource(this);
  // Element.animate → this Page's Animations (TD-0021). Same lifetime as geometry.
  script_->SetAnimationSource(AsAnimationSource());
  // And its media questions, for the same reason and with the same lifetime: the state machines
  // are this object's, so `<video>` has its API from the first script of the first document.
  script_->SetMediaController(this);
  // And its canvas commands. Same lifetime and same reason: a `<canvas>` in the first document must have
  // its context from the first script, and a surface handed over later would leave that script without
  // one -- which for a page whose whole rendering is a canvas is a blank page.
  script_->SetCanvasSurface(this);
  script_->SetWorkerHost(this);
}

const std::vector<std::string>& Page::ConsoleOutput() const { return script_->ConsoleOutput(); }

const std::vector<std::string>& Page::ScriptErrors() const { return script_->ScriptErrors(); }

std::string Page::EvaluateScript(std::string_view source) {
  if (document_ == nullptr) {
    return {};
  }
  const std::string answer = script_->Evaluate(*document_, url_, source);
  // A probe can mutate the document -- and a probe that *renders* something is
  // the useful kind. Laying out afterwards means the caller's next frame shows
  // what it did rather than what the page looked like before it.
  EnsureLayoutClean();
  return answer;
}

void Page::AddScript(std::size_t pending_index, std::string source) {
  script_->AddFetched(pending_index, std::move(source));
}

bool Page::CollectInsertedScripts() {
  if (document_ == nullptr) {
    return false;
  }
  return script_->CollectInserted(*document_, policy_);
}

void Page::RunScripts(std::int64_t now_ms) {
  if (document_ == nullptr) {
    return;
  }
  // Snapshot before the turn: a script that only schedules work (timers, rAF,
  // fetch) must not drop a laid-out tree. Polymer's stamp path used to pay a
  // full BuildBoxTree per such turn even when MutationVersion was unchanged.
  const std::uint64_t doc_before = document_->MutationVersion();
  const std::uint64_t cascade_before = resolver_.Generation();
  script_->Run(*document_, url_, now_ms);
  if (document_->MutationVersion() != doc_before ||
      resolver_.Generation() != cascade_before) {
    // A script can change the tree or the cascade, so anything derived from
    // either is stale. The box tree is dropped rather than patched: incremental
    // layout is a later decision and a wrong one made early here would be
    // invisible.
    InvalidateBoxTree();
    CollectImages();
    util::AddPerformanceCounter(util::PerfCounterId::BoxTreeInvalidatedByScript);
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::BoxTreeScriptSkipped);
  }
}

void Page::Load(std::string_view html, std::string url, csp::PolicyList header_policy,
                std::string_view content_type) {
  util::PerformanceTrace::Scope scope("engine::Page::Load");

  url_ = std::move(url);
  // Before anything is collected. The policy decides which stylesheets and
  // scripts this document even has.
  policy_.Reset(std::move(header_policy), url_);
  // Before the document goes, and this order is load-bearing: the binding layer
  // holds a reference to it, so dropping the script half after replacing the
  // document would leave that reference dangling for exactly as long as it took
  // the next page's first script to read the tree.
  script_->Detach();
  // Before the document goes, for the reason the line above is: every frame's element lives in the
  // document that is about to be replaced, and the borrowed pointer each one holds has to be
  // cleared while that element is still there to clear it on. ADR 0027 §1, and see Frames.h.
  ClearFrames();
  // A fresh resolver per document. Author sheets belong to the document that
  // carried them, and keeping the old one would let the previous page's CSS
  // style this one.
  ResetResolver();
  animations_.Clear();
  keyframes_.clear();
  // A canvas is the largest thing a document can hold, so one that outlived its page would be up to
  // 64MB leaked per navigation.
  canvases_.Clear();
  video_.Clear();
  // ADR 0022 §1's "joined when its document dies, before the document's objects are destroyed". Every
  // worker thread is stopped and joined here, on the main thread, while the document is still alive.
  workers_.Clear();
  unrequested_worker_scripts_.clear();
  blob_urls_.Clear();
  pointer_down_target_ = nullptr;
  // **The bytes become text here, before the tokenizer sees them.** ADR 0025 §2: the encoding comes
  // from the BOM, then `Content-Type`, then a prescan of the first 1024 bytes, then windows-1252 --
  // and the tokenizer's input is code points rather than bytes, which is what makes an ill-formed
  // sequence a U+FFFD rather than a byte it might read as markup.
  //
  // Decoded into a local that outlives the parse: `ParseDocument` takes a view, and a temporary here
  // would be a dangling one.
  const html::Encoding encoding = html::SniffEncoding(html, content_type);
  // Remembered beside the base URL, because "encoding-parse a URL" takes both
  // and nothing else -- see DocumentPolicy.h. Every link on this document and
  // every form it submits has its query encoded with it.
  policy_.SetEncoding(encoding);
  const std::string decoded = html::DecodeToUtf8(html, encoding);
  {
    util::LoadTimeline::Mark("document.parse.start");
    util::PerformanceTrace::ScopeLabel label("html::ParseDocument");
    label.Field("bytes", static_cast<long long>(decoded.size()));
    util::PerformanceTrace::Scope parse(label.View());
    document_ = html::ParseDocument(decoded);
  }
  InvalidateBoxTree();
  // A new document starts at the top, and the scroll offset goes with the
  // layout state rather than surviving it. So does every per-element offset:
  // the keys are pointers into the document that just went, and keeping them
  // would be a use-after-free waiting for the next page to allocate an element
  // at the same address.
  layout_ = LayoutState{};
  scroll_ = ScrollState{};
  content_height_ = 0.0f;
  resources_ = DocumentResources{};
  control_defaults_.clear();

  // The `<meta>` policies and the `<base href>`, before the collections that
  // depend on both: a policy delivered in the document governs that document's
  // own resources, and a `<base>` changes what every relative URL in it means.
  ApplyDocumentHeadPolicy();

  CollectStyleSheets();
  // The child browsing contexts, collected where the stylesheets are and for the same reason: a
  // frame is a subresource this document named, and what a URL turns into is the loader's problem
  // rather than this one's. The loader asks `CollectFrames` for what to fetch.
  CollectFrames();
  // `:target` comes from the address rather than from the markup, so it is set
  // where the address arrives. ADR 0016 §2 -- one copy, and it cannot disagree
  // with what the URL bar says.
  RefreshTargetState();
  if (document_ != nullptr) {
    // Found now, run later: an external script has to arrive before anything
    // after it in the document may run, and what a URL turns into is the
    // loader's problem rather than this one's.
    // Before Collect, because a module script's source can arrive and be asked
    // what it imports long before there is an interpreter -- and resolving a
    // relative specifier needs a base.
    script_->SetModuleDocumentUrl(url_);
    script_->Collect(*document_, policy_);
  }
  if (document_ != nullptr) {
    document_->ForEachDescendant([&](const dom::Node& node) {
      if (!node.IsElement()) {
        return;
      }
      const auto& element = static_cast<const dom::Element&>(node);
      if (element.TagName() == "input" || element.TagName() == "textarea") {
        control_defaults_.emplace(&element,
                                  std::pair<std::string, bool>{ControlValue(element),
                                                               element.HasAttribute("checked")});
      }
    });
  }
  ExtractTitle();
}

void Page::SetViewport(const css::MediaContext& viewport) {
  const bool media_changed = viewport.viewport_width != viewport_.viewport_width ||
                             viewport.viewport_height != viewport_.viewport_height ||
                             viewport.device_pixel_ratio != viewport_.device_pixel_ratio;
  viewport_ = viewport;
  resolver_.SetMediaContext(viewport_);
  // So that a layout forced by a geometry query before the engine's first
  // Layout still runs at the width the document will be shown at.
  layout_.width = viewport.viewport_width;
  if (media_changed && !resources_.author_sheet_slots.empty()) {
    // `@media` is evaluated when a sheet is parsed, so a viewport that moved
    // means the sheets have to be parsed again -- a rule that did not match at
    // 500 pixels does at 1280, and the sheet as stored has already dropped it.
    // Gated on the size actually changing, because the engine sets the viewport
    // on every resize message and re-parsing a page's stylesheets per pixel of
    // a drag is not a thing to do. See ParseStyleSheet.
    RebuildAuthorStyleSheets();
  }
}

void Page::ExtractTitle() {
  title_.clear();
  if (document_ != nullptr) {
    if (const dom::Element* element = document_->FirstElementByTagName("title")) {
      title_ = DirectText(*element);
    }
  }
  if (title_.empty()) {
    // Never empty: a tab strip has to show something, and "" is not a title,
    // it is a missing one.
    title_ = url_.empty() ? std::string("New Tab") : url_;
  }
}

float Page::Layout(float width) {
  util::PerformanceTrace::Scope scope("engine::Page::Layout");
  layout_.width = width;
  if (document_ == nullptr) {
    content_height_ = 0.0f;
    layout_.laid_out_width = width;
    return 0.0f;
  }
  // Recorded before layout rather than after: nothing here mutates the document,
  // and reading it afterwards would fold any future mutation made
  // *during* layout into the version this claims to describe.
  // A shadow root's `<style>` is unreachable from the document walk that collects
  // the rest, so it is collected here -- at the one point that runs after every
  // batch of mutations and before the cascade reads anything. The comparison is
  // over the text, so an unchanged component costs one walk rather than a
  // re-parse. ADR 0019 §3.
  if (CollectShadowStyleSheets()) {
    RebuildAuthorStyleSheets();
  }
  // Same clean check `EnsureLayoutClean` uses, plus cascade and laid-out width.
  // `LayoutAndPaint` used to reflow on every rAF even when the tree had not
  // moved — on a stamped youtube tree each pass is seconds, and the snapshot's
  // post-load `RunDueWork` loop never finished (TD-0018).
  const std::uint64_t cascade = resolver_.Generation();
  const std::uint64_t doc_ver = document_->MutationVersion();
  if (boxes_ != nullptr && layout_.document_version == doc_ver &&
      layout_.box_tree_cascade_generation == cascade &&
      layout_.laid_out_width == width) {
    AddPerformanceCounter(PerfCounterId::LayoutSkippedClean);
    return content_height_;
  }
  AddPerformanceCounter(PerfCounterId::LayoutPasses);
  EnsureBoxTree();
  // Attribute / style writes leave the box tree in place but stale styles on
  // the boxes. Restyle before placing, or offsetWidth after `el.style.width`
  // reads the previous geometry (GeometryQueryTests / TD-0021).
  if (boxes_ != nullptr && document_ != nullptr &&
      layout_.document_version != document_->MutationVersion()) {
    RestyleWithoutLayout();
  }
  const layout::LayoutEngine engine(resolver_, text_ctx_.Measurer(), this);
  // The box tree is rebuilt when the document *structure* or cascade changes.
  // Background images are queued during that one cascade pass rather than in a
  // second walk -- TD-0005.
  util::LoadTimeline::Mark("layout.start");
  {
    if (util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE")) {
      std::fprintf(stderr, "[load] LayoutBoxes enter\n");
      std::fflush(stderr);
    }
    util::PerformanceTrace::Scope place("engine::LayoutBoxes");
    content_height_ = engine.Layout(*boxes_, width, viewport_.viewport_height);
    if (util::EnvFlagEnabled("MICROBROWSER_LOAD_TURN_TRACE")) {
      std::fprintf(stderr, "[load] LayoutBoxes end\n");
      std::fflush(stderr);
    }
    AddPerformanceCounter(PerfCounterId::LayoutPassBoxes, CountBoxes(*boxes_));
  }
  layout_.document_version = document_->MutationVersion();
  layout_.structure_version = document_->StructureVersion();
  layout_.laid_out_width = width;
  util::LoadTimeline::Mark("layout.end");
  // The scroll offsets go back on, clamped against the overflow this layout
  // just measured. Layout consults them and does not own them -- ADR 0018 §1 --
  // which is what makes a scrolled menu still scrolled after a script changes a
  // class on the page around it.
  layout::UpdateScrollState(*boxes_, scroll_.offsets);
  return content_height_;
}

void Page::Paint(gfx::DisplayList& out) const {
  util::PerformanceTrace::Scope scope("engine::Page::Paint");
  if (boxes_ == nullptr) {
    return;
  }
  layout::BuildDisplayList(*boxes_, out, gfx::FloatPoint{0.0f, -layout_.scroll_y},
                           gfx::FloatSize{viewport_.viewport_width, viewport_.viewport_height});
  AddPerformanceCounter(PerfCounterId::DisplayListBuilds);
}

std::optional<FormSubmission> Page::SubmitForm(const dom::Element& form,
                                               const dom::Element* submitter) {
  // The event first. A page that adds fields in `onsubmit` -- which is what
  // reddit's interstitial does -- has to have run before the data set is
  // built, and one that calls `preventDefault` must not be submitted at all.
  if (script_->DispatchSubmit(const_cast<dom::Element&>(form))) {
    return std::nullopt;
  }
  return BuildFormSubmission(form, submitter, *document_, url_, policy_.Encoding());
}

std::optional<FormSubmission> Page::TakeScriptFormSubmission() {
  const std::optional<bindings::PendingSubmit> pending = script_->TakePendingSubmit();
  if (!pending.has_value() || pending->form == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  // No `submit` event here: `requestSubmit()` already fired one on its way
  // through and `submit()` fires none by definition. Firing one now would run
  // a page's handler twice for one submission.
  return BuildFormSubmission(*pending->form, pending->submitter, *document_, url_,
                             policy_.Encoding());
}

std::optional<FormSubmission> Page::ApplyScriptActivation(bool& changed_document,
                                                          std::optional<std::string>& href) {
  changed_document = false;
  href.reset();
  const std::vector<dom::Element*> clicked = script_->TakePendingActivations();
  if (clicked.empty()) {
    return std::nullopt;
  }
  // **The same walk a real click takes**, once per click. `element.click()`
  // from script has the activation behaviour a pointer release has -- the
  // specification runs one algorithm, and two copies of it is how a checkbox
  // toggles under the mouse and not under `click()`. The `preventDefault`
  // check already happened where the event was dispatched.
  //
  // Every element in the list, in the order they were clicked. A turn that
  // clicked four things activated only one before this was a list, and the
  // other three did nothing -- silently, which is the worst way for an
  // activation to fail.
  std::optional<FormSubmission> submission;
  for (dom::Element* element : clicked) {
    if (element == nullptr) {
      continue;
    }
    const ClickActivation activation = ResolveClickActivation(element);
    if (activation.form.has_value() && !submission.has_value()) {
      submission = activation.form;
    }
    if (activation.href.has_value() && !href.has_value()) {
      href = activation.href;
    }
    changed_document = changed_document || activation.reset_form ||
                       activation.toggled_checkable || activation.toggled_media ||
                       activation.toggled_details;
  }
  return submission;
}

std::optional<FormSubmission> Page::FormSubmissionRequestAt(gfx::FloatPoint document_point) {
  EnsureLayoutClean();
  if (boxes_ == nullptr || document_ == nullptr) {
    return std::nullopt;
  }
  const dom::Element* submitter =
      HitTestFormControlAt(*boxes_, document_point, html::IsSubmitControl, layout_.scroll_y);
  if (submitter == nullptr) {
    return std::nullopt;
  }
  const dom::Element* form = html::FormOwner(*submitter, *document_);
  if (form == nullptr) {
    return std::nullopt;
  }
  return SubmitForm(*form, submitter);
}

bool Page::DispatchPointerDownAt(gfx::FloatPoint document_point,
                                 const bindings::PointerInput& pointer) {
  if (boxes_ == nullptr) {
    return false;
  }
  const dom::Element* target = ElementAt(document_point);
  if (target == nullptr) {
    pointer_down_target_ = nullptr;
    return false;
  }
  // User activation on press, not on click: `play()` and fullscreen gates read
  // it from the gesture that started here (ADR 0017 §3, ADR 0028 §1).
  if (document_ != nullptr) {
    document_->NoteUserActivation();
  }
  auto& element = *const_cast<dom::Element*>(target);
  pointer_down_target_ = &element;
  (void)script_->DispatchPointerMouse(element, "pointerdown", pointer);
  (void)script_->DispatchPointerMouse(element, "mousedown", pointer);
  (void)FocusFromClickAt(document_point);
  return true;
}

DispatchOutcome Page::DispatchPointerReleaseAt(gfx::FloatPoint document_point,
                                               const bindings::PointerInput& pointer) {
  DispatchOutcome outcome;
  if (boxes_ == nullptr) {
    pointer_down_target_ = nullptr;
    return outcome;
  }
  dom::Element* down = pointer_down_target_;
  pointer_down_target_ = nullptr;
  const dom::Element* up_hit = ElementAt(document_point);
  dom::Element* up = up_hit != nullptr ? const_cast<dom::Element*>(up_hit) : nullptr;

  // UI Events: pointerup/mouseup fire at the element under the pointer; click
  // fires at the nearest common ancestor of the press and release targets. A
  // re-hit-test-only click is how Accept-over-a-result navigates to /watch.
  // Parent walks cross shadow hosts (ADR 0019) — youtube thumbnails put the
  // <img> inside yt-image's shadow under a#thumbnail.
  auto common_ancestor = [](dom::Element* a, dom::Element* b) -> dom::Element* {
    if (a == nullptr || b == nullptr) {
      return nullptr;
    }
    for (dom::Element* candidate = a; candidate != nullptr;
         candidate = ComposedParentElement(candidate)) {
      for (dom::Element* other = b; other != nullptr; other = ComposedParentElement(other)) {
        if (candidate == other) {
          return candidate;
        }
      }
    }
    return nullptr;
  };

  if (!IsInComposedDocument(down, document_.get())) {
    down = nullptr;
  }
  if (!IsInComposedDocument(up, document_.get())) {
    up = nullptr;
  }

  dom::Element* click_target = nullptr;
  if (down != nullptr && up != nullptr) {
    click_target = common_ancestor(down, up);
    if (down != up) {
      util::AddPerformanceCounter(util::PerfCounterId::InputClickRetargeted);
    }
  } else if (down != nullptr) {
    click_target = down;
  } else if (up != nullptr) {
    // No remembered press (or it left the document): fall back to the release
    // hit so synthetic paths that only release still activate.
    click_target = up;
  }

  dom::Element* up_event_target = up != nullptr ? up : click_target;
  if (up_event_target == nullptr) {
    return outcome;
  }
  outcome.ran = script_->HasListeners();
  outcome.click_target = click_target;
  (void)script_->DispatchPointerMouse(*up_event_target, "pointerup", pointer);
  (void)script_->DispatchPointerMouse(*up_event_target, "mouseup", pointer);
  if (click_target != nullptr) {
    outcome.prevented = script_->DispatchClick(*click_target, pointer);
  }
  return outcome;
}

DispatchOutcome Page::DispatchClickAt(gfx::FloatPoint document_point,
                                      const bindings::PointerInput& pointer) {
  (void)DispatchPointerDownAt(document_point, pointer);
  return DispatchPointerReleaseAt(document_point, pointer);
}


void Page::AbandonForNavigation() {
  // Interpreter, timers, rAF, idle, host tasks. The DOM stays until Load; it
  // must not schedule against the in-flight document GET (TD-0048).
  script_->Detach();
  animations_.Clear();
  util::AddPerformanceCounter(util::PerfCounterId::EngineScriptAbandonedForNavigation);
}

std::optional<std::uint32_t> Page::NextWakeDelay(std::int64_t now_ms) const {
  std::optional<std::uint32_t> from_script = script_->NextWakeDelay(now_ms);
  // A running transition or animation asks for a frame. **Nothing when nothing is running**, which is
  // the whole of how ADR 0014 §5's invariant is kept: a page with a `:hover` transition costs nothing
  // while the pointer is elsewhere, because there is no transition to ask.
  if (const std::optional<std::uint32_t> frame = animations_.NextDelayMs(now_ms)) {
    from_script = from_script.has_value() ? std::min(*from_script, *frame) : frame;
  }
  if (const std::optional<std::uint32_t> video = video_.NextDelayMs(now_ms)) {
    from_script = from_script.has_value() ? std::min(*from_script, *video) : video;
  }
  if (scroll_.pending_events.empty()) {
    return from_script;
  }
  // A `scroll` event is owed. Zero rather than a frame interval, because the
  // wheel that caused it already woke the loop and the event is delivered on
  // the way through this turn -- and because a delay would let a second notch
  // arrive first, which is what the throttling is *for* rather than something
  // to schedule around. The queue empties in RunDueWork and the loop goes back
  // to blocking; a settled page never reaches this line.
  return 0;
}

Page::DueWorkKind Page::RunDueWork(std::int64_t now_ms, bool* script_ran) {
  // Drop the box tree only when the *structure* or cascade moved. Attribute /
  // style writes (WAAPI polyfills, Polymer hosts) bump MutationVersion every
  // frame — rebuilding boxes for those was a 60Hz BuildBoxTree on youtube
  // (TD-0021). Animation ticks and video frames are restyle/paint paths.
  const std::uint64_t ver_before =
      document_ != nullptr ? document_->MutationVersion() : 0;
  const std::uint64_t structure_before =
      document_ != nullptr ? document_->StructureVersion() : 0;
  const std::uint64_t cascade_before = resolver_.Generation();
  bool ran = DispatchPendingScrollEvents();
  ran = script_->RunDueWork(now_ms) || ran;
  if (script_ran != nullptr) {
    *script_ran = ran;
  }
  bool animation_tick = false;
  if (animations_.Running()) {
    (void)animations_.Advance(now_ms);
    animation_time_ms_ = now_ms;
    animation_tick = true;
    ran = true;
    AddPerformanceCounter(PerfCounterId::AnimationFramesProduced);
  }
  // After Advance *and* after script: cancel() queues a finished notice while
  // Running() may already be false, so delivery cannot sit inside the Running
  // branch alone.
  if (script_->DeliverFinishedAnimations()) {
    ran = true;
  }
  const bool video_updated =
      video_.AdvanceAll([this](dom::Element& element) { return MediaStateFor(element); });
  ran = video_updated || ran;
  if (!ran) {
    return DueWorkKind::None;
  }
  const bool structure_changed =
      document_ != nullptr && document_->StructureVersion() != structure_before;
  const bool attrs_changed =
      document_ != nullptr && document_->MutationVersion() != ver_before;
  const bool cascade_changed = resolver_.Generation() != cascade_before;
  if (structure_changed || cascade_changed) {
    InvalidateLayout();
    AddPerformanceCounter(PerfCounterId::BoxTreeInvalidatedByDueWork);
    return DueWorkKind::Layout;
  }
  if (attrs_changed || animation_tick) {
    if (attrs_changed) {
      // `img.src = url` from IntersectionObserver (or any script) bumps
      // MutationVersion without StructureVersion — CollectImages must still run
      // or TakeUnrequestedImages never sees the new URL (youtube search thumbs).
      CollectImages();
    }
    if (boxes_ != nullptr) {
      // Throttle full-document restyle: WAAPI polyfills write `style` every
      // frame, and RestyleWithoutLayout walks the whole tree (TD-0021).
      constexpr std::int64_t kAttrRestyleMinMs = 50;
      const bool restyle_now =
          animation_tick || last_attr_restyle_ms_ == 0 ||
          now_ms - last_attr_restyle_ms_ >= kAttrRestyleMinMs;
      if (restyle_now) {
        RestyleWithoutLayout();
        last_attr_restyle_ms_ = now_ms;
      }
      if (animation_tick && animations_.TickNeedsLayout()) {
        layout_.laid_out_width = -1.0f;
        AddPerformanceCounter(PerfCounterId::LayoutAnimationTick);
        return DueWorkKind::Layout;
      }
      if (attrs_changed) {
        AddPerformanceCounter(PerfCounterId::LayoutAttrPaintOnly);
      } else {
        AddPerformanceCounter(PerfCounterId::LayoutAnimationPaintOnly);
      }
      return DueWorkKind::Paint;
    }
    InvalidateLayout();
    AddPerformanceCounter(PerfCounterId::BoxTreeInvalidatedByDueWork);
    return DueWorkKind::Layout;
  }
  if (video_updated) {
    AddPerformanceCounter(PerfCounterId::LayoutVideoPaintOnly);
    return DueWorkKind::Paint;
  }
  // Timers / rAF / tasks ran without touching the tree. Paint if a frame was
  // owed; do not reflow — Layout would no-op only when MutationVersion matches,
  // and a script that only scheduled more work must not force LayoutAndPaint's
  // full path as "Layout".
  AddPerformanceCounter(PerfCounterId::LayoutDueWorkClean);
  return DueWorkKind::Paint;
}

void Page::SetNetworkSource(bindings::NetworkSource* network) {
  script_->SetNetworkSource(network);
}

void Page::SetTrustedInsertionFlush(std::function<void()> hook) {
  script_->SetTrustedInsertionFlush(std::move(hook));
}

void Page::SetHistorySource(bindings::HistorySource* history) {
  script_->SetHistorySource(history);
}

void Page::SetStorageSource(bindings::StorageSource* storage) {
  script_->SetStorageSource(storage);
}

void Page::SetIndexedDbSource(bindings::IndexedDbSource* indexed_db) {
  script_->SetIndexedDbSource(indexed_db);
}

void Page::SetCookieSource(bindings::CookieSource* cookies) {
  script_->SetCookieSource(cookies);
}

void Page::SetSocketSource(bindings::SocketSource* sockets) {
  script_->SetSocketSource(sockets);
}

std::string Page::RegisterBlobUrl(std::string body, std::string mime_type) {
  return blob_urls_.Register(std::move(body), std::move(mime_type));
}

void Page::RevokeBlobUrl(const std::string& url) { blob_urls_.Revoke(url); }

bool Page::DeliverSocketOpen(std::uint64_t id) { return script_->DeliverSocketOpen(id); }

bool Page::DeliverEventSourceOpen(std::uint64_t id) {
  return script_->DeliverEventSourceOpen(id);
}

bool Page::DeliverEventSourceMessage(std::uint64_t id, const std::string& type,
                                     const std::string& data, const std::string& last_id) {
  return script_->DeliverEventSourceMessage(id, type, data, last_id);
}

bool Page::DeliverEventSourceError(std::uint64_t id, bool permanent) {
  return script_->DeliverEventSourceError(id, permanent);
}

bool Page::DeliverSocketMessage(std::uint64_t id, const std::string& data, bool text) {
  return script_->DeliverSocketMessage(id, data, text);
}

bool Page::DeliverSocketClose(std::uint64_t id, std::uint16_t code, const std::string& reason,
                              bool clean, bool failed) {
  return script_->DeliverSocketClose(id, code, reason, clean, failed);
}

void Page::UpdateUrl(std::string url) {
  url_ = std::move(url);
  policy_.UpdateDocumentUrl(url_);
  // What a page reads back. One address, and it is the one the URL bar shows --
  // ADR 0026 §2's sentence, and the reason this is not two separate updates.
  script_->SetDocumentUrl(url_);
  // From the address rather than from the markup, and recomputed here so that
  // `#section` in a `pushState` URL styles the same element it would have styled
  // had the page been loaded at it. ADR 0016 §2: one copy.
  RefreshTargetState();
  // The cascade may now match differently, so whatever was derived from it is
  // stale. Not a full reload: the document is the same document, which is the
  // entire difference between this and Load.
  InvalidateLayout();
}

bool Page::DeliverFetchResponse(std::uint64_t id, const bindings::ScriptResponse& response) {
  return script_->DeliverFetchResponse(id, response);
}

bool Page::DeliverObservations(std::int64_t now_ms) {
  // The page's clock, published before anything a script can read it from runs.
  script_->TickClock(now_ms);
  if (document_ == nullptr) {
    return false;
  }
  // A loop, and the bound is the reason it is one. A `ResizeObserver` callback
  // that resizes what it observes is a page fighting itself: each delivery
  // makes the next one have something to say, and the specification's answer is
  // a depth limit rather than a promise that it settles. Without one this is a
  // hang a page can cause on purpose.
  //
  // The relayout inside the loop is what makes the second pass mean anything:
  // a callback that moved something must be measured against where it moved it
  // to, not against where it was.
  static constexpr int kObservationDepthLimit = 8;
  bool ran = false;
  int depth = 0;
  for (; depth < kObservationDepthLimit; ++depth) {
    EnsureLayoutClean();
    if (!script_->DeliverViewObservations(now_ms)) {
      break;
    }
    ran = true;
  }
  if (depth == kObservationDepthLimit) {
    AddPerformanceCounter(PerfCounterId::ViewResizeLoopLimit);
  }
  if (ran) {
    // Whatever the last callback did has to be on screen, and the caller paints
    // from the box tree rather than from the document.
    EnsureLayoutClean();
  }
  return ran;
}

void Page::InvalidateLayout() {
  InvalidateBoxTree();
  CollectImages();
}

bool Page::FocusFromClickAt(gfx::FloatPoint document_point) {
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  // The nearest focusable ancestor of what was hit, which is what makes a click
  // on the text inside a `<button>` focus the button rather than nothing. Null
  // when there is none, and that is not a failure: a click on the background
  // blurs whatever had focus, which is the only way to leave a field with the
  // mouse.
  //
  // Here rather than in PageEditing.cpp with the rest of the focus model
  // because this is the one part of it that is a *hit test*, and the hit-test
  // walk is in this file with the four others that use it.
  const dom::Element* target = ElementAt(document_point);
  while (target != nullptr && !html::IsFocusable(*target)) {
    target = ComposedParentElement(const_cast<dom::Element*>(target));
  }
  // Not keyboard-driven, so no focus ring: a ring on every click is the reason
  // authors write `outline: none`, which is worse for the user than either
  // behaviour. ADR 0017 §4.
  return MoveFocus(const_cast<dom::Element*>(target), false);
}

bool Page::ActivateCheckableInputAt(gfx::FloatPoint document_point) {
  EnsureLayoutClean();
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  dom::Element* hit =
      HitTestFormControlAt(*boxes_, document_point, html::IsCheckableInput, layout_.scroll_y);
  return hit != nullptr && ActivateCheckableInputOn(*hit);
}

bool Page::ActivateCheckableInputOn(dom::Element& input) {
  if (document_ == nullptr || !html::IsCheckableInput(input)) {
    return false;
  }
  if (html::IsCheckboxInput(input)) {
    if (input.HasAttribute("checked")) {
      input.RemoveAttribute("checked");
    } else {
      input.SetAttribute("checked", "");
    }
    InvalidateBoxTree();
    return true;
  }
  if (input.HasAttribute("checked")) {
    return false;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& candidate = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (IsRadioGroupPeer(candidate, input, *document_)) {
      candidate.RemoveAttribute("checked");
    }
  });
  input.SetAttribute("checked", "");
  InvalidateBoxTree();
  return true;
}

bool Page::ResetFormAt(gfx::FloatPoint document_point) {
  EnsureLayoutClean();
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  const dom::Element* reset =
      HitTestFormControlAt(*boxes_, document_point, html::IsResetControl, layout_.scroll_y);
  return reset != nullptr && ResetFormOn(*reset);
}

bool Page::ResetFormOn(const dom::Element& reset) {
  if (document_ == nullptr || !html::IsResetControl(reset)) {
    return false;
  }
  const dom::Element* form = html::FormOwner(reset, *document_);
  if (form == nullptr) {
    return false;
  }
  bool changed = false;
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (!html::BelongsToForm(element, *form, *document_)) {
      return;
    }
    if (element.TagName() != "input" && element.TagName() != "textarea") {
      return;
    }
    const auto found = control_defaults_.find(&element);
    if (found == control_defaults_.end()) {
      return;
    }
    const auto& [value, checked] = found->second;
    if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
      if (checked) {
        changed = !element.HasAttribute("checked") || changed;
        element.SetAttribute("checked", "");
      } else {
        changed = element.RemoveAttribute("checked") || changed;
      }
    } else if (IsValueResettableControl(element)) {
      const std::string* current = element.GetAttribute("value");
      changed = current == nullptr || *current != value || changed;
      element.SetAttribute("value", value);
    }
  });
  if (!changed) {
    return false;
  }
  InvalidateBoxTree();
  return true;
}

}  // namespace microbrowser::engine
