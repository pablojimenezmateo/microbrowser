#pragma once

#include <cstdint>

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::bindings {

// What `HTMLMediaElement`'s properties and methods are answered through.
//
// ADR 0028 §1, and the same inversion the other seams use: declared here, implemented by
// `src/engine`. This module may see `util`, `js`, `dom` and `html` -- **not `media`** -- so
// nothing here can name `media::MediaState`, hold a ring buffer, or reach a device. The state
// machine is the specification's and it lives on the far side; this side only asks it questions.
//
// The questions are per *element* rather than per source object, because that is what the API
// is: `video.currentTime` is a property of the element, and two `<video>` elements pointing at
// one file are two independent players. An element pointer is the key on both sides.
class MediaController {
 public:
  // Why `play()` was refused. Three answers rather than a bool because a page handles them
  // differently -- `NotAllowedError` means show a play button and `NotSupportedError` means show
  // an error -- and collapsing them makes a video that could have played look broken.
  enum class PlayResult : std::uint8_t { Started, NotAllowed, NotSupported };

  MediaController() = default;
  MediaController(const MediaController&) = delete;
  MediaController& operator=(const MediaController&) = delete;
  virtual ~MediaController() = default;

  virtual PlayResult Play(dom::Element& element) = 0;
  virtual void Pause(dom::Element& element) = 0;
  // A seek. The position moves immediately -- which is what the API promises and what a scrubber
  // depends on -- and the readiness drop that follows is the state machine's business.
  virtual void Seek(dom::Element& element, double seconds) = 0;
  virtual void SetMuted(dom::Element& element, bool muted) = 0;
  virtual void SetVolume(dom::Element& element, double volume) = 0;

  // The readable state. Doubles and ints rather than enums this module declares, because the
  // *numbers* are the API -- a page writes `video.readyState >= 3` -- and a second enumeration
  // of the same four values is one more place for them to stop matching.
  virtual double CurrentTime(const dom::Element& element) const = 0;
  virtual double Duration(const dom::Element& element) const = 0;
  virtual double Volume(const dom::Element& element) const = 0;
  virtual int ReadyState(const dom::Element& element) const = 0;
  virtual int NetworkState(const dom::Element& element) const = 0;
  virtual bool Paused(const dom::Element& element) const = 0;
  virtual bool Ended(const dom::Element& element) const = 0;
  virtual bool Muted(const dom::Element& element) const = 0;
  // Whether this element is a media element at all. Asked before anything else, so that
  // `document.body.play` is undefined rather than a function that answers about nothing.
  virtual bool IsMedia(const dom::Element& element) const = 0;
};

}  // namespace microbrowser::bindings
