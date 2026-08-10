#include "engine/Engine.h"

#include <cmath>
#include <optional>
#include <string>

#include "engine/LinkResolution.h"
#include "util/PerformanceCounters.h"

// The two things a user's hands do, and what the engine does about them.
//
// Split from Engine.cpp because that file reached the module's line cap, and
// the cap means a missing translation unit rather than a bigger file. The split
// falls where ADR 0017 draws its own line: everything here is **dispatch first,
// default action second**. A click runs the page's handlers and only then
// follows the link; a key runs them and only then inserts the character. Both
// halves of both are in one file so that the ordering is one thing to read
// rather than two to keep in step.

namespace microbrowser::engine {

namespace {

// How far the arrow and page keys scroll, in CSS pixels. The same numbers the
// browser chrome used before this moved, so what a key does has not changed --
// only who decides whether it happens.
constexpr int kPixelsPerArrowKey = 40;
constexpr int kPixelsPerPage = 320;

}  // namespace

css::StyleChangeEffect Engine::UpdatePointerState(const ipc::PointerInputMessage& pointer) {
  // What the pointer's *position* means to the cascade, which is a different
  // question from what a click does and is asked on every event including the
  // moves nothing else cares about.
  //
  // The order matters: Page asks the invalidation index before it hit-tests, so
  // on a page whose stylesheet never mentions `:hover` this call reaches an
  // early return and costs a bitmask test. That is ADR 0016 §3's headline
  // property, and it is the one that decays silently -- a browser that restyles
  // on every mouse move is indistinguishable from one that does not until
  // somebody measures the idle cost of moving a mouse across a window.
  const gfx::FloatPoint document_point{pointer.position.x,
                                       pointer.position.y + static_cast<float>(ScrollY())};
  const bool held = pointer.kind != ipc::PointerInputMessage::Kind::Up && (pointer.buttons & 1) != 0;
  const dom::ElementState changed = page_.UpdateHoverChain(document_point, held);
  if (!Any(changed)) {
    return css::StyleChangeEffect::None;
  }
  return page_.StateChangeEffect(changed);
}

bool Engine::ApplyStyleChange(css::StyleChangeEffect effect) {
  switch (effect) {
    case css::StyleChangeEffect::None:
      return false;
    case css::StyleChangeEffect::Paint:
      // The cascade is re-resolved over the box tree that is already laid out.
      // Every rule keyed on what changed affects paint alone -- that is what
      // the index said -- so the geometry is still correct and the damage comes
      // out of the display-list diff as the rectangles that actually changed.
      page_.RestyleWithoutLayout();
      PaintAndSend();
      return true;
    case css::StyleChangeEffect::Layout:
      page_.InvalidateLayout();
      LayoutAndPaint();
      return true;
  }
  return false;
}

bool Engine::HandlePointer(const ipc::PointerInputMessage& pointer) {
  // Hover and active first, and for every kind of pointer event: a button going
  // down is also a pointer being somewhere, and a page that styles `:active`
  // expects the state to be right before the click is routed against it.
  //
  // Applied afterwards rather than here, because a click that ends in a layout
  // and a paint has already done everything a restyle would ask for -- and
  // painting twice for one click is a frame the user sees flicker.
  const css::StyleChangeEffect effect = UpdatePointerState(pointer);
  if (pointer.button != 0) {
    return ApplyStyleChange(effect);
  }
  const gfx::FloatPoint document_point{pointer.position.x,
                                       pointer.position.y + static_cast<float>(ScrollY())};
  bindings::PointerInput input;
  input.client_x = pointer.position.x;
  input.client_y = pointer.position.y;
  input.page_x = document_point.x;
  input.page_y = document_point.y;
  input.button = pointer.button;
  input.buttons = pointer.buttons;
  input.control = pointer.modifiers.control;
  input.shift = pointer.modifiers.shift;
  input.alt = pointer.modifiers.alt;
  input.meta = pointer.modifiers.meta;

  if (pointer.kind == ipc::PointerInputMessage::Kind::Down) {
    const bool pressed = page_.DispatchPointerDownAt(document_point, input);
    // :active / :hover through the invalidation index only. The pointerdown/up
    // split used to InvalidateLayout+LayoutAndPaint on every press, which rebuilt
    // youtube's result list under the finger; click then raced a restamping tree
    // and SPA `preventDefault` lost to href activation (TD-0047). Paint after the
    // gesture completes (release / prevented path below).
    return ApplyStyleChange(effect) || pressed;
  }
  if (pointer.kind != ipc::PointerInputMessage::Kind::Up) {
    return ApplyStyleChange(effect);
  }

  // Release: `pointerup`, `mouseup`, `click`, then default actions. Focus moved
  // on the corresponding down; the specification runs activation there.
  const DispatchOutcome click = page_.DispatchPointerReleaseAt(document_point, input);
  if (FollowScriptNavigation()) {
    return true;
  }
  if (click.prevented) {
    util::AddPerformanceCounter(util::PerfCounterId::InputClickPrevented);
    page_.InvalidateLayout();
    LayoutAndPaint();
    return true;
  }
  const ClickActivation activation = page_.ResolveClickActivation(click.click_target);
  if (activation.form.has_value()) {
    return Navigate(*activation.form);
  }
  if (activation.reset_form || activation.toggled_checkable || activation.toggled_media) {
    LayoutAndPaint();
    return true;
  }
  if (!activation.href.has_value()) {
    if (click.ran) {
      page_.InvalidateLayout();
      LayoutAndPaint();
      return true;
    }
    return ApplyStyleChange(effect);
  }
  util::AddPerformanceCounter(util::PerfCounterId::InputClickDefaultHref);
  const std::optional<std::string> resolved = ResolveLink(*activation.href, page_.Url());
  if (!resolved.has_value()) {
    return ApplyStyleChange(effect);
  }
  if (NavigateToFragment(*resolved)) {
    return true;
  }
  NavigateFromCurrentDocument(*resolved, {});
  return true;
}

bool Engine::HandleKey(const ipc::KeyInputMessage& key) {
  bindings::KeyInput input;
  input.down = key.kind == ipc::KeyInputMessage::Kind::Down;
  input.code = key.code;
  input.key = key.key;
  input.text = key.text;
  input.control = key.modifiers.control;
  input.shift = key.modifiers.shift;
  input.alt = key.modifiers.alt;
  input.meta = key.modifiers.meta;
  input.repeat = key.repeat;

  // Dispatch, then the default action, and never the other way round. ADR 0017
  // §2: a `preventDefault` on a keydown has to stop the character being
  // inserted, and it can only do that if nothing has inserted it yet.
  const DispatchOutcome dispatched = page_.DispatchKeyToFocus(input);
  // A handler may have submitted a form, and that happens whether or not it
  // also cancelled the key -- the same rule a click already followed.
  if (FollowScriptNavigation()) {
    return true;
  }
  if (!input.down || dispatched.prevented) {
    // A `keyup` has no default action here -- everything below is what a
    // *press* does, and running it on the release would do it twice -- and a
    // cancelled keydown has had its default action taken away.
    return HandleScriptSideEffects(dispatched.ran);
  }

  // The default actions, in the order the specification runs them: a key that
  // inserts text inserts it, and a named key does what it names.
  if (!input.text.empty() && page_.InsertTextIntoFocusedTextControl(input.text)) {
    LayoutAndPaint();
    return true;
  }
  if (input.key == "Backspace" && page_.DeleteBackwardFromFocusedTextControl()) {
    LayoutAndPaint();
    return true;
  }
  if (input.key == "Enter") {
    if (const std::optional<FormSubmission> submission = page_.FocusedFormSubmission()) {
      return Navigate(*submission);
    }
  }
  if (input.key == "Tab" && page_.MoveFocusByTab(input.shift)) {
    // A default action, which is what makes `preventDefault` on a Tab work --
    // and a page that manages its own roving focus does exactly that.
    page_.InvalidateLayout();
    LayoutAndPaint();
    return true;
  }
  if (ScrollByKey(input)) {
    return true;
  }
  // "Delete" has no default action yet: the caret model is end-of-text only, so
  // there is no forward character to remove. Named here rather than left out,
  // because the absence is a caret limitation and not a decision about the key.
  return HandleScriptSideEffects(dispatched.ran);
}

bool Engine::ScrollByKey(const bindings::KeyInput& key) {
  // The arrow and page keys, as a *default action of a keydown* rather than as
  // something the browser chrome did before the page ever saw the key.
  //
  // It used to be the chrome's: `ui::BrowserChrome` turned Down and PageDown
  // into a scroll intent and reported the key handled, so a page with an
  // ArrowDown handler never saw one and `preventDefault` on it meant nothing.
  // ADR 0017 §2 is explicit that a scroll is a default action and that
  // cancelling it is what `preventDefault` on the key is for -- and ADR 0017 §4
  // leaves `src/app` deciding *whose* key it is, which is not the same question
  // as what the key does.
  int css_delta = 0;
  if (key.key == "ArrowDown") {
    css_delta = kPixelsPerArrowKey;
  } else if (key.key == "ArrowUp") {
    css_delta = -kPixelsPerArrowKey;
  } else if (key.key == "PageDown") {
    css_delta = kPixelsPerPage;
  } else if (key.key == "PageUp") {
    css_delta = -kPixelsPerPage;
  } else {
    return false;
  }
  // Multiplied back up because ScrollMessage is in device pixels and ScrollBy
  // divides the scale out again. Stated here rather than passed through in CSS
  // pixels so that there is one place a scroll delta changes units, which is
  // the seam itself.
  const double scale = device_scale_ > 0.0f ? static_cast<double>(device_scale_) : 1.0;
  const int device_delta = static_cast<int>(std::lround(static_cast<double>(css_delta) * scale));
  // At the top-left of the viewport, which routes it to the document unless
  // something scrollable is under that corner -- the same position the chrome
  // sent, so what a page key scrolls has not changed.
  ScrollBy(ipc::ScrollMessage{0, device_delta, gfx::IntPoint{}});
  return true;
}

std::string Engine::FocusDescription() const {
  const dom::Element* focused = page_.FocusedElement();
  if (focused == nullptr) {
    return "none";
  }
  std::string text{focused->TagName()};
  // Whichever of the two the page gave it. `id` first because that is what a
  // check names an element by; `name` because a form control often has only
  // that, and a search field with neither is the case worth telling apart from
  // no focus at all.
  if (const std::string* id = focused->GetAttribute("id"); id != nullptr && !id->empty()) {
    text += "#";
    text += *id;
  } else if (const std::string* name = focused->GetAttribute("name");
             name != nullptr && !name->empty()) {
    text += "[name=";
    text += *name;
    text += "]";
  } else if (const std::string* href = focused->GetAttribute("href");
             href != nullptr && !href->empty()) {
    // A link, which on a real page is most of what Tab stops on and which
    // usually has neither an id nor a name. Without this, walking a page's tab
    // order printed `a` at every step and said nothing about whether focus had
    // moved at all.
    text += "[href=";
    text += *href;
    text += "]";
  }
  text += page_.FocusIsVisible() ? " keyboard" : " pointer";
  return text;
}

bool Engine::HandleScriptSideEffects(bool ran) {
  // A handler ran and the document may have moved under the layout. Nothing
  // here knows what it changed, so the layout is dropped and rebuilt -- and
  // only when something actually ran, because a key on a static page must not
  // cost a relayout.
  if (!ran) {
    return false;
  }
  page_.InvalidateLayout();
  LayoutAndPaint();
  return true;
}

// The wheel, and the damage a scroll produces.
//
// Moved out of Engine.cpp when that file reached its module cap, and it belongs
// here rather than in a third file: a wheel is an input, it arrives as an input
// message, and ADR 0018's whole argument is that what it produces is a *paint*
// rather than a layout. The two functions are one decision -- where the delta
// goes, and what that costs -- split only because the second is asked again by
// the frame that honours a blit.

void Engine::ScrollBy(const ipc::ScrollMessage& scroll) {
  const float scale = device_scale_ > 0.0f ? device_scale_ : 1.0f;
  // Where the wheel is, in the document's coordinates -- which is what routing
  // needs, because a box's geometry is where the flow put it and the pointer is
  // somewhere over the scrolled result.
  const gfx::FloatPoint document_point{
      static_cast<float>(scroll.position.x) / scale,
      static_cast<float>(scroll.position.y) / scale + static_cast<float>(ScrollY())};
  const gfx::FloatPoint delta{static_cast<float>(scroll.delta_x) / scale,
                              static_cast<float>(scroll.delta_y) / scale};

  // The deepest scrolling box under the pointer that can still move takes it;
  // when none can, the document does. ADR 0018 §4, and the case that makes it
  // worth writing down is a menu at its end: the wheel goes on rather than
  // stopping dead.
  const Page::ScrollOutcome outcome = page_.ScrollAt(document_point, delta);
  if (outcome.moved) {
    // Only the scroller's own rectangle changed, so this frame is not a
    // document blit -- it is an ordinary partial repaint, and the display-list
    // diff would find it anyway. Reported explicitly because the diff cannot
    // bound it: every command inside the box moved.
    PaintAndSend(gfx::IntPoint{}, &outcome.damage);
    return;
  }
  if (!outcome.viewport) {
    return;
  }

  const int previous = ScrollY();
  page_.SetScrollOffsetY(
      static_cast<float>(std::clamp(previous + scroll.delta_y, 0, MaxScroll())));
  const int moved = ScrollY() - previous;
  if (moved == 0) {
    return;
  }
  // Paints without laying out. The geometry has not changed, and a scroll that
  // relaid out is the classic reason scrolling is slow. The delta says how far
  // the previous frame's pixels moved, which is what lets the UI blit the
  // overlap and repaint only the strip that came into view.
  PaintAndSend(gfx::IntPoint{0, -moved}, nullptr);
}

// The band a scroll of `delta` newly exposes, plus everything that did not move
// with it.
//
// Two exceptions and both are known in advance rather than discovered: a
// `fixed` box does not move at all, and a `sticky` one moves sometimes. ADR
// 0018 §2 names them as the two things that break a blit and says to subtract
// them from it -- and over-reporting here is safe where under-reporting is not,
// because damage that is too small leaves a stale rectangle on screen forever.
std::vector<gfx::IntRect> Engine::ScrollDamage(gfx::IntPoint delta) const {
  std::vector<gfx::IntRect> damage;
  const int width = viewport_size_.width;
  const int height = viewport_size_.height;
  if (delta.y > 0) {
    damage.push_back(gfx::IntRect{0, 0, width, std::min(delta.y, height)});
  } else if (delta.y < 0) {
    const int band = std::min(-delta.y, height);
    damage.push_back(gfx::IntRect{0, height - band, width, band});
  }
  const std::size_t band = damage.size();
  page_.AppendScrollInvariantRects(damage);
  // Clipped to the viewport, and only the boxes are: a sticky box's damage is
  // the strip between where the flow put it and where it sticks, and most of
  // that strip is usually off screen. Reporting it unclipped is what turned a
  // 40-pixel header on a 900-pixel window into 38% of the surface -- true, and
  // useless, because damage outside the window costs a repaint of nothing.
  std::vector<gfx::IntRect> clipped(damage.begin(), damage.begin() + static_cast<long>(band));
  const gfx::IntRect viewport{0, 0, viewport_size_.width, viewport_size_.height};
  for (std::size_t i = band; i < damage.size(); ++i) {
    const gfx::IntRect visible = damage[i].Intersected(viewport);
    if (!visible.IsEmpty()) {
      clipped.push_back(visible);
    }
  }
  return clipped;
}

}  // namespace microbrowser::engine
