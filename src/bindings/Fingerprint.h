#pragma once

#include <string_view>

namespace microbrowser::bindings {

// **ADR 0029 §6's table, as code.**
//
// Session 37. The ADR wrote the answers down in a table "so they are consistent"; this is that table
// where the answers actually come from, for the reason `util::kUserAgent` is one constant rather than
// two: a page may sniff several of these, and two constants that were meant to agree eventually do not.
//
// The governing rule is ADR 0029 §1 and it is worth having in front of the values: **where a page asks
// about the machine, the answer must be one that every copy of this browser would give.** Not
// randomised -- *constant*. A jittered answer is still an answer, it is distinguishable as jittered, and
// repeated sampling averages it away; meanwhile it breaks every honest consumer.
//
// The absences matter as much as the values, and they are asserted by a test rather than left to be
// noticed. `navigator.deviceMemory`, `navigator.connection`, `navigator.getBattery`,
// `navigator.geolocation`, `navigator.mediaDevices`, `navigator.doNotTrack` and `navigator.fonts` are
// all **absent**, which under ADR 0012's rule sends a page to whatever path it has for a browser
// without them rather than into a wall. A test names each one, so adding any of them is a decision
// somebody makes on purpose.

// `navigator.platform`. A constant that says what this browser is and nothing about the machine -- and
// not the empty string, because a page that branches on `platform` and finds nothing often takes a
// worse path than one that finds something it does not recognise.
inline constexpr std::string_view kPlatform = "Unknown";

// `navigator.vendor`. Empty is what Firefox reports and what the specification calls the "vendor" of
// the browser rather than of the machine; it carries no bits either way.
inline constexpr std::string_view kVendor = "";

// `navigator.language` and the single entry of `navigator.languages`.
//
// The same constant `Accept-Language` sends, and shared for the reason the user agent is: a page that
// localises from the header and from the property must not be told two different things. **This is a
// real cost** -- a user whose system language is not English gets English -- and it is the trade ADR
// 0029 §1 selects, because a language list is among the highest-entropy things a browser volunteers.
inline constexpr std::string_view kLanguage = "en-US";

// `navigator.hardwareConcurrency`. A constant, not the core count.
//
// Four rather than one: a page that reads it usually sizes a worker pool from it, and one would make
// every such page single-threaded forever. Four is enough to be a plausible answer and small enough
// that a page sizing buffers per worker does not allocate for thirty-two.
inline constexpr int kHardwareConcurrency = 4;

// `devicePixelRatio`, quantised to a small set.
//
// ADR 0029 §1's one concession: this is a number rendering genuinely needs, so the mitigation is to
// reduce the entropy of the truth rather than to lie about it. Three values -- 1, 2 and 3 -- covers
// every display anyone ships, and a 1.5x or 2.25x panel reports the nearest of them. **The cost is
// real and it is sharpness**: text on such a display is rendered for a ratio slightly off its own.
double QuantizeDevicePixelRatio(double actual);

// The viewport, quantised, for `innerWidth`/`innerHeight`, `screen.*` and `matchMedia`.
//
// The window's exact pixel size is one of the highest-entropy signals a page can read without asking
// for anything, because a user resizes a window to a number nobody else has. Rounding *down* to a
// multiple of `kViewportQuantum` collapses that: a page laying out to the reported width fits inside
// the real one, which is the direction that cannot overflow.
//
// **Down rather than to-nearest** for exactly that reason -- rounding up would report a viewport
// larger than the window and a page that filled it would overflow by up to a quantum.
inline constexpr int kViewportQuantum = 8;
int QuantizeViewportExtent(int actual);

// `performance.now()`'s resolution, in milliseconds.
//
// **This one is a security measure rather than a privacy one**, and it is on this table because the
// mechanism is identical: high-resolution timers are what turn cache and speculative-execution side
// channels from papers into practical attacks. 100 microseconds is the figure the browsers converged on
// after Spectre -- coarse enough that a timing loop cannot resolve a cache hit from a miss, fine enough
// that a page measuring a frame or a fetch gets a useful number.
inline constexpr double kTimerResolutionMs = 0.1;
double QuantizeTimestamp(double milliseconds);

// `Notification.permission` and every entry the Permissions API reports.
//
// **Default deny, and no prompt.** Prompting is rejected as a mechanism rather than deferred: a prompt
// on a capability the user did not ask for is a decision they are unequipped to make in the moment, and
// every study of them says the answer is fatigue. The state is reported *honestly*, which is what makes
// the Permissions API worth having rather than decorative -- a page that asks and is told "denied" can
// say so to its user, where one told "prompt" waits forever.
inline constexpr std::string_view kPermissionDenied = "denied";
inline constexpr std::string_view kPermissionGranted = "granted";

// Whether a permission name is one this browser has an answer for at all. An unknown name is a
// `TypeError` from `permissions.query`, which is the specification's answer and is more useful than
// "denied": a page querying a capability that does not exist here has a bug rather than a refusal.
bool IsKnownPermission(std::string_view name);

// What `permissions.query` answers for a known name. Only `clipboard-write` is granted, and only
// because a copy button is a thing the user pressed -- the gesture requirement is enforced separately
// (ADR 0017's `isTrusted`), and this is the *steady-state* answer a page reads before it tries.
std::string_view PermissionStateFor(std::string_view name);

}  // namespace microbrowser::bindings
