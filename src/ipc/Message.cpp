#include "ipc/Message.h"

#include <cmath>
#include <span>

#include "ipc/ByteStream.h"
#include "ipc/DisplayListCodec.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::ipc {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Tags are explicit constants, not the variant index. A variant index shifts
// when someone inserts an alternative, which would silently reinterpret every
// in-flight frame; an explicit tag makes that a compile-time conflict instead.
enum class UiTag : std::uint8_t {
  Navigate = 1,
  Reload = 2,
  StopLoad = 3,
  ResizeViewport = 4,
  Scroll = 5,
  // 6, 7 and 8 were Pointer, TextInput and InputCommand. Reused rather than
  // retired because the protocol version moved with them: a peer old enough to
  // send the previous tag 6 is refused by the version check before the tag is
  // ever read.
  PointerInput = 6,
  KeyInput = 7,
  TraverseHistory = 8,
};

enum class EngineTag : std::uint8_t {
  PaintFrame = 1,
  TitleChanged = 2,
  LoadProgress = 3,
  NavigationCommitted = 4,
  HistoryState = 5,
};

// A viewport edge, and the physical-pixels-per-CSS-pixel scale. Both are far
// past anything real -- no display is 65536 pixels wide, and no device has a
// scale factor of 64 -- and both keep the products they feed inside their
// types.
constexpr int kMaxViewportEdge = 65536;
constexpr float kMaxDeviceScale = 64.0f;

// Shared frame envelope. Returns nullopt unless the version matches and at
// least a tag byte follows.
std::optional<std::uint8_t> ReadFrameHeader(ByteReader& reader) {
  if (reader.ReadU32() != kProtocolVersion) {
    return std::nullopt;
  }
  const std::uint8_t tag = reader.ReadU8();
  if (!reader.Ok()) {
    return std::nullopt;
  }
  return tag;
}

// A frame that decoded but left bytes behind means the two ends disagree about
// the payload shape. Surfacing that as a decode failure is the whole point of
// having a version field.
bool FrameFullyConsumed(const ByteReader& reader) {
  return reader.Ok() && reader.AtEnd();
}

void WriteModifiers(ByteWriter& writer, const InputModifiers& modifiers) {
  writer.WriteU8(modifiers.control ? 1u : 0u);
  writer.WriteU8(modifiers.shift ? 1u : 0u);
  writer.WriteU8(modifiers.alt ? 1u : 0u);
  writer.WriteU8(modifiers.meta ? 1u : 0u);
}

// Any nonzero byte is true. A bool that only accepts 0 and 1 would make a
// frame from a peer that writes 0xFF for true a decode failure rather than a
// held control key, and there is nothing to confuse here: the field is one bit
// of meaning however it was spelled.
InputModifiers ReadModifiers(ByteReader& reader) {
  InputModifiers modifiers;
  modifiers.control = reader.ReadU8() != 0;
  modifiers.shift = reader.ReadU8() != 0;
  modifiers.alt = reader.ReadU8() != 0;
  modifiers.meta = reader.ReadU8() != 0;
  return modifiers;
}

std::vector<std::byte> FinishFrame(ByteWriter& writer) {
  AddPerformanceCounter(PerfCounterId::IpcBytesSerialized,
                        static_cast<std::uint64_t>(writer.Size()));
  return writer.Take();
}

}  // namespace

std::vector<std::byte> Serialize(const UiToEngine& message) {
  ByteWriter writer;
  writer.WriteU32(kProtocolVersion);

  if (const auto* navigate = std::get_if<NavigateMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::Navigate));
    writer.WriteString(navigate->url);
  } else if (const auto* reload = std::get_if<ReloadMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::Reload));
    writer.WriteU8(reload->bypass_cache ? 1u : 0u);
  } else if (std::holds_alternative<StopLoadMessage>(message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::StopLoad));
  } else if (const auto* resize = std::get_if<ResizeViewportMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::ResizeViewport));
    writer.WriteI32(resize->size.width);
    writer.WriteI32(resize->size.height);
    writer.WriteF32(resize->device_scale);
  } else if (const auto* scroll = std::get_if<ScrollMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::Scroll));
    writer.WriteI32(scroll->delta_x);
    writer.WriteI32(scroll->delta_y);
    writer.WriteI32(scroll->position.x);
    writer.WriteI32(scroll->position.y);
  } else if (const auto* pointer = std::get_if<PointerInputMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::PointerInput));
    writer.WriteU8(static_cast<std::uint8_t>(pointer->kind));
    writer.WriteF32(pointer->position.x);
    writer.WriteF32(pointer->position.y);
    writer.WriteI32(pointer->pointer_id);
    writer.WriteU8(static_cast<std::uint8_t>(pointer->type));
    writer.WriteU16(pointer->buttons);
    writer.WriteU8(pointer->button);
    WriteModifiers(writer, pointer->modifiers);
  } else if (const auto* traverse = std::get_if<TraverseHistoryMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::TraverseHistory));
    writer.WriteI32(traverse->delta);
  } else {
    const auto& key = std::get<KeyInputMessage>(message);
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::KeyInput));
    writer.WriteU8(static_cast<std::uint8_t>(key.kind));
    writer.WriteString(key.code);
    writer.WriteString(key.key);
    writer.WriteString(key.text);
    WriteModifiers(writer, key.modifiers);
    writer.WriteU8(key.repeat ? 1u : 0u);
  }

  return FinishFrame(writer);
}

std::vector<std::byte> Serialize(const EngineToUi& message) {
  ByteWriter writer;
  writer.WriteU32(kProtocolVersion);

  if (const auto* paint = std::get_if<PaintFrameMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::PaintFrame));
    WriteDisplayList(writer, paint->display_list);
    writer.WriteU32(static_cast<std::uint32_t>(paint->damage.size()));
    for (const gfx::IntRect& rect : paint->damage) {
      WriteRect(writer, rect);
    }
    writer.WriteI32(paint->scroll_delta.x);
    writer.WriteI32(paint->scroll_delta.y);
  } else if (const auto* title = std::get_if<TitleChangedMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::TitleChanged));
    writer.WriteString(title->title);
  } else if (const auto* progress = std::get_if<LoadProgressMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::LoadProgress));
    writer.WriteF32(progress->fraction);
  } else if (const auto* committed = std::get_if<NavigationCommittedMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::NavigationCommitted));
    writer.WriteString(committed->url);
  } else {
    const auto& history = std::get<HistoryStateMessage>(message);
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::HistoryState));
    writer.WriteU8(history.can_go_back ? 1u : 0u);
    writer.WriteU8(history.can_go_forward ? 1u : 0u);
  }

  return FinishFrame(writer);
}

std::optional<UiToEngine> DeserializeUiToEngine(std::span<const std::byte> bytes) {
  AddPerformanceCounter(PerfCounterId::IpcBytesDeserialized,
                        static_cast<std::uint64_t>(bytes.size()));
  ByteReader reader(bytes);
  const std::optional<std::uint8_t> tag = ReadFrameHeader(reader);
  if (!tag.has_value()) {
    return std::nullopt;
  }

  UiToEngine message;
  switch (static_cast<UiTag>(*tag)) {
    case UiTag::Navigate: {
      NavigateMessage value;
      value.url = reader.ReadString();
      message = std::move(value);
      break;
    }
    case UiTag::Reload: {
      ReloadMessage value;
      value.bypass_cache = reader.ReadU8() != 0;
      message = value;
      break;
    }
    case UiTag::StopLoad:
      message = StopLoadMessage{};
      break;
    case UiTag::ResizeViewport: {
      ResizeViewportMessage value;
      value.size.width = reader.ReadI32();
      value.size.height = reader.ReadI32();
      value.device_scale = reader.ReadF32();
      if (!reader.Ok()) {
        return std::nullopt;
      }
      // A viewport size becomes a canvas allocation of width * height * 4, so
      // a negative or enormous edge is an overflow with an attacker's hand on
      // it. kMaxViewportEdge is far past any real display and still leaves the
      // product inside a 64-bit size_t with room to spare.
      if (value.size.width < 0 || value.size.height < 0 ||
          value.size.width > kMaxViewportEdge || value.size.height > kMaxViewportEdge) {
        return std::nullopt;
      }
      // A NaN scale multiplies into every layout coordinate and compares false
      // against every bound it is checked with, which is how a NaN gets past
      // range checks and into the rasterizer.
      if (!std::isfinite(value.device_scale) || value.device_scale <= 0.0f ||
          value.device_scale > kMaxDeviceScale) {
        return std::nullopt;
      }
      message = value;
      break;
    }
    case UiTag::Scroll: {
      ScrollMessage value;
      value.delta_x = reader.ReadI32();
      value.delta_y = reader.ReadI32();
      value.position.x = reader.ReadI32();
      value.position.y = reader.ReadI32();
      message = value;
      break;
    }
    case UiTag::PointerInput: {
      PointerInputMessage value;
      const std::uint8_t kind = reader.ReadU8();
      if (kind > static_cast<std::uint8_t>(PointerInputMessage::Kind::Up)) {
        return std::nullopt;
      }
      value.kind = static_cast<PointerInputMessage::Kind>(kind);
      value.position.x = reader.ReadF32();
      value.position.y = reader.ReadF32();
      value.pointer_id = reader.ReadI32();
      const std::uint8_t type = reader.ReadU8();
      if (type > static_cast<std::uint8_t>(PointerInputMessage::Type::Touch)) {
        return std::nullopt;
      }
      value.type = static_cast<PointerInputMessage::Type>(type);
      value.buttons = reader.ReadU16();
      value.button = reader.ReadU8();
      value.modifiers = ReadModifiers(reader);
      if (!reader.Ok()) {
        return std::nullopt;
      }
      // A pointer position is hit-tested against layout geometry, so it is held
      // to the same coordinate range every rect is. NaN is checked first and
      // separately, because a NaN compares false against every bound that would
      // otherwise have caught it -- which is how one gets past a range check and
      // into the rasterizer.
      if (!std::isfinite(value.position.x) || !std::isfinite(value.position.y) ||
          std::fabs(value.position.x) > static_cast<float>(gfx::kMaxDeviceCoordinate) ||
          std::fabs(value.position.y) > static_cast<float>(gfx::kMaxDeviceCoordinate)) {
        return std::nullopt;
      }
      message = value;
      break;
    }
    case UiTag::KeyInput: {
      KeyInputMessage value;
      const std::uint8_t kind = reader.ReadU8();
      if (kind > static_cast<std::uint8_t>(KeyInputMessage::Kind::Up)) {
        return std::nullopt;
      }
      value.kind = static_cast<KeyInputMessage::Kind>(kind);
      value.code = reader.ReadString();
      value.key = reader.ReadString();
      value.text = reader.ReadString();
      value.modifiers = ReadModifiers(reader);
      value.repeat = reader.ReadU8() != 0;
      if (!reader.Ok()) {
        return std::nullopt;
      }
      // Each of these becomes a JavaScript string a page reads, and `text`
      // becomes characters inserted into a control. A keyboard names one key and
      // inserts at most a grapheme cluster; a megabyte here is a sender that is
      // not a keyboard, and refusing it is cheaper than every consumer having to
      // remember it might be.
      if (value.code.size() > kMaxKeyNameBytes || value.key.size() > kMaxKeyNameBytes ||
          value.text.size() > kMaxKeyTextBytes) {
        return std::nullopt;
      }
      message = std::move(value);
      break;
    }
    case UiTag::TraverseHistory: {
      TraverseHistoryMessage value;
      value.delta = reader.ReadI32();
      if (!reader.Ok()) {
        return std::nullopt;
      }
      // A delta a sender can make arbitrarily large is one the history clamps
      // to nothing anyway, so there is no bound to add here: SessionHistory::Go
      // refuses a target outside the list rather than clamping to its end.
      message = value;
      break;
    }
    default:
      return std::nullopt;
  }

  if (!FrameFullyConsumed(reader)) {
    return std::nullopt;
  }
  return message;
}

std::optional<EngineToUi> DeserializeEngineToUi(std::span<const std::byte> bytes) {
  AddPerformanceCounter(PerfCounterId::IpcBytesDeserialized,
                        static_cast<std::uint64_t>(bytes.size()));
  ByteReader reader(bytes);
  const std::optional<std::uint8_t> tag = ReadFrameHeader(reader);
  if (!tag.has_value()) {
    return std::nullopt;
  }

  EngineToUi message;
  switch (static_cast<EngineTag>(*tag)) {
    case EngineTag::PaintFrame: {
      PaintFrameMessage value;
      if (!ReadDisplayList(reader, value.display_list)) {
        return std::nullopt;
      }
      const std::optional<std::uint32_t> count = reader.ReadCount(kBytesPerRect);
      if (!count.has_value()) {
        return std::nullopt;
      }
      value.damage.reserve(*count);
      for (std::uint32_t i = 0; i < *count; ++i) {
        gfx::IntRect rect;
        if (!ReadRect(reader, rect)) {
          return std::nullopt;
        }
        value.damage.push_back(rect);
      }
      value.scroll_delta.x = reader.ReadI32();
      value.scroll_delta.y = reader.ReadI32();
      message = std::move(value);
      break;
    }
    case EngineTag::TitleChanged: {
      TitleChangedMessage value;
      value.title = reader.ReadString();
      message = std::move(value);
      break;
    }
    case EngineTag::LoadProgress: {
      LoadProgressMessage value;
      value.fraction = reader.ReadF32();
      if (!reader.Ok() || !std::isfinite(value.fraction) || value.fraction < 0.0f ||
          value.fraction > 1.0f) {
        // A fraction is a fraction. Out of range it drives a progress bar's
        // width, and NaN makes every clamp that would have caught it compare
        // false.
        return std::nullopt;
      }
      message = value;
      break;
    }
    case EngineTag::NavigationCommitted: {
      NavigationCommittedMessage value;
      value.url = reader.ReadString();
      message = std::move(value);
      break;
    }
    case EngineTag::HistoryState: {
      HistoryStateMessage value;
      value.can_go_back = reader.ReadU8() != 0;
      value.can_go_forward = reader.ReadU8() != 0;
      if (!reader.Ok()) {
        return std::nullopt;
      }
      message = value;
      break;
    }
    default:
      return std::nullopt;
  }

  if (!FrameFullyConsumed(reader)) {
    return std::nullopt;
  }
  return message;
}

}  // namespace microbrowser::ipc
