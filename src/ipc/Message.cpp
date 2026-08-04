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
  Pointer = 6,
  TextInput = 7,
  InputCommand = 8,
};

enum class EngineTag : std::uint8_t {
  PaintFrame = 1,
  TitleChanged = 2,
  LoadProgress = 3,
  NavigationCommitted = 4,
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
  } else if (const auto* pointer = std::get_if<PointerMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::Pointer));
    writer.WriteU8(static_cast<std::uint8_t>(pointer->kind));
    writer.WriteI32(pointer->position.x);
    writer.WriteI32(pointer->position.y);
    writer.WriteU8(pointer->button);
  } else if (const auto* text = std::get_if<TextInputMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::TextInput));
    writer.WriteString(text->text);
  } else {
    const auto& command = std::get<InputCommandMessage>(message);
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::InputCommand));
    writer.WriteU8(static_cast<std::uint8_t>(command.command));
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
  } else if (const auto* title = std::get_if<TitleChangedMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::TitleChanged));
    writer.WriteString(title->title);
  } else if (const auto* progress = std::get_if<LoadProgressMessage>(&message)) {
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::LoadProgress));
    writer.WriteF32(progress->fraction);
  } else {
    const auto& committed = std::get<NavigationCommittedMessage>(message);
    writer.WriteU8(static_cast<std::uint8_t>(EngineTag::NavigationCommitted));
    writer.WriteString(committed.url);
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
      message = value;
      break;
    }
    case UiTag::Pointer: {
      PointerMessage value;
      const std::uint8_t kind = reader.ReadU8();
      if (kind > static_cast<std::uint8_t>(PointerMessage::Kind::Up)) {
        return std::nullopt;
      }
      value.kind = static_cast<PointerMessage::Kind>(kind);
      value.position.x = reader.ReadI32();
      value.position.y = reader.ReadI32();
      value.button = reader.ReadU8();
      if (!reader.Ok()) {
        return std::nullopt;
      }
      // A pointer position is hit-tested against layout geometry, which is the
      // same coordinate range every rect is required to stay inside.
      if (!gfx::IsWithinDeviceRange(gfx::IntRect{value.position.x, value.position.y, 0, 0})) {
        return std::nullopt;
      }
      message = value;
      break;
    }
    case UiTag::TextInput: {
      TextInputMessage value;
      value.text = reader.ReadString();
      if (!reader.Ok()) {
        return std::nullopt;
      }
      message = std::move(value);
      break;
    }
    case UiTag::InputCommand: {
      InputCommandMessage value;
      const std::uint8_t command = reader.ReadU8();
      if (!reader.Ok() ||
          command > static_cast<std::uint8_t>(InputCommandMessage::Command::Enter)) {
        return std::nullopt;
      }
      value.command = static_cast<InputCommandMessage::Command>(command);
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
    default:
      return std::nullopt;
  }

  if (!FrameFullyConsumed(reader)) {
    return std::nullopt;
  }
  return message;
}

}  // namespace microbrowser::ipc
