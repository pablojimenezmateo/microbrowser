#pragma once

#include <cstddef>

namespace microbrowser::media {

class AudioRing;

// Where samples go, declared here and implemented by `src/platform`.
//
// ADR 0028 §4, and the inversion this repository uses for every seam like it (geometry,
// storage, sockets): `src/engine` may not name `src/platform` -- the module that knows what
// a device is is the module nothing above it may reach into -- so the *interface* lives
// here, in a module both can see, and `src/app` connects the two.
//
// **The device is the thread.** With SDL3's audio API the callback that drains the ring runs
// on SDL's own thread, so "no audio thread when nothing is playing" is exactly "no open
// device when nothing is playing" -- which is why this interface is `Start`/`Stop` rather
// than `SetVolume`-style state on a device that is always there. A caller that keeps a
// device open for a paused element has broken the zero-idle-CPU invariant even though it
// wrote no timer.
//
// The ring is *borrowed*, and its lifetime is the caller's problem in exactly one direction:
// it must outlive the sink. `Stop` joins -- it does not return until the device will make no
// further callback -- so a caller that stops before destroying the ring is correct by
// construction and one that does not is a use-after-free the sink cannot prevent.
class AudioSink {
 public:
  AudioSink() = default;
  AudioSink(const AudioSink&) = delete;
  AudioSink& operator=(const AudioSink&) = delete;
  virtual ~AudioSink() = default;

  // Opens a device and starts draining `ring`. False when there is no device -- a machine
  // with no sound card, a container, a test host -- which is not an error: it is a browser
  // that plays video silently rather than one that refuses to play it.
  virtual bool Start(AudioRing& ring) = 0;

  // Closes the device and joins. After this returns the ring is untouched by anything but
  // the caller.
  virtual void Stop() = 0;

  virtual bool IsRunning() const = 0;
};

}  // namespace microbrowser::media
