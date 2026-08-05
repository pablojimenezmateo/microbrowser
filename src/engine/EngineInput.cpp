#include "engine/Engine.h"

#include <optional>
#include <string>

#include "engine/LinkResolution.h"

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

bool Engine::HandlePointer(const ipc::PointerInputMessage& pointer) {
  // The primary button, going down. `button` is the DOM's numbering, where the
  // primary button is zero.
  if (pointer.kind != ipc::PointerInputMessage::Kind::Down || pointer.button != 0) {
    return false;
  }
  // Already in CSS pixels: the host divided the device scale out at the seam,
  // because that is the coordinate system every answer given back about this
  // point is in. Only the scroll offset is added, which is what turns a
  // viewport coordinate into a document one.
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
  // The page's own handlers run first, and a `preventDefault` stops everything
  // below. That ordering is the whole contract of the method: a script that
  // intercepts a click on a link expects the link not to be followed, and
  // deciding to navigate before asking would make `preventDefault` a lie.
  const DispatchOutcome click = page_.DispatchClickAt(document_point, input);
  // Before the default action, and before `preventDefault` is consulted: a
  // handler that submitted a form asked for a navigation of its own, and that
  // is what happens whether or not it also stopped the click.
  if (FollowScriptNavigation()) {
    return true;
  }
  if (click.prevented) {
    page_.InvalidateLayout();
    LayoutAndPaint();
    return true;
  }
  if (const std::optional<FormSubmission> submission =
          page_.FormSubmissionRequestAt(document_point)) {
    return Navigate(*submission);
  }
  if (page_.ResetFormAt(document_point)) {
    LayoutAndPaint();
    return true;
  }
  if (page_.ActivateCheckableInputAt(document_point)) {
    LayoutAndPaint();
    return true;
  }
  if (page_.FocusTextControlAt(document_point)) {
    return false;
  }
  const std::optional<std::string> href = page_.LinkAt(document_point);
  if (!href.has_value()) {
    // Nothing to navigate to, but a handler may still have changed the
    // document -- which is the case that used to run the handler and leave the
    // screen alone.
    if (click.ran) {
      page_.InvalidateLayout();
      LayoutAndPaint();
      return true;
    }
    return false;
  }
  const std::optional<std::string> resolved = ResolveLink(*href, page_.Url());
  if (!resolved.has_value()) {
    return false;
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
  // "Delete" has no default action yet: the caret model is end-of-text only, so
  // there is no forward character to remove. Named here rather than left out,
  // because the absence is a caret limitation and not a decision about the key.
  return HandleScriptSideEffects(dispatched.ran);
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

}  // namespace microbrowser::engine
