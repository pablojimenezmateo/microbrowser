#include "ui/TextField.h"

#include <algorithm>
#include <utility>

namespace microbrowser::ui {

namespace {

// A UTF-8 continuation byte, which is never a codepoint boundary. Stepping the
// caret by one byte through a multi-byte character would split it and produce
// text that is no longer UTF-8.
bool IsContinuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Appends `codepoint` as UTF-8.
void AppendUtf8(std::string& out, char32_t codepoint) {
  if (codepoint < 0x80) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

}  // namespace

void TextField::SetText(std::string text) {
  text_ = std::move(text);
  // The caret goes to the end, which is where it is wanted after the omnibox
  // is filled with the current URL. Selecting all is the caller's choice, not
  // this one's.
  caret_ = text_.size();
  anchor_ = caret_;
}

std::string_view TextField::SelectedText() const {
  return std::string_view(text_).substr(SelectionBegin(), SelectionEnd() - SelectionBegin());
}

std::size_t TextField::NextBoundary(std::size_t at) const {
  std::size_t next = std::min(at + 1, text_.size());
  while (next < text_.size() && IsContinuation(text_[next])) {
    ++next;
  }
  return next;
}

std::size_t TextField::PreviousBoundary(std::size_t at) const {
  if (at == 0) {
    return 0;
  }
  std::size_t previous = at - 1;
  while (previous > 0 && IsContinuation(text_[previous])) {
    --previous;
  }
  return previous;
}

bool TextField::DeleteSelection() {
  if (!HasSelection()) {
    return false;
  }
  const std::size_t begin = SelectionBegin();
  text_.erase(begin, SelectionEnd() - begin);
  caret_ = begin;
  anchor_ = begin;
  return true;
}

void TextField::InsertCodepoint(char32_t codepoint) {
  DeleteSelection();
  std::string encoded;
  AppendUtf8(encoded, codepoint);
  text_.insert(caret_, encoded);
  caret_ += encoded.size();
  anchor_ = caret_;
}

void TextField::DeleteBackward() {
  // A selection is what gets deleted. Deleting it *and* the character before it
  // is the classic double-delete.
  if (DeleteSelection()) {
    return;
  }
  if (caret_ == 0) {
    return;
  }
  const std::size_t previous = PreviousBoundary(caret_);
  text_.erase(previous, caret_ - previous);
  caret_ = previous;
  anchor_ = caret_;
}

void TextField::DeleteForward() {
  if (DeleteSelection()) {
    return;
  }
  if (caret_ >= text_.size()) {
    return;
  }
  const std::size_t next = NextBoundary(caret_);
  text_.erase(caret_, next - caret_);
  anchor_ = caret_;
}

void TextField::MoveLeft(bool select) {
  if (!select && HasSelection()) {
    // Collapses to the near edge rather than moving: pressing Left with text
    // selected puts the caret at the start of it, which is what every editor
    // does and what nobody notices until it is wrong.
    caret_ = SelectionBegin();
    anchor_ = caret_;
    return;
  }
  caret_ = PreviousBoundary(caret_);
  if (!select) {
    anchor_ = caret_;
  }
}

void TextField::MoveRight(bool select) {
  if (!select && HasSelection()) {
    caret_ = SelectionEnd();
    anchor_ = caret_;
    return;
  }
  caret_ = NextBoundary(caret_);
  if (!select) {
    anchor_ = caret_;
  }
}

void TextField::MoveToStart(bool select) {
  caret_ = 0;
  if (!select) {
    anchor_ = caret_;
  }
}

void TextField::MoveToEnd(bool select) {
  caret_ = text_.size();
  if (!select) {
    anchor_ = caret_;
  }
}

void TextField::SelectAll() {
  anchor_ = 0;
  caret_ = text_.size();
}

void TextField::ClearSelection() { anchor_ = caret_; }

bool TextField::HandleKey(const platform::KeyEvent& event) {
  if (!event.pressed) {
    return false;
  }
  const platform::Modifiers& modifiers = event.modifiers;

  if (modifiers.control && event.key == platform::Key::None) {
    switch (event.codepoint) {
      case U'a':
      case U'A':
        SelectAll();
        return true;
      case U'u':
      case U'U':
        // Clear the line, as in a terminal. The omnibox is a place people type
        // URLs, and the habit comes with them.
        text_.clear();
        caret_ = 0;
        anchor_ = 0;
        return true;
      default:
        return false;
    }
  }

  switch (event.key) {
    case platform::Key::Backspace:
      DeleteBackward();
      return true;
    case platform::Key::Delete:
      DeleteForward();
      return true;
    case platform::Key::Left:
      MoveLeft(modifiers.shift);
      return true;
    case platform::Key::Right:
      MoveRight(modifiers.shift);
      return true;
    case platform::Key::Home:
      MoveToStart(modifiers.shift);
      return true;
    case platform::Key::End:
      MoveToEnd(modifiers.shift);
      return true;
    default:
      break;
  }

  // Typing. A control or alt chord is a shortcut belonging to whoever owns the
  // field, not a character -- otherwise ctrl+R types an 'r' into the URL.
  if (event.key == platform::Key::None && event.codepoint >= 0x20 && event.codepoint != 0x7F &&
      modifiers.PlainTyping()) {
    InsertCodepoint(event.codepoint);
    return true;
  }
  return false;
}

}  // namespace microbrowser::ui
