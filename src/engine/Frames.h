#pragma once

#include <memory>
#include <string>
#include <vector>

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
// `page`, which is owned by this struct, which is owned by the parent `Page`. So the pointer is
// valid exactly as long as the frame is -- and `Page::ClearFrames` is the one place that ends a
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
  // Whether the child's document has arrived. The parent's `load` event waits for every frame,
  // which is what HTML says and what makes `body onload` a point where `contentDocument` is
  // readable -- the whole of how the `encoding/legacy-mb-*` decode tests are written.
  bool loaded = false;
  // Whether the loader has been told about it. Distinct from `loaded` because
  // `CollectFrames` runs again on every mutation that touches the tree, and a
  // frame that was asked for and has not answered yet must not be asked twice --
  // which would be a second request to whoever the frame points at, per
  // mutation, for as long as the page keeps mutating.
  bool requested = false;
  // Whether the child is same-origin with this document. The answer to
  // `iframe.contentDocument`, decided here because this module is the one that understands URLs,
  // and recorded rather than recomputed so that one frame cannot be same-origin to one caller and
  // not to another.
  bool same_origin = false;
};

// Every `<iframe>` in `document`, in tree order, with the `src` exactly as written. A frame inside
// a `<template>`'s contents is not here, because nothing reaches template contents -- which is the
// property that stops a template full of ad slots from loading them.
std::vector<dom::Element*> CollectFrameElements(dom::Document& document);

}  // namespace microbrowser::engine
