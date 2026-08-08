#include "media/MediaState.h"

#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::media {

namespace {

// A quarter second. The specification says "about 4Hz" and leaves it to the implementation; a
// page that re-rendered a scrubber per audio callback would drop frames doing it, which is the
// reason the throttle exists rather than a nicety.
constexpr double kTimeUpdateInterval = 0.25;

}  // namespace

std::vector<std::string_view> MediaState::TakeEvents() {
  std::vector<std::string_view> taken;
  taken.swap(events_);
  return taken;
}

void MediaState::SetReady(Ready next) {
  if (next == ready_) {
    return;
  }
  const Ready previous = ready_;
  ready_ = next;
  // The ladder's events, and each one fires *once per climb* rather than per state. Climbing
  // from Nothing to EnoughData in one step still fires all of them in order, which is what a
  // page waiting on `canplay` depends on when a whole file arrives at once.
  if (previous < Ready::Metadata && next >= Ready::Metadata) {
    events_.push_back("loadedmetadata");
  }
  if (previous < Ready::CurrentData && next >= Ready::CurrentData) {
    events_.push_back("loadeddata");
  }
  if (previous < Ready::FutureData && next >= Ready::FutureData) {
    events_.push_back("canplay");
  }
  if (previous < Ready::EnoughData && next >= Ready::EnoughData) {
    events_.push_back("canplaythrough");
  }
  // Coming *back* up after a stall is where `playing` fires again, and it is a separate
  // question from the ladder: a page shows a spinner on `waiting` and hides it here.
  if (waiting_ && next >= Ready::FutureData) {
    waiting_ = false;
    if (!paused_) {
      events_.push_back("playing");
    }
  }
}

void MediaState::BeginLoad() {
  network_ = Network::Loading;
  ready_ = Ready::Nothing;
  ended_ = false;
  seeking_ = false;
  waiting_ = false;
  fired_playing_ = false;
  duration_ = 0.0;
  current_time_ = 0.0;
  last_time_update_ = -1.0;
  events_.push_back("loadstart");
  util::AddPerformanceCounter(util::PerfCounterId::MediaLoadsStarted);
}

void MediaState::FailNoSource() {
  // Not Loading, and that is the point: a page polling `networkState` for a stalled load has to
  // be able to tell "there is nothing to load" from "loading slowly".
  network_ = Network::NoSource;
  ready_ = Ready::Nothing;
  events_.push_back("error");
  util::AddPerformanceCounter(util::PerfCounterId::MediaLoadFailures);
}

void MediaState::MetadataArrived(double duration_seconds) {
  const double next = duration_seconds > 0.0 ? duration_seconds : 0.0;
  // Only signal when the number changes. UpdateMediaReadiness asks this after every append, and
  // firing `durationchange` (and worse, dropping `readyState` back to HAVE_METADATA) on each one
  // is what made youtube tear down a working MediaSource and attach an empty replacement.
  if (next != duration_) {
    duration_ = next;
    events_.push_back("durationchange");
  }
  if (ready_ < Ready::Metadata) {
    SetReady(Ready::Metadata);
  }
}

void MediaState::BufferedAhead(double seconds) {
  if (ready_ == Ready::Nothing) {
    // Data before metadata is data for a container nobody has parsed. Ignored rather than
    // promoted: the ladder's rungs mean specific things and the first one is "we know what this
    // is".
    return;
  }
  // One number in, one state out. Deriving the ladder from a single measurement is what keeps
  // it monotone and explicable -- and what makes "why is it not EnoughData" a question with an
  // answer.
  if (seconds <= 0.0) {
    SetReady(Ready::CurrentData);
  } else if (seconds < 1.0) {
    SetReady(Ready::FutureData);
  } else {
    SetReady(Ready::EnoughData);
  }
  if (network_ == Network::Loading && ready_ == Ready::EnoughData) {
    network_ = Network::Idle;
  }
}

void MediaState::ReachedEnd() {
  if (ended_) {
    return;
  }
  ended_ = true;
  paused_ = true;
  network_ = Network::Idle;
  current_time_ = duration_;
  // `pause` is *not* fired at the end, which is a real asymmetry: `ended` is the end of a
  // stream and `pause` is a thing a page or a user did, and a page that treats them the same
  // shows a play button where it should show a replay button.
  events_.push_back("timeupdate");
  events_.push_back("ended");
}

MediaState::PlayRefusal MediaState::Play(bool has_user_activation) {
  if (network_ == Network::NoSource) {
    // Nothing to play. `NotSupportedError` rather than `NotAllowedError`, because a page
    // handles the two differently -- an error message versus a play button -- and confusing
    // them makes a video that could have played look broken.
    return PlayRefusal::NotSupported;
  }
  if (!has_user_activation && !muted_) {
    // ADR 0028 §1, and it reads ADR 0017's user activation. Muted is allowed, which is what
    // every browser does and what pages are written against.
    util::AddPerformanceCounter(util::PerfCounterId::MediaAutoplayRefusals);
    return PlayRefusal::NotAllowed;
  }
  if (ended_) {
    // `play()` on a finished element rewinds. Without this, a replay button does nothing and
    // the page has to know to seek first.
    ended_ = false;
    current_time_ = 0.0;
    seeking_ = false;
  }
  const bool was_paused = paused_;
  paused_ = false;
  if (was_paused) {
    events_.push_back("play");
  }
  if (ready_ >= Ready::FutureData) {
    if (!fired_playing_ || was_paused) {
      events_.push_back("playing");
      fired_playing_ = true;
    }
  } else {
    // Playing without enough data is `waiting`, not `playing`. A page shows its spinner from
    // this, and an element that claimed `playing` here would show video that is not moving.
    waiting_ = true;
    events_.push_back("waiting");
  }
  return PlayRefusal::None;
}

void MediaState::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;
  waiting_ = false;
  events_.push_back("timeupdate");
  events_.push_back("pause");
}

void MediaState::SeekTo(double seconds) {
  const double clamped = seconds < 0.0 ? 0.0 : (duration_ > 0.0 && seconds > duration_ ? duration_
                                                                                      : seconds);
  current_time_ = clamped;
  ended_ = false;
  seeking_ = true;
  events_.push_back("seeking");
  // **Readiness drops.** What was decoded was for somewhere else, and a `readyState` that
  // stayed at EnoughData across a seek is exactly the bug ADR 0028 §1 names: a player that
  // stalls with no error and no way for the page to tell.
  if (ready_ > Ready::Metadata) {
    ready_ = Ready::Metadata;
  }
  if (!paused_) {
    waiting_ = true;
  }
}

void MediaState::AdvanceTo(double seconds) {
  if (seeking_) {
    // The first advance after a seek is the seek completing: the clock is now reporting the new
    // position, which is the only evidence a decoder has arrived there.
    seeking_ = false;
    events_.push_back("seeked");
  }
  current_time_ = seconds;
  if (last_time_update_ < 0.0 || seconds - last_time_update_ >= kTimeUpdateInterval ||
      seconds < last_time_update_) {
    last_time_update_ = seconds;
    events_.push_back("timeupdate");
  }
  if (duration_ > 0.0 && seconds >= duration_) {
    ReachedEnd();
  }
}

}  // namespace microbrowser::media
