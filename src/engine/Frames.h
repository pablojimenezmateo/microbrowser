#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "csp/ContentSecurityPolicy.h"
#include "dom/Node.h"

namespace microbrowser::engine {

class Page;

// A child browsing context: the `<iframe>` that holds it, and the document in it.
//
// ADR 0027 §1. `engine::Page` is already "one loaded document: its DOM, its styles, its box tree
// and the display list they produce", and its own header calls it the unit a second tab duplicates
// -- so a child context is another `Page`, and the tree is `Page` owning `Frame`s. That is the
// smallest shape that makes the rest expressible: the child lays out into a viewport the parent's
// layout gave it, produces its own display list, and the parent splices that list under a transform
// and a clip. Nothing in it requires the child to hold a pointer into its parent, which is ADR 0027
// §5's first constraint and the thing that makes the process split an extraction rather than a
// rewrite.
//
// **Lifetime, which is the dangerous part.** `dom::Element::SetNestedDocument` puts a *borrowed*
// pointer to the child's document on the parent's element, so that `iframe.contentDocument` can
// answer without `src/bindings` having to see this module. The child's document is owned by
// `page`, which is owned by this struct, which is owned by the parent's `FrameTree`. So the pointer
// is valid exactly as long as the frame is -- and `FrameTree::Clear` is the one place that ends a
// frame, and it clears the element's pointer *before* dropping the page. Every path that discards
// frames goes through it; adding a second one is how this becomes a use-after-free that a page can
// drive with `iframe.remove()`.
struct Frame {
  // The `<iframe>` in the *parent's* document. Borrowed: the element is owned by that document,
  // which outlives this, because a frame is discarded when the document that named it is.
  dom::Element* element = nullptr;
  // The child context. Never null for a frame that exists.
  std::unique_ptr<Page> page;
  // The URL the frame was told to load, resolved against the parent's base -- or `about:blank`,
  // which is what a frame with no `src` is and which inherits the embedder's origin. That
  // inheritance is the one place a document's origin is not derived from its URL, and ADR 0027's
  // consequences list names it as the source of a recurring vulnerability class.
  std::string url;
  // What the element asked for when this frame was last handed a document: the `src` attribute as
  // written, or the `srcdoc` markup. **The frame reloads when this changes and only then** --
  // `iframe.src = other` is a navigation and `div.className = x` is not, and without a recorded
  // answer the two are indistinguishable from a collection pass that runs on every mutation.
  std::string requested_source;
  // Whether the child's document has arrived. The parent's `load` event waits for every frame,
  // which is what HTML says and what makes `body onload` a point where `contentDocument` is
  // readable -- the whole of how the `encoding/legacy-mb-*` decode tests are written.
  bool loaded = false;
  // Whether the loader has been told about it. Distinct from `loaded` because
  // `Collect` runs again on every mutation that touches the tree, and a
  // frame that was asked for and has not answered yet must not be asked twice --
  // which would be a second request to whoever the frame points at, per
  // mutation, for as long as the page keeps mutating.
  bool requested = false;
  // Whether the child is same-origin with this document. The answer to
  // `iframe.contentDocument`, decided by the loader because that is the module that understands
  // URLs, and recorded rather than recomputed so that one frame cannot be same-origin to one caller
  // and not to another.
  bool same_origin = false;
};

// Every `<iframe>` in `document`, in tree order, with the `src` exactly as written. A frame inside
// a `<template>`'s contents is not here, because nothing reaches template contents -- which is the
// property that stops a template full of ad slots from loading them.
std::vector<dom::Element*> CollectFrameElements(dom::Document& document);

// One document's child browsing contexts: which there are, which have documents, and which are
// owed a `load` event.
//
// Lifted out of `Page` because ADR 0027's consequences list opens with "`Page` stops meaning *the
// document* across the whole engine", and the first thing that costs is somewhere for the tree's
// own state to live. `Page` was at its member budget with `frames_` alone; a collection pass that
// can run on every mutation needs two more fields than that, and hanging them off the class that
// owns everything else about a document is how "which context?" becomes unanswerable.
class FrameTree {
 public:
  FrameTree();
  ~FrameTree();
  FrameTree(const FrameTree&) = delete;
  FrameTree& operator=(const FrameTree&) = delete;
  FrameTree(FrameTree&&) = delete;
  FrameTree& operator=(FrameTree&&) = delete;

  const std::vector<Frame>& Frames() const { return frames_; }
  std::vector<Frame>& MutableFrames() { return frames_; }

  // Whether the tree is worth re-deriving: true when `document`'s structure has moved since the
  // last `Collect`. **A structure version rather than a mutation version**, and the difference is
  // measurable: a WAAPI polyfill writes an attribute every frame and bumps the mutation version
  // with it, and re-walking a hundred thousand nodes at 60Hz to discover no new `<iframe>` is the
  // shape TD-0021 was filed about.
  bool NeedsCollect(const dom::Document* document) const;

  // Re-derives the list from `document`, keeping the context of every element that survived, and
  // returns the indices that have no document yet. `make_page` builds an empty child context; a
  // callback rather than a constructor argument because what a `Page` needs to exist is the
  // parent's font context, and this class has no business knowing that.
  std::vector<std::size_t> Collect(dom::Document* document,
                                   const std::function<std::unique_ptr<Page>()>& make_page);

  // Records that this frame's document has been asked for, and *what* was asked for. The second
  // half is what makes `iframe.src = other` a navigation and `div.className = x` not one; see the
  // body, because the ordering against the response is where this goes wrong.
  void MarkRequested(std::size_t index);

  // Hands one frame its document. The caller decides `same_origin`, because deciding it needs
  // `src/url` and neither this class nor `Page` may see it -- ADR 0027 §2, and the reason the
  // cross-origin case has nothing to hide rather than something to guard.
  void SetDocument(std::size_t index, std::string_view html, std::string url,
                   csp::PolicyList header_policy, std::string_view content_type, bool same_origin);

  // Whether every frame in the whole subtree has a document. The parent's `load` waits on it.
  bool AllLoaded() const;

  void Clear();

  // The elements owed a `load` event, taken once and cleared.
  //
  // Queued rather than fired from `SetDocument` because HTML fires this from a task, and because
  // an `about:blank` frame is handed its document *synchronously* inside the collection pass --
  // firing there would dispatch `load` before the script that is about to set `onload` has run,
  // which is every `iframe.onload = ...; document.body.appendChild(iframe)` in the suite.
  std::vector<dom::Element*> TakePendingLoadEvents();

 private:
  // The frames alone: every element's borrowed document pointer cleared, then every context
  // dropped. `Clear` is this plus the owed-`load` queue; the collection pass wants only this half,
  // because it runs *between* a frame being handed a document and the event being dispatched.
  void DropFrames();
  // Drops queued `load` events for elements that are not in `live`. A frame that left the document
  // between its response arriving and this turn is owed nothing.
  void ForgetLoadEventsNotIn(const std::vector<Frame>& live);

  std::vector<Frame> frames_;
  // What `document->StructureVersion()` read at the last `Collect`, plus one so that the first
  // call always runs. See NeedsCollect.
  std::uint64_t collected_structure_version_ = 0;
  bool ever_collected_ = false;
  std::vector<dom::Element*> pending_load_events_;
};

}  // namespace microbrowser::engine
