#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/Loader.h"
#include "engine/Page.h"
#include "engine/PendingLoad.h"
#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ipc/Message.h"
#include "ipc/Transport.h"
#include "util/WaitDescriptor.h"

namespace microbrowser::engine {

// The engine half of the seam.
//
// It loads a URL, parses it into a document, resolves its styles, lays it out,
// and paints it into a display list. The properties that matter are structural:
//
//   * It talks to the outside world only through ipc::EngineEndpoint. It has no
//     window handle, no renderer, no canvas, and no way to acquire one.
//   * It is driven, never driving. HandlePendingMessages() and Advance() run to
//     completion and return; neither owns a loop or a thread. A future process
//     split gives it its own loop without changing anything above.
//   * Painting is producing a display list. It never touches a pixel.
//
// Since ADR 0011 a navigation *starts* rather than happens: `Navigate` sends the
// document request and returns, and the load moves forward one turn at a time
// through `Advance()`. What that buys is a browser whose loop is not blocked for
// the length of a fetch, and what it costs is the state below -- which a
// navigation has to be able to throw away, because a response for a document
// that is gone must be dropped.
//
// The temptation this class must resist for the next year is becoming the place
// where "the browser" lives. Document, navigation history, network, and script
// each get their own type; Engine stays the thing that routes messages to them.
// Its budget in src/engine/MODULE.deps is the tripwire.
class Engine {
 public:
  // Fonts arrive from the caller because which fonts exist is a property of
  // the machine, and the engine is the half of the seam that does not know
  // what machine it is on. That is the same reason it has no window.
  Engine(ipc::EngineEndpoint& endpoint, gfx::FontProvider& fonts);

  // Drain and act on everything the UI has queued. Returns true when the engine
  // produced any outgoing message, which is what tells the host loop a repaint
  // may be pending.
  bool HandlePendingMessages();

  // Carries the load in flight as far as it can go without blocking, and acts
  // on whatever arrived. True when anything happened, which is the host loop's
  // signal that there may be a frame to show.
  bool Advance();

  // Milliseconds until the engine's soonest deadline: a page timer, an
  // animation frame, or the point at which a silent server is given up on.
  // Nothing when it has none -- which is the answer that lets the loop block.
  std::optional<std::uint32_t> NextDeadlineMs() const;
  // Runs every timer that is due and the animation frame if its boundary has
  // arrived, and repaints when one changed the page. True when anything ran.
  bool RunDueWork();

  // What the loop's single blocking wait must watch for this engine to make
  // progress. Appends rather than assigns, because the loop waits on more than
  // one source.
  void AppendWaitDescriptors(util::WaitDescriptorList& out) const;
  // True when something can move with no wait at all. A socket is almost never
  // in this state and a canned transport always is; without the question the
  // loop would block on input while a test's load stood still.
  bool HasRunnableWork() const;
  // True while a navigation has not finished. The snapshot tool and the tests
  // drive the loop until this goes false.
  bool IsLoading() const { return load_.active; }

  // What the page's script threw, so a host that is debugging one can say why
  // a document rendered the way it did. Forwarded rather than exposing the
  // Page, which would put the whole engine on the wrong side of the seam.
  const std::vector<std::string>& ScriptErrors() const { return page_.ScriptErrors(); }

  const std::string& Title() const { return page_.Title(); }
  const std::string& Url() const { return page_.Url(); }
  gfx::IntSize ViewportSize() const { return viewport_size_; }

  // The loader, so a caller can install a transport or adjust privacy settings
  // before the first navigation. Tests serve canned bytes through it; there is
  // no other way to exercise a navigation without a network.
  Loader& PageLoader() { return loader_; }

 private:
  void Navigate(const std::string& url);
  void Navigate(const std::string& url, const net::FetchOptions& options);
  void Navigate(const std::string& url, const net::FetchOptions& options,
                const url::Url* referrer_document);
  void NavigateFromCurrentDocument(const std::string& url, const net::FetchOptions& options);
  bool Navigate(const FormSubmission& submission);
  // Acts on the submission a script asked for, if it asked for one. Called
  // after every point where a page's script can have run -- its scripts, a
  // timer, an animation frame, a click handler -- because a navigation started
  // from inside one would tear down the interpreter running it. True when a
  // navigation started, which also means `load_` is now a different load.
  bool FollowScriptNavigation();
  void SetViewport(const gfx::IntSize& size, float device_scale);
  void ScrollBy(int delta_x, int delta_y);
  // Where the viewport sits over the document. Kept on the Page rather than
  // here, because painting and a script's `getBoundingClientRect` both have to
  // subtract it and two copies of a scroll offset drift. See Page and ADR 0015.
  int ScrollY() const;
  bool HandlePointer(const ipc::PointerMessage& pointer);

  // Acts on one thing that arrived.
  void OnCompletion(Loader::Completion completion);
  void OnDocument(Loader::Result result);
  // Starts every subresource the parsed document referenced, all at once.
  // Concurrency is bounded per partition key inside the request queue, which is
  // where that bound belongs -- see ADR 0005 for why it is per key.
  void StartSubresources();
  // Runs the scripts once every render-blocking resource has resolved, puts
  // the page on screen, and lets the navigation go once even the scripts the
  // page said it would not wait for have landed.
  void AdvanceLoad();
  // Decodes the images and sends the first frame of this document.
  void Paint();
  void DecodePendingImages();

  // Lays out at the current viewport width, then paints. Separate from
  // PaintAndSend because scrolling repaints without relaying out, and a
  // scroll that ran layout would be the classic reason scrolling is slow.
  void LayoutAndPaint();

  // Rebuild the display list from current state and send it with full-viewport
  // damage. Incremental damage arrives with the paint system in M6; reporting
  // the truth (everything changed) is the correct placeholder, and is why the
  // damage field is not simply omitted.
  void PaintAndSend();

  // Renders `message` as the page, for a load that failed. A blank window is
  // indistinguishable from a hung browser.
  void ShowError(std::string_view url, std::string_view message);

  // Clamped so that scrolling stops at the end of the document rather than
  // running off into blank space.
  int MaxScroll() const;

  ipc::EngineEndpoint& endpoint_;
  Loader loader_;
  Page page_;
  // The frame most recently sent, kept so the next one can be diffed against
  // it. This is what the display list being a comparable value buys: damage is
  // computed from two frames rather than trusted from every call site that
  // invalidated something.
  gfx::DisplayList display_list_;
  // Reused rather than reallocated per frame; painting is the hot path.
  gfx::DisplayList pending_;
  gfx::IntSize viewport_size_;
  float device_scale_ = 1.0f;
  PendingLoad load_;
};

}  // namespace microbrowser::engine
