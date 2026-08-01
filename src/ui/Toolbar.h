#pragma once

#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"
#include "ui/TextField.h"

namespace microbrowser::ui {

// The browser chrome above the page: back, forward, reload, and the omnibox.
//
// Produces a display list like everything else that draws. That is not
// uniformity for its own sake — it means the chrome is damage-tracked by the
// same diff as the page, testable without a window, and incapable of drawing
// outside the rect it was given.
//
// It knows nothing about the engine. Clicking back sets a flag; who acts on it
// is BrowserChrome's problem, and keeping that out of here is what stops the
// toolbar from becoming the place navigation lives.
class Toolbar {
 public:
  // Where a point landed. `None` means the toolbar, but nothing in it.
  enum class Part : std::uint8_t { Outside, None, Back, Forward, Reload, Omnibox };

  static constexpr int kHeight = 36;

  // Lays the parts out for a toolbar `width` wide. Called on resize; the rects
  // are then answers rather than computations repeated per click and per paint.
  void SetWidth(int width);
  int Width() const { return width_; }
  gfx::IntRect Bounds() const { return gfx::IntRect{0, 0, width_, kHeight}; }

  Part HitTest(gfx::IntPoint point) const;

  // Where the caret and the selection edges fall, in pixels from the start of
  // the omnibox text.
  //
  // Measured by the caller rather than here: this module has no font stack and
  // must not grow one. Three numbers rather than one, because a partial
  // selection needs both its edges and the caret is not always at either --
  // guessing the third from the other two is how a selection ends up drawn the
  // width of the whole field.
  struct OmniboxMetrics {
    float caret = 0.0f;
    float selection_begin = 0.0f;
    float selection_end = 0.0f;
  };

  // Records the chrome into `out`.
  void Paint(gfx::DisplayList& out, const OmniboxMetrics& metrics) const;

  TextField& Omnibox() { return omnibox_; }
  const TextField& Omnibox() const { return omnibox_; }

  bool IsOmniboxFocused() const { return omnibox_focused_; }
  void SetOmniboxFocused(bool focused) { omnibox_focused_ = focused; }

  void SetCanGoBack(bool can) { can_go_back_ = can; }
  void SetCanGoForward(bool can) { can_go_forward_ = can; }
  bool CanGoBack() const { return can_go_back_; }
  bool CanGoForward() const { return can_go_forward_; }

  // The rect the omnibox text is drawn inside, for measuring and for the caret.
  gfx::IntRect OmniboxTextRect() const;

  // Font the omnibox draws with. Exposed so the caller can measure the same
  // text with the same font -- measuring with a different one puts the caret in
  // the wrong place, which is the whole class of bug this avoids.
  static gfx::FontRequest OmniboxFont();

 private:
  gfx::IntRect back_{};
  gfx::IntRect forward_{};
  gfx::IntRect reload_{};
  gfx::IntRect omnibox_rect_{};
  int width_ = 0;

  TextField omnibox_;
  bool omnibox_focused_ = false;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
};

}  // namespace microbrowser::ui
