#include "ui/NavigationHistory.h"

#include <utility>

namespace microbrowser::ui {

void NavigationHistory::Push(std::string url, std::string title) {
  if (!entries_.empty()) {
    // Everything after the current entry is a branch the user left. Truncating
    // is what makes forward stop working after you take a different path, and
    // pretending the branch is still reachable is worse than losing it.
    entries_.resize(index_ + 1);
  }
  entries_.push_back(Entry{std::move(url), std::move(title)});
  index_ = entries_.size() - 1;
}

void NavigationHistory::SetCurrentTitle(std::string title) {
  if (entries_.empty()) {
    return;
  }
  entries_[index_].title = std::move(title);
}

const NavigationHistory::Entry* NavigationHistory::GoBack() {
  if (!CanGoBack()) {
    return nullptr;
  }
  --index_;
  return &entries_[index_];
}

const NavigationHistory::Entry* NavigationHistory::GoForward() {
  if (!CanGoForward()) {
    return nullptr;
  }
  ++index_;
  return &entries_[index_];
}

const NavigationHistory::Entry* NavigationHistory::Current() const {
  return entries_.empty() ? nullptr : &entries_[index_];
}

void NavigationHistory::Clear() {
  entries_.clear();
  index_ = 0;
}

}  // namespace microbrowser::ui
