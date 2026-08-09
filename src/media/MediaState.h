#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace microbrowser::media {

// `HTMLMediaElement`'s two state machines, as state and nothing else.
//
// ADR 0028 §1: **the states are the API.** Every player on the web is written against them, and
// a `readyState` that reaches `HAVE_ENOUGH_DATA` when it should be `HAVE_CURRENT_DATA` produces
// a player that stalls with no error and no way for the page to tell. So this is a separate,
// pure object rather than fields on a DOM element: the transitions can be driven and asserted
// without a decoder, a network or a document, which is the only way to know they are the
// specification's rather than an approximation of them.
//
// It holds no samples, no buffers and no element. What it produces is a list of *events to
// fire*, in order, because in this API the order is observable -- a page that gets `canplay`
// before `loadedmetadata` reads a duration that is not there yet.
class MediaState {
 public:
  // The specification's numbers, because a page compares against them literally.
  enum class Network : std::uint8_t { Empty = 0, Idle = 1, Loading = 2, NoSource = 3 };
  enum class Ready : std::uint8_t {
    Nothing = 0,       // HAVE_NOTHING
    Metadata = 1,      // HAVE_METADATA: duration and dimensions, no frame
    CurrentData = 2,   // HAVE_CURRENT_DATA: the current frame, nothing after it
    FutureData = 3,    // HAVE_FUTURE_DATA: enough to advance a little
    EnoughData = 4,    // HAVE_ENOUGH_DATA
  };

  // Why `play()` was refused, and it is a real distinction rather than an error string: a page
  // handles `NotAllowedError` by showing a play button and `NotSupportedError` by showing an
  // error, and confusing the two makes a video that could have played look broken.
  enum class PlayRefusal : std::uint8_t { None, NotAllowed, NotSupported };

  Network NetworkState() const { return network_; }
  Ready ReadyState() const { return ready_; }
  bool Paused() const { return paused_; }
  bool Ended() const { return ended_; }
  bool Seeking() const { return seeking_; }
  bool Muted() const { return muted_; }
  void SetMuted(bool muted) { muted_ = muted; }
  double Duration() const { return duration_; }

  // Events fired since the last time they were taken, in order. Taken rather than pushed so
  // that a caller with no interpreter yet -- a document whose script has not run -- does not
  // lose them.
  std::vector<std::string_view> TakeEvents();

  // --- what the loader tells it ---------------------------------------------
  // A `src` was set. `loadstart` and Loading, and the state resets: a page that changes `src`
  // gets a fresh element, which is why `duration` becomes NaN-shaped (zero here) rather than
  // keeping the old media's.
  void BeginLoad();
  // The element has no resource yet (no `src`, no `<source>` children). `NoSource` and
  // **no** `error`: HTML's resource-selection algorithm leaves an empty element at
  // NETWORK_NO_SOURCE without MEDIA_ERR_SRC_NOT_SUPPORTED. Firing `error` here is what made
  // youtube's player stick on `fmt.unplayable` while MSE later buffered a playable stream
  // (TD-0020).
  void MarkNoSource();
  // A resource URL was selected (e.g. `video.src = URL.createObjectURL(mediaSource)`) but
  // the load has not begun. Leaves `NoSource` without `BeginLoad`: a speculative MSE attach
  // must not wipe a working readiness ladder, yet `play()` must not see `NotSupportedError`
  // either — that rejection is what youtube records as `fmt.unplayable` before the first
  // `addSourceBuffer` (TD-0020).
  void ResourceSelected();
  // Resource selection ran and every candidate failed. `NoSource` plus `error`, and **not**
  // Loading: a page that polls `networkState` for a stalled load must be able to tell
  // "nothing to load" from "loading slowly".
  void FailNoSource();
  // The container was parsed: duration is known, no frame yet.
  void MetadataArrived(double duration_seconds);
  // How much is decoded and ready, in seconds ahead of the current position. The readiness
  // ladder is climbed from this and nothing else, which is what keeps it monotone and
  // explicable -- one number in, one state out.
  void BufferedAhead(double seconds);
  // The stream ended and everything decoded has been played.
  void ReachedEnd();

  // --- what the page asks for ----------------------------------------------
  // `play()`. `has_user_activation` comes from ADR 0017's trusted-event path, and this is where
  // autoplay is refused: without activation, an unmuted element is `NotAllowedError`. A muted
  // one is allowed, which is what every browser does and what pages are written against.
  PlayRefusal Play(bool has_user_activation);
  void Pause();
  // A seek. Fires `seeking`, and readiness drops to Metadata because what was decoded was for
  // somewhere else -- a `readyState` that stayed at EnoughData across a seek is the exact bug
  // ADR 0028 §1 names: a player that stalls with no error.
  void SeekTo(double seconds);
  double CurrentTime() const { return current_time_; }
  // Time advanced, from the playback clock. `timeupdate` is throttled to about four a second
  // the way the specification asks, because a page that re-renders a scrubber per audio
  // callback is a page that drops frames doing it.
  void AdvanceTo(double seconds);

 private:
  void SetReady(Ready next);

  Network network_ = Network::Empty;
  Ready ready_ = Ready::Nothing;
  bool paused_ = true;
  bool ended_ = false;
  bool seeking_ = false;
  bool muted_ = false;
  bool waiting_ = false;
  bool fired_playing_ = false;
  double duration_ = 0.0;
  double current_time_ = 0.0;
  double last_time_update_ = -1.0;
  std::vector<std::string_view> events_;
};

}  // namespace microbrowser::media
