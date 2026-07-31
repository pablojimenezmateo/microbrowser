#include "ipc/Message.h"

#include "ipc/ByteStream.h"
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
};

enum class EngineTag : std::uint8_t {
  PaintFrame = 1,
  TitleChanged = 2,
  LoadProgress = 3,
  NavigationCommitted = 4,
};

// Display-list command tags, same reasoning.
enum class CommandTag : std::uint8_t {
  FillRect = 1,
  PushClip = 2,
  PopClip = 3,
};

void WriteRect(ByteWriter& writer, const gfx::IntRect& rect) {
  writer.WriteI32(rect.x);
  writer.WriteI32(rect.y);
  writer.WriteI32(rect.width);
  writer.WriteI32(rect.height);
}

gfx::IntRect ReadRect(ByteReader& reader) {
  gfx::IntRect rect;
  rect.x = reader.ReadI32();
  rect.y = reader.ReadI32();
  rect.width = reader.ReadI32();
  rect.height = reader.ReadI32();
  return rect;
}

constexpr std::size_t kMinBytesPerCommand = 1;
constexpr std::size_t kBytesPerRect = 16;

void WriteDisplayList(ByteWriter& writer, const gfx::DisplayList& list) {
  writer.WriteU32(static_cast<std::uint32_t>(list.Size()));
  for (const gfx::DisplayCommand& command : list.Commands()) {
    if (const auto* fill = std::get_if<gfx::FillRectCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::FillRect));
      WriteRect(writer, fill->rect);
      writer.WriteU32(fill->color.argb);
    } else if (const auto* push = std::get_if<gfx::PushClipCommand>(&command)) {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PushClip));
      WriteRect(writer, push->rect);
    } else {
      writer.WriteU8(static_cast<std::uint8_t>(CommandTag::PopClip));
    }
  }
}

// Decoding replays the wire commands through the DisplayList builder rather
// than appending them raw. That normalizes as it decodes: a degenerate fill
// (empty rect, zero alpha) from a malformed or hostile frame is dropped instead
// of becoming a no-op command that every later frame diff has to carry. Round
// trips of lists built through the public API are exact, because the builder
// never emits a degenerate fill in the first place.
bool ReadDisplayList(ByteReader& reader, gfx::DisplayList& out) {
  const std::optional<std::uint32_t> count = reader.ReadCount(kMinBytesPerCommand);
  if (!count.has_value()) {
    return false;
  }
  for (std::uint32_t i = 0; i < *count; ++i) {
    const auto tag = static_cast<CommandTag>(reader.ReadU8());
    if (!reader.Ok()) {
      return false;
    }
    switch (tag) {
      case CommandTag::FillRect: {
        const gfx::IntRect rect = ReadRect(reader);
        const gfx::Color color{reader.ReadU32()};
        if (!reader.Ok()) {
          return false;
        }
        out.FillRect(rect, color);
        break;
      }
      case CommandTag::PushClip: {
        const gfx::IntRect rect = ReadRect(reader);
        if (!reader.Ok()) {
          return false;
        }
        out.PushClip(rect);
        break;
      }
      case CommandTag::PopClip:
        out.PopClip();
        break;
      default:
        return false;
    }
  }
  return reader.Ok();
}

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
  } else {
    const auto& pointer = std::get<PointerMessage>(message);
    writer.WriteU8(static_cast<std::uint8_t>(UiTag::Pointer));
    writer.WriteU8(static_cast<std::uint8_t>(pointer.kind));
    writer.WriteI32(pointer.position.x);
    writer.WriteI32(pointer.position.y);
    writer.WriteU8(pointer.button);
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
        value.damage.push_back(ReadRect(reader));
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
