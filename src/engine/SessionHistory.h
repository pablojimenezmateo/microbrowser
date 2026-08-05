#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "js/StructuredClone.h"

namespace microbrowser::engine {

// Back and forward, for one tab, where the documents are.
//
// ADR 0026 §1 moved this out of `src/ui`, and the reason is precise rather than
// architectural taste: with `pushState`, a history entry is not "a URL that was
// loaded". It is a URL **plus a state object owned by a document**, and the
// chrome cannot see a document -- `src/ui` has no `dom`, no `css`, no `layout`,
// and that separation is right. The chrome keeps what it needs to draw two
// buttons and receives it over the IPC seam, which is a strict narrowing of what
// it knows.
//
// A list plus an index, not two stacks, for the reason the version in `src/ui`
// gave: two stacks make "go back three, then navigate" a sequence of pops that
// has to be got exactly right, and they cannot answer "what is two entries back"
// at all.
struct HistoryEntry {
  // Serialized, after redirects and after the privacy layer. What the omnibox
  // shows, which is the sentence ADR 0026 §2 holds every feature to: the URL bar
  // shows the origin of the document that is displayed.
  std::string url;
  std::string title;
  // `history.state`, as bytes. Not a live `js::Value`: an entry outlives the
  // document that created it, and holding one would keep a dead document's heap
  // alive and hand a later document a reference into it. See
  // js/StructuredClone.h.
  js::SerializedValue state;
  // Restored on traversal. Zero for an entry nobody has scrolled.
  float scroll_y = 0.0f;
  // Which document this entry belongs to.
  //
  // The whole same-document/cross-document distinction is this number: two
  // entries sharing it were created by `pushState` or by a fragment change
  // within one document, so traversing between them fires `popstate` and paints,
  // and traversing to an entry with a different id loads. A URL comparison
  // cannot answer that -- `pushState('/a')` twice on one document and two loads
  // of `/a` look identical as URLs.
  std::uint64_t document = 0;
};

class SessionHistory {
 public:
  // A navigation that produced a new document. Truncates the forward entries,
  // which is what makes the forward button stop working after you take a
  // different path: the branch you left is not reachable and pretending
  // otherwise is worse than losing it.
  void PushDocument(std::string url, std::uint64_t document);

  // `history.pushState`. A new entry on the *current* document, so a traversal
  // back to the previous one is a `popstate` rather than a load.
  void PushState(std::string url, js::SerializedValue state);
  // `history.replaceState`. Rewrites the current entry and creates none, which
  // is the difference a page relies on when it is keeping the URL in step with a
  // filter box rather than with a page of results.
  void ReplaceState(std::string url, js::SerializedValue state);

  void SetCurrentTitle(std::string title);
  void SetCurrentScroll(float scroll_y);
  // Rewrites the current entry's URL without creating one. A fragment-only
  // navigation *does* create an entry; this is for the commit of a load that
  // redirected, where the entry already exists and its URL turned out to be
  // somewhere else.
  void SetCurrentUrl(std::string url);

  bool CanGoBack() const { return index_ > 0; }
  bool CanGoForward() const { return !entries_.empty() && index_ + 1 < entries_.size(); }

  // Moves by `delta` and returns the entry now current, or null when the move
  // is not possible at all. A `delta` that would run off either end moves
  // nothing -- which is what `history.go(-100)` does in every browser, and is
  // deliberately not a clamp: a page that asked to go somewhere that does not
  // exist gets no traversal rather than an arbitrary one.
  const HistoryEntry* Go(int delta);

  const HistoryEntry* Current() const;
  HistoryEntry* MutableCurrent();
  std::size_t Length() const { return entries_.size(); }
  std::size_t CurrentIndex() const { return index_; }
  bool IsEmpty() const { return entries_.empty(); }
  void Clear();

  // Bumped by anything that changes what `history.state` would answer. The
  // binding layer caches the deserialized object against this, because
  // `history.state === history.state` has to hold within one document and
  // deserializing on every read would answer with a new object each time.
  std::uint64_t StateGeneration() const { return state_generation_; }

 private:
  std::vector<HistoryEntry> entries_;
  // Always < entries_.size() when non-empty. Zero on an empty history, which is
  // not a valid index and is why Current() checks emptiness rather than the
  // index.
  std::size_t index_ = 0;
  std::uint64_t state_generation_ = 1;
};

}  // namespace microbrowser::engine
