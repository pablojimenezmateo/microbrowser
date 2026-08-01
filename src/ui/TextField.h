#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "platform/InputEvent.h"

namespace microbrowser::ui {

// An editable line of text: the omnibox, and later a form field.
//
// Pure state and pure transitions — no painting, no font, no window. That is
// what makes "does ctrl+A then typing replace everything" a unit test rather
// than something someone tries by hand after every change. Editing is where
// off-by-one errors live, and they are invisible until a user hits exactly the
// wrong key at exactly the wrong caret position.
//
// A caret and an anchor rather than a caret and a length: a selection has a
// direction, and shift+Left from a right-to-left drag has to shrink the
// selection rather than move it. Offsets are byte offsets into UTF-8, always at
// a codepoint boundary — see MoveLeft, which steps over continuation bytes.
class TextField {
 public:
  void SetText(std::string text);
  const std::string& Text() const { return text_; }

  std::size_t Caret() const { return caret_; }
  std::size_t Anchor() const { return anchor_; }
  bool HasSelection() const { return caret_ != anchor_; }
  std::size_t SelectionBegin() const { return caret_ < anchor_ ? caret_ : anchor_; }
  std::size_t SelectionEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }
  std::string_view SelectedText() const;

  // True when the field consumed the event. A field that returns false has not
  // changed, and the caller may treat the key as its own -- which is how Escape
  // both leaves the omnibox and, when the omnibox is not focused, does nothing.
  bool HandleKey(const platform::KeyEvent& event);

  void InsertCodepoint(char32_t codepoint);
  void DeleteBackward();
  void DeleteForward();
  void MoveLeft(bool select);
  void MoveRight(bool select);
  void MoveToStart(bool select);
  void MoveToEnd(bool select);
  void SelectAll();
  void ClearSelection();

 private:
  // Deletes the selection if there is one. Returns whether it deleted
  // anything, because "backspace with a selection" deletes the selection and
  // stops rather than also deleting the character before it.
  bool DeleteSelection();

  std::size_t NextBoundary(std::size_t at) const;
  std::size_t PreviousBoundary(std::size_t at) const;

  std::string text_;
  std::size_t caret_ = 0;
  std::size_t anchor_ = 0;
};

}  // namespace microbrowser::ui
