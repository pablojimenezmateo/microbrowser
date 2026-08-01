#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace microbrowser::ui {

// Back and forward, for one tab.
//
// A list plus an index, not two stacks. Two stacks make "go back three, then
// navigate" a sequence of pops that has to be got exactly right, and they
// cannot answer "what is two entries back" at all — which is what a
// long-press-on-back menu asks. One list with a cursor makes both trivial and
// makes the invariant checkable: the index is always inside the list.
//
// Deliberately holds no page state. An entry is a URL and a title; restoring
// scroll position and form state is a decision about what a browser remembers
// across a session, and this type must not quietly become the place that
// decides it.
class NavigationHistory {
 public:
  struct Entry {
    std::string url;
    std::string title;

    friend bool operator==(const Entry&, const Entry&) = default;
  };

  // Records a navigation the user caused. Truncates the forward entries, which
  // is what makes the forward button stop working after you take a different
  // path -- the branch you left is not reachable and pretending otherwise is
  // worse than losing it.
  void Push(std::string url, std::string title);

  // Updates the current entry's title without creating a new one. A page whose
  // <title> arrives after the navigation must not become a second history
  // entry, or the back button needs pressing twice.
  void SetCurrentTitle(std::string title);

  bool CanGoBack() const { return index_ > 0; }
  bool CanGoForward() const { return !entries_.empty() && index_ + 1 < entries_.size(); }

  // Move and return the entry now current. Null when the move is not possible,
  // which a caller checks rather than being given a silently unchanged entry.
  const Entry* GoBack();
  const Entry* GoForward();

  const Entry* Current() const;
  const std::vector<Entry>& Entries() const { return entries_; }
  std::size_t CurrentIndex() const { return index_; }
  bool IsEmpty() const { return entries_.empty(); }
  void Clear();

 private:
  std::vector<Entry> entries_;
  // Always < entries_.size() when non-empty. Zero on an empty history, which is
  // not a valid index and is why Current() checks emptiness rather than the
  // index.
  std::size_t index_ = 0;
};

}  // namespace microbrowser::ui
