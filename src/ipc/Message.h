#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "gfx/DisplayList.h"
#include "gfx/Geometry.h"

namespace microbrowser::ipc {

// The complete vocabulary crossing the UI/Engine boundary.
//
// Nothing else may cross. The UI half of the process must not name a dom::,
// css::, layout::, html:: or js:: type, and the engine half must not name a
// window, a texture, or an SDL anything. That rule is what keeps moving the
// engine into a sandboxed process a scheduling decision rather than a rewrite —
// and it is enforced by the ArchitectureInvariants lint, not by good intentions.
//
// Every message serializes even though M0 delivers them through an in-process
// queue and never encodes anything. That is deliberate: a message that cannot
// be serialized is a message that quietly holds a pointer, and the round-trip
// test is what catches it on the commit that introduces it rather than a year
// later.

// Bumped to 2 when a display list gained a resource table for its images and a
// hole for a compositor surface. Both are wire-format changes rather than
// additions, so an old peer must be refused rather than left to misread a
// length: the version check is the whole mechanism for that.
//
// Bumped to 3 when the three input messages became the two ADR 0017 §1
// describes. Tag 6 means something different now and tags 7 and 8 mean nothing,
// which is precisely the case the version field exists for.
inline constexpr std::uint32_t kProtocolVersion = 3;

// --- UI -> Engine ------------------------------------------------------------

struct NavigateMessage {
  std::string url;

  friend bool operator==(const NavigateMessage&, const NavigateMessage&) = default;
};

struct ReloadMessage {
  bool bypass_cache = false;

  friend bool operator==(const ReloadMessage&, const ReloadMessage&) = default;
};

struct StopLoadMessage {
  friend bool operator==(const StopLoadMessage&, const StopLoadMessage&) = default;
};

struct ResizeViewportMessage {
  gfx::IntSize size;
  // Physical pixels per CSS pixel. Carried explicitly rather than read from the
  // window, because the engine has no window to read it from.
  float device_scale = 1.0f;

  friend bool operator==(const ResizeViewportMessage&, const ResizeViewportMessage&) = default;
};

struct ScrollMessage {
  int delta_x = 0;
  int delta_y = 0;
  // Where the pointer was, in page coordinates. A wheel is routed to the
  // deepest scrolling box under it that can still move and chains outward when
  // it cannot -- ADR 0018 §4 -- so a delta with no position is a wheel that can
  // only ever scroll the document.
  gfx::IntPoint position;

  friend bool operator==(const ScrollMessage&, const ScrollMessage&) = default;
};

// What was held down when the input happened.
//
// Named bools rather than a bitmask, and ADR 0017 §1 is explicit about why: it
// crosses the seam from what will eventually be the untrusted process, and a
// bitmask whose meaning is a convention is exactly the kind of field ADR 0004
// says to treat as a claim rather than a fact. Four bools decode to four
// booleans; an integer decodes to whatever the two ends currently agree it
// means.
struct InputModifiers {
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;

  friend bool operator==(const InputModifiers&, const InputModifiers&) = default;
};

// A pointer, as an input event actually is. ADR 0017 §1.
//
// The kinds here are the ones something sends. `Enter`/`Leave` belong with
// `:hover` and `Cancel` with touch, and each arrives with the thing that
// produces it -- an enum value no sender ever writes is decode surface with no
// behaviour behind it. A wheel is still `ScrollMessage`, which session 8 built
// and which routes to a scrolling box rather than to an event target.
struct PointerInputMessage {
  enum class Kind : std::uint8_t { Move, Down, Up };
  enum class Type : std::uint8_t { Mouse, Pen, Touch };

  Kind kind = Kind::Move;
  // CSS pixels, viewport-relative. CSS pixels because that is the coordinate
  // system every answer the engine gives about this point is in -- a rect from
  // `getBoundingClientRect`, a `clientX` on the event -- and converting once at
  // the seam is what stops the device scale being applied twice or not at all.
  gfx::FloatPoint position;
  // A mouse is one pointer; touches are many. Carried now so that the day a
  // touchscreen exists is not the day this message changes shape.
  std::int32_t pointer_id = 1;
  Type type = Type::Mouse;
  // What is held, as a bitmask in the DOM's order (1 = primary, 2 = secondary,
  // 4 = middle) -- this one *is* a bitmask because it is the value a page reads
  // back as `event.buttons`.
  std::uint16_t buttons = 0;
  // What changed. 0 is the primary button, which is the DOM's numbering and
  // not the platform's.
  std::uint8_t button = 0;
  InputModifiers modifiers;

  friend bool operator==(const PointerInputMessage&, const PointerInputMessage&) = default;
};

// A key. ADR 0017 §1: three strings, not one.
//
// A game reads `code` because WASD is a shape on the keyboard; a shortcut reads
// `key` because Ctrl+C is a letter; an editor reads `text` because a dead key
// produces nothing until the next one. Collapsing them -- which is what the
// message this replaces did -- makes two of the three unimplementable.
struct KeyInputMessage {
  enum class Kind : std::uint8_t { Down, Up };

  Kind kind = Kind::Down;
  // The physical key: "KeyA", "Escape". Layout-independent, and empty when the
  // platform did not say which key it was.
  std::string code;
  // What it means: "a", "A", "Escape". Layout-dependent.
  std::string key;
  // What it inserts, possibly empty. Inserting it is a *default action* of the
  // keydown, not something that happens on the way past.
  std::string text;
  InputModifiers modifiers;
  bool repeat = false;

  friend bool operator==(const KeyInputMessage&, const KeyInputMessage&) = default;
};

// Bounds on the three strings above. A key names one key and inserts at most a
// grapheme cluster; anything longer is a sender that is not a keyboard, and the
// engine turns each of these into a JavaScript string a page can read.
inline constexpr std::uint32_t kMaxKeyNameBytes = 32;
inline constexpr std::uint32_t kMaxKeyTextBytes = 64;

using UiToEngine = std::variant<NavigateMessage,
                                ReloadMessage,
                                StopLoadMessage,
                                ResizeViewportMessage,
                                ScrollMessage,
                                PointerInputMessage,
                                KeyInputMessage>;

// --- Engine -> UI ------------------------------------------------------------

struct PaintFrameMessage {
  gfx::DisplayList display_list;
  // Device pixels the engine believes changed. Empty means "the whole
  // viewport", which is what a fresh navigation reports.
  std::vector<gfx::IntRect> damage;

  // How far this frame is the previous frame, moved. Nonzero only when the
  // document scrolled and nothing else did, and it is the receiver's licence to
  // **blit** rather than repaint: copy the overlap within its own surface, then
  // paint the damage above. ADR 0018 §2 -- a scroll is a paint, and the paint it
  // is proportional to the newly exposed strip rather than to the window.
  //
  // Advisory, never trusted. The receiver clamps it to its own surface and
  // falls back to a full repaint for anything it cannot honour, because after
  // the process split this number arrives from a renderer and a blit driven by
  // an unchecked offset is a read out of somebody else's memory.
  gfx::IntPoint scroll_delta;

  friend bool operator==(const PaintFrameMessage&, const PaintFrameMessage&) = default;
};

struct TitleChangedMessage {
  std::string title;

  friend bool operator==(const TitleChangedMessage&, const TitleChangedMessage&) = default;
};

struct LoadProgressMessage {
  // 0.0 to 1.0. Reaching 1.0 is what ends a load; there is no separate
  // "finished" message to get out of sync with it.
  float fraction = 0.0f;

  friend bool operator==(const LoadProgressMessage&, const LoadProgressMessage&) = default;
};

struct NavigationCommittedMessage {
  // The URL after redirects and after the privacy layer's sanitization — what
  // the omnibox must show. Never the URL that was requested.
  std::string url;

  friend bool operator==(const NavigationCommittedMessage&,
                         const NavigationCommittedMessage&) = default;
};

using EngineToUi = std::variant<PaintFrameMessage,
                                TitleChangedMessage,
                                LoadProgressMessage,
                                NavigationCommittedMessage>;

// --- Wire format -------------------------------------------------------------
//
// Frame layout: [u32 version][u8 tag][payload]. Deserialize returns nullopt for
// a version mismatch, an unknown tag, a truncated payload, or trailing bytes.
// Trailing bytes are rejected rather than ignored so a length confusion between
// the two ends surfaces as a decode failure instead of a silently dropped field.

std::vector<std::byte> Serialize(const UiToEngine& message);
std::vector<std::byte> Serialize(const EngineToUi& message);

std::optional<UiToEngine> DeserializeUiToEngine(std::span<const std::byte> bytes);
std::optional<EngineToUi> DeserializeEngineToUi(std::span<const std::byte> bytes);

}  // namespace microbrowser::ipc
