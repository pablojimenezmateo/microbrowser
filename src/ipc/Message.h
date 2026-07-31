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

inline constexpr std::uint32_t kProtocolVersion = 1;

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

  friend bool operator==(const ScrollMessage&, const ScrollMessage&) = default;
};

struct PointerMessage {
  enum class Kind : std::uint8_t { Move, Down, Up };

  Kind kind = Kind::Move;
  gfx::IntPoint position;
  std::uint8_t button = 0;

  friend bool operator==(const PointerMessage&, const PointerMessage&) = default;
};

using UiToEngine = std::variant<NavigateMessage,
                                ReloadMessage,
                                StopLoadMessage,
                                ResizeViewportMessage,
                                ScrollMessage,
                                PointerMessage>;

// --- Engine -> UI ------------------------------------------------------------

struct PaintFrameMessage {
  gfx::DisplayList display_list;
  // Device pixels the engine believes changed. Empty means "the whole
  // viewport", which is what a fresh navigation reports.
  std::vector<gfx::IntRect> damage;

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
