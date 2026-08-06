#pragma once

#include <map>

#include "media/MediaState.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::engine {

// One state machine per media element, and the volume beside it.
//
// ADR 0028 §1. This exists because the architecture lint said so, and the lint was right: these
// members were on `Page`, which pushed it to holding members from five modules -- and the message
// is "split the coordination rather than widening the class". The split is real rather than
// cosmetic. What `Page` does with media is *coordinate*: it reads the document's user activation,
// asks this for a state machine, and fires the events at script. What this does is own the map.
//
// Created on first use rather than per element in the tree: a feed with fifty `<video>` elements
// gets fifty state machines only if a page touches fifty, and an untouched element reads as the
// specification's initial state anyway.
class MediaElements {
 public:
  // Null for an element that is not `<video>` or `<audio>`. The caller decides what to do about
  // that -- the binding throws, the engine ignores -- because "not a media element" means
  // different things to the two of them.
  media::MediaState* For(const dom::Element& element, bool has_source);
  const media::MediaState* Find(const dom::Element& element) const;

  // Volume is here rather than in `MediaState` because it changes nothing about readiness or
  // playback: it is a number the output stage reads, and putting it in the state machine would
  // mean a transition table with a case for "the user moved a slider".
  void SetVolume(const dom::Element& element, double volume);
  double Volume(const dom::Element& element) const;

  // A navigation. The elements are gone, so their state machines are too -- and a machine that
  // outlived its element would answer about a `<video>` nobody can see.
  void Clear();

 private:
  std::map<const dom::Element*, media::MediaState> states_;
  std::map<const dom::Element*, double> volumes_;
};

}  // namespace microbrowser::engine
