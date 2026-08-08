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
  // sets it -- see Page::DispatchClickAt. This is the line ADR 0028 §1's autoplay refusal
  // actually turns on.
  const bool activated = document_ != nullptr && document_->HasUserActivation();
  const media::MediaState::PlayRefusal refusal = state->Play(activated);
  FlushMediaEvents(element);
  switch (refusal) {
    case media::MediaState::PlayRefusal::None:
      video_.StartPlayback(element, *state);
      (void)video_.AdvanceAll([this](dom::Element& el) { return MediaStateFor(el); });
      InvalidateLayout();
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
    script_.DispatchMediaEvent(element, std::string(type));
  }
}

}  // namespace microbrowser::engine
