#include "bindings/Fingerprint.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace microbrowser::bindings {

double QuantizeDevicePixelRatio(double actual) {
  if (!std::isfinite(actual) || actual <= 0.0) {
    return 1.0;
  }
  // The nearest of 1, 2, 3. Not `round`, because the boundaries matter and are not at the halves: a
  // 1.5x display is far more common than a 1.49x one, and reporting 2 for it is closer to the truth
  // for rendering than 1 is. So the thresholds sit where the real hardware clusters.
  if (actual < 1.5) {
    return 1.0;
  }
  if (actual < 2.5) {
    return 2.0;
  }
  return 3.0;
}

int QuantizeViewportExtent(int actual) {
  if (actual <= 0) {
    return 0;
  }
  // Down, never up. A page laying out to the reported width fits inside the real window; reporting more
  // than there is would make a page that filled it overflow by up to a quantum.
  const int quantised = actual - (actual % kViewportQuantum);
  // ...but never to zero for a window that has pixels. A reported zero viewport makes every `vw` unit
  // zero and every media query match `max-width: 0`, which is a blank page rather than a slightly
  // wrong one.
  return quantised == 0 ? kViewportQuantum : quantised;
}

double QuantizeTimestamp(double milliseconds) {
  if (!std::isfinite(milliseconds) || milliseconds <= 0.0) {
    return 0.0;
  }
  // Floored to the resolution rather than rounded. Rounding would let a page recover sub-resolution
  // information by sampling a value near a boundary repeatedly -- the classic attack on a coarsened
  // clock -- because the rounding direction leaks which side of the boundary the true value was on.
  return std::floor(milliseconds / kTimerResolutionMs) * kTimerResolutionMs;
}

namespace {

// The permissions this browser has an answer for, and what that answer is.
//
// A table rather than a chain of comparisons, because the *set* is the interesting part: a page can
// enumerate what it can ask about, so what is on this list is itself a decision. Everything is denied
// except `clipboard-write`, and each refusal has its reason in ADR 0029 §5.
struct Permission {
  std::string_view name;
  bool granted;
};

constexpr Permission kPermissions[] = {
    // A page reading what the user copied from somewhere else. Refused: the clipboard holds whatever
    // the user last selected anywhere on the machine, which may be a password.
    {"clipboard-read", false},
    // **The one grant.** A copy button is common and the user pressed one, so the gate is ADR 0017's
    // `isTrusted` rather than a permission -- and reporting `granted` here is honest, because a write
    // from a real gesture does work.
    {"clipboard-write", true},
    // No location to report, and a page told "denied" handles it better than one that waits.
    {"geolocation", false},
    // Default deny with no prompt. The constructor exists and does nothing visible, which is the
    // *specification's* denied behaviour -- the page is told no in the vocabulary the API has for no.
    {"notifications", false},
    // A browser that cannot play video is not one that should be capturing it.
    {"camera", false},
    {"microphone", false},
    // Nothing here reads a sensor, and each of these is a stream of numbers about the machine and the
    // room it is in.
    {"accelerometer", false},
    {"gyroscope", false},
    {"magnetometer", false},
    {"ambient-light-sensor", false},
    // ADR 0021 §6: storage is partitioned and nothing is promised to survive, so a page asking for
    // persistence is told no rather than told yes and then evicted.
    {"persistent-storage", false},
    {"midi", false},
    {"background-sync", false},
    {"push", false},
};

}  // namespace

bool IsKnownPermission(std::string_view name) {
  return std::any_of(std::begin(kPermissions), std::end(kPermissions),
                     [name](const Permission& entry) { return entry.name == name; });
}

std::string_view PermissionStateFor(std::string_view name) {
  for (const Permission& entry : kPermissions) {
    if (entry.name == name) {
      return entry.granted ? kPermissionGranted : kPermissionDenied;
    }
  }
  return kPermissionDenied;
}

}  // namespace microbrowser::bindings
