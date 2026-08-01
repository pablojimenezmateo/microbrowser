#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include "ipc/Message.h"

// Every message from a renderer is attacker-controlled, and a paint frame is
// the largest and most structured of them: a display list carries paths,
// stroke styles, text runs and font requests, each with its own bounds.
//
// Three properties, checked rather than merely surviving:
//
//   1. Decoding terminates and never reads out of bounds (ASan and the
//      decoder's own accounting).
//   2. Anything that decodes re-encodes and decodes again to the same value.
//      That is what makes the wire format a *format* rather than whatever the
//      current decoder happens to accept: if a frame survives decoding, the
//      encoder must be able to produce it.
//   3. A decoded display list is internally consistent — every command's
//      index resolves. The in-memory list uses indices into side tables, and
//      the whole reason those indices do not cross the wire is that a hostile
//      frame naming a table entry that is not there would be an out-of-bounds
//      read. This checks the decoder never builds one anyway.
namespace {

void CheckListIsSelfConsistent(const microbrowser::gfx::DisplayList& list) {
  for (const microbrowser::gfx::DisplayCommand& command : list.Commands()) {
    if (const auto* fill = std::get_if<microbrowser::gfx::FillPathCommand>(&command)) {
      if (list.PathAt(fill->path) == nullptr) {
        __builtin_trap();
      }
    } else if (const auto* stroke =
                   std::get_if<microbrowser::gfx::StrokePathCommand>(&command)) {
      if (list.PathAt(stroke->path) == nullptr) {
        __builtin_trap();
      }
    } else if (const auto* text = std::get_if<microbrowser::gfx::DrawTextCommand>(&command)) {
      if (list.TextAt(text->text) == nullptr || list.FontAt(text->font) == nullptr) {
        __builtin_trap();
      }
    }
  }
  // Bounds must be computable without a font stack, and without tripping over
  // a value the decoder let through.
  (void)list.Bounds();
}

template <typename Message, typename Deserialize>
void RoundTrip(std::span<const std::byte> input, Deserialize deserialize) {
  const std::optional<Message> decoded = deserialize(input);
  if (!decoded.has_value()) {
    return;
  }
  // Only one direction carries a display list, and `if constexpr` says so at
  // compile time rather than instantiating get_if for a variant that has no
  // such alternative.
  if constexpr (std::is_same_v<Message, microbrowser::ipc::EngineToUi>) {
    if (const auto* paint = std::get_if<microbrowser::ipc::PaintFrameMessage>(&*decoded)) {
      CheckListIsSelfConsistent(paint->display_list);
    }
  }

  const std::vector<std::byte> reencoded = microbrowser::ipc::Serialize(*decoded);
  const std::optional<Message> again = deserialize(reencoded);
  if (!again.has_value()) {
    __builtin_trap();  // the encoder produced a frame its own decoder rejects
  }
  if (!(*again == *decoded)) {
    __builtin_trap();  // decode is not a fixed point of encode
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);

  // Both directions from the same bytes: which side a frame arrives on is not
  // something the frame gets to decide, and each decoder must reject the
  // other's messages rather than misread them.
  RoundTrip<microbrowser::ipc::EngineToUi>(input, microbrowser::ipc::DeserializeEngineToUi);
  RoundTrip<microbrowser::ipc::UiToEngine>(input, microbrowser::ipc::DeserializeUiToEngine);
  return 0;
}
