// `<video>` and `<audio>`, from the page's side.
//
// ADR 0028 §1. Its own translation unit because Page.cpp is at its module cap, and the seam is a
// real one: everything here is the *state machine per element* plus the events it produced, and
// the state machine itself is `media::MediaState` -- a pure object with no element in it, tested
// on its own. This file is what connects one to the other, and that is all it is.
//
// `src/bindings` implements none of this: it may not see `media`, so it asks through
// `bindings::MediaController` and receives numbers. A page therefore cannot reach a ring buffer
// or a device through its own element.

#include <string>
#include <utility>

#include "bindings/Media.h"
#include "dom/Node.h"
#include "engine/Page.h"
#include "media/MediaState.h"

namespace microbrowser::engine {

namespace {

bool IsMediaTag(const dom::Element& element) {
  return element.TagName() == "video" || element.TagName() == "audio";
}

bool HasSourceAttribute(const dom::Element& element) {
  const std::string* src = element.GetAttribute("src");
  return src != nullptr && !src->empty();
}

const dom::Element* ParentElement(const dom::Element& element) {
  const dom::Node* parent = element.Parent();
  return parent != nullptr && parent->IsElement() ? static_cast<const dom::Element*>(parent)
                                                    : nullptr;
}

const dom::Element* FindMediaInSubtree(const dom::Element& root) {
  if (IsMediaTag(root)) {
    return &root;
  }
  const dom::Element* found = nullptr;
  root.ForEachDescendant([&](const dom::Node& node) {
    if (found != nullptr || !node.IsElement()) {
      return;
    }
    const auto& candidate = static_cast<const dom::Element&>(node);
    if (IsMediaTag(candidate)) {
      found = &candidate;
    }
  });
  return found;
}

const dom::Element* MediaElementForClickTarget(const dom::Element& hit) {
  for (const dom::Element* at = &hit; at != nullptr; at = ParentElement(*at)) {
    if (IsMediaTag(*at)) {
      return at;
    }
    const std::string* id = at->GetAttribute("id");
    if (id != nullptr && *id == "movie_player") {
      return FindMediaInSubtree(*at);
    }
  }
  return nullptr;
}

}  // namespace

media::MediaState* Page::MediaStateFor(const dom::Element& element) {
  return media_.For(element, HasSourceAttribute(element));
}

const media::MediaState* Page::MediaStateFor(const dom::Element& element) const {
  // Creates, like the non-const one. See Page.h: a read has to answer about the `src` the
  // element already has, and the alternative -- defaults until something writes -- reports
  // "no source" for an element that has one.
  return media_.For(element, HasSourceAttribute(element));
}

bool Page::IsMedia(const dom::Element& element) const { return IsMediaTag(element); }

bindings::MediaController::PlayResult Page::Play(dom::Element& element) {
  media::MediaState* state = MediaStateFor(element);
  if (state == nullptr) {
    return bindings::MediaController::PlayResult::NotSupported;
  }
  // The activation the state machine asks about is the document's, and only a trusted event
  // sets it -- see Page::DispatchPointerDownAt. This is the line ADR 0028 §1's autoplay refusal
  // actually turns on.
  const bool activated = document_ != nullptr && document_->HasUserActivation();
  // A `src` (including `blob:` for MSE) means resource selection has a candidate. `play()`
  // before the first SourceBuffer must not reject with `NotSupportedError` — that rejection
  // is not a failed selection. `ResourceSelected` leaves NO_SOURCE without `BeginLoad`.
  if (state->NetworkState() == media::MediaState::Network::NoSource &&
      HasSourceAttribute(element)) {
    state->ResourceSelected();
  }
  const media::MediaState::PlayRefusal refusal = state->Play(activated);
  FlushMediaEvents(element);
  switch (refusal) {
    case media::MediaState::PlayRefusal::None:
      video_.StartPlayback(element, *state);
      (void)video_.AdvanceAll([this](dom::Element& el) { return MediaStateFor(el); });
      // A new decoded frame damages the compositor surface (AddSurfaceDamage),
      // not the box tree — InvalidateLayout here was a 60Hz BuildBoxTree.
      return bindings::MediaController::PlayResult::Started;
    case media::MediaState::PlayRefusal::NotAllowed:
      return bindings::MediaController::PlayResult::NotAllowed;
    case media::MediaState::PlayRefusal::NotSupported:
      break;
  }
  return bindings::MediaController::PlayResult::NotSupported;
}

void Page::Pause(dom::Element& element) {
  if (media::MediaState* state = MediaStateFor(element)) {
    state->Pause();
    FlushMediaEvents(element);
  }
  // Stop the device even if this element had no MediaState yet -- a playing
  // session's sink must not outlive Pause (zero-idle-CPU / ADR 0028 §4).
  video_.StopOutput();
}

void Page::Load(dom::Element& element) {
  media::MediaState* state = MediaStateFor(element);
  if (state == nullptr) {
    return;
  }
  // HTML's media element load algorithm, reduced to what this engine can honour: abort the
  // current resource selection answer and start again from the `src` on the element. A no-op
  // would leave stale readiness; a throw made youtube's `playVideo` → `load()` path a hard
  // failure (TD-0020).
  if (HasSourceAttribute(element)) {
    state->BeginLoad();
    FlushMediaEvents(element);
    const std::string* src = element.GetAttribute("src");
    if (src != nullptr && src->rfind("blob:", 0) == 0) {
      (void)AttachMediaSource(element, *src);
    }
  } else {
    state->MarkNoSource();
    FlushMediaEvents(element);
  }
}

void Page::Seek(dom::Element& element, double seconds) {
  if (media::MediaState* state = MediaStateFor(element)) {
    state->SeekTo(seconds);
    FlushMediaEvents(element);
  }
}

void Page::SetMuted(dom::Element& element, bool muted) {
  if (media::MediaState* state = MediaStateFor(element)) {
    state->SetMuted(muted);
    video_.UpdateOutput(*state);
  }
}

void Page::SetVolume(dom::Element& element, double volume) { media_.SetVolume(element, volume); }

double Page::Volume(const dom::Element& element) const { return media_.Volume(element); }

double Page::CurrentTime(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  return state == nullptr ? 0.0 : state->CurrentTime();
}

double Page::Duration(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  // Zero rather than NaN for an element nobody has touched. NaN is what the specification says
  // and it is what a *loaded* element with unknown duration reads; answering it here would make
  // `duration` NaN for an element this page has simply not created state for yet, which reads
  // the same to a page and is not the same thing. Recorded as a deviation rather than hidden.
  return state == nullptr ? 0.0 : state->Duration();
}

int Page::ReadyState(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  return state == nullptr ? 0 : static_cast<int>(state->ReadyState());
}

int Page::NetworkState(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  // `EMPTY` for an element with no state yet, which is exactly what it is: nothing has been
  // asked of it.
  return state == nullptr ? 0 : static_cast<int>(state->NetworkState());
}

bool Page::Paused(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  return state == nullptr || state->Paused();
}

bool Page::Ended(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  return state != nullptr && state->Ended();
}

bool Page::Muted(const dom::Element& element) const {
  const media::MediaState* state = MediaStateFor(element);
  return state != nullptr && state->Muted();
}

int Page::VideoWidth(const dom::Element& element) const { return video_.VideoWidth(element); }

int Page::VideoHeight(const dom::Element& element) const { return video_.VideoHeight(element); }

void Page::FlushMediaEvents(dom::Element& element) {
  media::MediaState* state = MediaStateFor(element);
  if (state == nullptr) {
    return;
  }
  // The state machine produced them in order and this fires them in that order. Nothing here
  // decides *which* events happened -- that is the machine's job and it is tested on its own,
  // which is the whole reason this function is four lines.
  for (const std::string_view type : state->TakeEvents()) {
    script_->DispatchMediaEvent(element, std::string(type));
  }
}

bool Page::ToggleMediaPlaybackAt(gfx::FloatPoint document_point) {
  EnsureLayoutClean();
  if (boxes_ == nullptr || document_ == nullptr) {
    return false;
  }
  const dom::Element* hit = ElementAt(document_point);
  if (hit == nullptr) {
    return false;
  }
  return ToggleMediaPlaybackOn(*const_cast<dom::Element*>(hit));
}

bool Page::ToggleMediaPlaybackOn(dom::Element& hit) {
  const dom::Element* media = MediaElementForClickTarget(hit);
  if (media == nullptr || !IsMediaTag(*media)) {
    return false;
  }
  auto& element = *const_cast<dom::Element*>(media);
  if (Paused(element)) {
    return Play(element) == bindings::MediaController::PlayResult::Started;
  }
  Pause(element);
  return true;
}

}  // namespace microbrowser::engine
