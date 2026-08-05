#include "engine/SessionHistory.h"

#include <utility>

namespace microbrowser::engine {

namespace {

// How many entries one tab keeps. A page can call `pushState` in a loop, and an
// unbounded list is memory a page controls -- which is the same argument the
// preflight cache and the violation log make. The oldest entries go, because the
// back button is used from the near end.
constexpr std::size_t kMaxEntries = 200;

}  // namespace

void SessionHistory::PushDocument(std::string url, std::uint64_t document) {
  entries_.resize(entries_.empty() ? 0 : index_ + 1);
  HistoryEntry entry;
  entry.url = std::move(url);
  entry.document = document;
  entries_.push_back(std::move(entry));
  if (entries_.size() > kMaxEntries) {
    entries_.erase(entries_.begin());
  }
  index_ = entries_.size() - 1;
  ++state_generation_;
}

void SessionHistory::PushState(std::string url, js::SerializedValue state) {
  if (entries_.empty()) {
    // A page that calls `pushState` before anything committed. There is no
    // document for the entry to belong to, so there is nothing to push onto --
    // and inventing an entry would give the back button somewhere to go that was
    // never displayed.
    return;
  }
  const std::uint64_t document = entries_[index_].document;
  entries_.resize(index_ + 1);
  HistoryEntry entry;
  entry.url = std::move(url);
  entry.state = std::move(state);
  entry.document = document;
  entries_.push_back(std::move(entry));
  if (entries_.size() > kMaxEntries) {
    entries_.erase(entries_.begin());
  }
  index_ = entries_.size() - 1;
  ++state_generation_;
}

void SessionHistory::ReplaceState(std::string url, js::SerializedValue state) {
  if (entries_.empty()) {
    return;
  }
  entries_[index_].url = std::move(url);
  entries_[index_].state = std::move(state);
  ++state_generation_;
}

void SessionHistory::SetCurrentTitle(std::string title) {
  if (!entries_.empty()) {
    entries_[index_].title = std::move(title);
  }
}

void SessionHistory::SetCurrentScroll(float scroll_y) {
  if (!entries_.empty()) {
    entries_[index_].scroll_y = scroll_y;
  }
}

void SessionHistory::SetCurrentUrl(std::string url) {
  if (!entries_.empty()) {
    entries_[index_].url = std::move(url);
  }
}

const HistoryEntry* SessionHistory::Go(int delta) {
  if (entries_.empty() || delta == 0) {
    return nullptr;
  }
  const long long target = static_cast<long long>(index_) + delta;
  if (target < 0 || target >= static_cast<long long>(entries_.size())) {
    return nullptr;
  }
  index_ = static_cast<std::size_t>(target);
  ++state_generation_;
  return &entries_[index_];
}

const HistoryEntry* SessionHistory::Current() const {
  return entries_.empty() ? nullptr : &entries_[index_];
}

HistoryEntry* SessionHistory::MutableCurrent() {
  return entries_.empty() ? nullptr : &entries_[index_];
}

void SessionHistory::Clear() {
  entries_.clear();
  index_ = 0;
  ++state_generation_;
}

}  // namespace microbrowser::engine
