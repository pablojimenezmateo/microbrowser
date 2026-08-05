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
inline constexpr std::uint32_t kProtocolVersion = 2;

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

struct PointerMessage {
  enum class Kind : std::uint8_t { Move, Down, Up };

  Kind kind = Kind::Move;
  gfx::IntPoint position;
  std::uint8_t button = 0;

  friend bool operator==(const PointerMessage&, const PointerMessage&) = default;
};

struct TextInputMessage {
  // UTF-8 text produced by a key event. Editing commands grow separate
  // messages; this one is only insertion text.
  std::string text;

  friend bool operator==(const TextInputMessage&, const TextInputMessage&) = default;
};

struct InputCommandMessage {
  enum class Command : std::uint8_t {
    Backspace,
    Delete,
    Enter,
  };

  Command command = Command::Backspace;

  friend bool operator==(const InputCommandMessage&, const InputCommandMessage&) = default;
};

using UiToEngine = std::variant<NavigateMessage,
                                ReloadMessage,
                                StopLoadMessage,
                                ResizeViewportMessage,
                                ScrollMessage,
                                PointerMessage,
                                TextInputMessage,
                                InputCommandMessage>;

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
