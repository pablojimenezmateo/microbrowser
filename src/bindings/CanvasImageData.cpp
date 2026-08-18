// `ImageData`, and the three context methods that produce or consume one.
//
// ADR 0029 §2. Split from `CanvasBindings.cpp` because it is a different object with a different
// lifetime: an `ImageData` outlives the context that made it, can be made without a context at all
// (`new ImageData(w, h)`), and is the one place a page holds canvas pixels as data rather than as a
// drawing.
//
// **`getImageData` is the fingerprinting surface ADR 0029 §2 names**, and the answer there is that
// this browser has one rasterizer, one shaper and a font set a page cannot enumerate -- so two copies
// of it produce the same pixels for the same input. What is enforced *here* is the other half, which
// is a security boundary rather than a privacy one: a canvas that has had cross-origin pixels drawn
// into it refuses every read for the rest of its life.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/Canvas.h"
#include "bindings/DomBindings.h"
#include "dom/Node.h"
#include "js/Interpreter.h"
#include "js/Value.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

constexpr const char* kContextCanvasSlot = "#context-canvas";
constexpr const char* kImageDataWidthSlot = "#imageDataWidth";
constexpr const char* kImageDataHeightSlot = "#imageDataHeight";
constexpr const char* kImageDataArraySlot = "#imageDataArray";

// A page chooses both dimensions, so `new ImageData(1e5, 1e5)` is 40GB. Sixteen megapixels is the
// same ceiling a canvas backing store has, for the same reason.
constexpr std::size_t kMaxImageDataBytes = 16u * 1024u * 1024u * 4u;

dom::Element* CanvasOf(const Value& context) {
  if (!context.IsObject()) {
    return nullptr;
  }
  const Value* canvas = context.object->GetOwn(kContextCanvasSlot);
  if (canvas == nullptr) {
    return nullptr;
  }
  dom::Node* node = NodeOf(*canvas);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

// `ToNumber` then the specification's `[EnforceRange] long` conversion. Not `static_cast<int>`: a
// double past `INT_MAX` is undefined behaviour on the way in, and the value comes from a page.
bool ToLong(const Value& value, int& out) {
  const double number = js::ToNumber(value);
  if (!std::isfinite(number) || number > 2147483647.0 || number < -2147483648.0) {
    return false;
  }
  out = static_cast<int>(number);
  return true;
}

// The bytes behind a `Uint8ClampedArray`, or null when the value is not one.
const js::BufferView* ViewOf(const Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  const js::BufferView* view = value.object->View();
  return view != nullptr && view->bytes != nullptr ? view : nullptr;
}

}  // namespace

void DomBindings::InstallImageData(const js::Value& prototype) {
  if (!prototype.IsObject() || canvas_ == nullptr) {
    return;
  }

  const Value get_image_data =
      interpreter_->NewNativeValue("getImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        if (call.arguments.size() < 4) {
          return call.Throw("TypeError", "getImageData requires four arguments");
        }
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        if (!ToLong(call.arguments[0], x) || !ToLong(call.arguments[1], y) ||
            !ToLong(call.arguments[2], width) || !ToLong(call.arguments[3], height)) {
          return call.Throw("TypeError", "the source rectangle is not representable");
        }
        if (width == 0 || height == 0) {
          // Zero is an `IndexSizeError` in the specification, and it is worth throwing rather than
          // handing back an empty buffer: a page that asked for zero pixels has a bug, and an empty
          // `data` array would let it run on.
          return ThrowDom(call, "IndexSizeError", "the source rectangle is empty");
        }
        // A negative extent flips the rectangle rather than failing, which is what the specification
        // says and what makes `getImageData(x, y, -w, -h)` the region above and to the left.
        if (width < 0) {
          x += width;
          width = -width;
        }
        if (height < 0) {
          y += height;
          height = -height;
        }
        // **The taint check, and it throws rather than answering.** A tainted canvas has had
        // cross-origin pixels drawn into it, and a page reading them would be reading an image it was
        // never allowed to see. `SecurityError` is the specified name and the one a page switches on.
        if (owner->canvas_->CanvasIsTainted(*element)) {
          return ThrowDom(call, "SecurityError", "the canvas has been tainted by cross-origin data");
        }
        const std::vector<std::uint8_t> pixels =
            owner->canvas_->ReadCanvasPixels(*element, x, y, width, height);
        if (pixels.empty()) {
          return call.Throw("RangeError", "the requested region is too large to read");
        }
        return owner->MakeImageData(width, height, pixels);
      });
  if (get_image_data.IsObject()) {
    get_image_data.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "getImageData", get_image_data);
  }

  const Value put_image_data =
      interpreter_->NewNativeValue("putImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        if (call.arguments.size() < 3) {
          return call.Throw("TypeError", "putImageData requires three arguments");
        }
        const Value data = call.arguments[0];
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        if (!data.IsObject()) {
          return call.Throw("TypeError", "putImageData expects an ImageData");
        }
        // The hidden slots rather than the public names: `width`, `height` and `data` are readonly
        // *accessors* on the prototype, and `Object::Get` on an accessor hands back the property
        // record's empty value rather than calling the getter. Reading them by name here is what made
        // every `putImageData` throw "expects an ImageData" at its own `getImageData` result.
        const Value* width_value = data.object->GetOwn(kImageDataWidthSlot);
        const Value* height_value = data.object->GetOwn(kImageDataHeightSlot);
        const Value* array = data.object->GetOwn(kImageDataArraySlot);
        if (width_value == nullptr || height_value == nullptr || array == nullptr) {
          return call.Throw("TypeError", "putImageData expects an ImageData");
        }
        const int width = static_cast<int>(js::ToNumber(*width_value));
        const int height = static_cast<int>(js::ToNumber(*height_value));
        const js::BufferView* view = ViewOf(*array);
        if (width <= 0 || height <= 0 || view == nullptr) {
          return call.Throw("TypeError", "putImageData expects an ImageData");
        }
        const std::size_t needed =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (view->offset > view->bytes->size() || needed > view->bytes->size() - view->offset) {
          return call.Throw("TypeError", "the ImageData's buffer is too small for its size");
        }
        const std::vector<std::uint8_t> rgba(
            view->bytes->begin() + static_cast<std::ptrdiff_t>(view->offset),
            view->bytes->begin() + static_cast<std::ptrdiff_t>(view->offset + needed));
        int dx = 0;
        int dy = 0;
        if (!ToLong(call.arguments[1], dx) || !ToLong(call.arguments[2], dy)) {
          return call.Throw("TypeError", "the destination is not representable");
        }
        owner->canvas_->WriteCanvasPixels(*element, dx, dy, width, height, rgba);
        return Value::Undefined();
      });
  if (put_image_data.IsObject()) {
    put_image_data.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "putImageData", put_image_data);
  }

  const Value create_image_data =
      interpreter_->NewNativeValue("createImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || call.arguments.empty()) {
          return call.Throw("TypeError", "createImageData requires an argument");
        }
        // Two overloads: `(sw, sh)` and `(imagedata)`. The second copies the *size* and not the
        // pixels, which is easy to get backwards -- the specification says the new buffer is
        // transparent black.
        int width = 0;
        int height = 0;
        if (call.arguments.size() == 1) {
          const Value source = call.arguments[0];
          if (!source.IsObject()) {
            return call.Throw("TypeError", "createImageData expects an ImageData or two sizes");
          }
          const Value* w = source.object->GetOwn(kImageDataWidthSlot);
          const Value* h = source.object->GetOwn(kImageDataHeightSlot);
          if (w == nullptr || h == nullptr) {
            return call.Throw("TypeError", "createImageData expects an ImageData or two sizes");
          }
          width = static_cast<int>(js::ToNumber(*w));
          height = static_cast<int>(js::ToNumber(*h));
        } else if (!ToLong(call.arguments[0], width) || !ToLong(call.arguments[1], height)) {
          return call.Throw("TypeError", "the size is not representable");
        }
        if (width == 0 || height == 0) {
          return ThrowDom(call, "IndexSizeError", "the size is empty");
        }
        width = std::abs(width);
        height = std::abs(height);
        const std::size_t needed =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (needed > kMaxImageDataBytes) {
          return call.Throw("RangeError", "the requested ImageData is too large");
        }
        return owner->MakeImageData(width, height, std::vector<std::uint8_t>(needed, 0));
      });
  if (create_image_data.IsObject()) {
    create_image_data.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "createImageData", create_image_data);
  }
}

js::Value DomBindings::ImageDataPrototype() {
  const Value prototype = MakeInterface("ImageData", Value::Undefined());
  if (!prototype.IsObject()) {
    return Value::Undefined();
  }
  if (prototype.object->HasOwn("width")) {
    return prototype;
  }
  // `width`, `height` and `data` are readonly attributes, so they are accessors on the prototype
  // rather than own data properties. `2d.imageData.object.readonly` assigns to each and checks the
  // value did not move; a writable own property would silently accept it and leave `data` describing
  // a buffer of another size.
  static constexpr struct {
    const char* name;
    const char* slot;
  } kAttributes[] = {
      {"width", kImageDataWidthSlot},
      {"height", kImageDataHeightSlot},
      {"data", kImageDataArraySlot},
  };
  for (const auto& attribute : kAttributes) {
    const std::string slot = attribute.slot;
    const Value get =
        interpreter_->NewNativeValue(attribute.name, [slot](NativeCall& call) -> Value {
          if (!call.self.IsObject()) {
            return Value::Undefined();
          }
          const Value* stored = call.self.object->GetOwn(slot);
          return stored == nullptr ? Value::Undefined() : *stored;
        });
    if (get.IsObject()) {
      prototype.object->DefineAccessor(attribute.name, get.object, nullptr);
    }
  }

  // The constructor. `MakeInterface` gives every interface one that throws "Illegal constructor",
  // which is right for `HTMLDivElement` and wrong here: `new ImageData(...)` is how a page builds
  // pixels without a canvas, and `2d.imageData.object.ctor.*` is seven test files on it alone.
  DomBindings* self = this;
  const Value constructor =
      interpreter_->NewNativeValue("ImageData", [self](NativeCall& call) -> Value {
        if (call.arguments.size() < 2) {
          return call.Throw("TypeError", "ImageData requires at least two arguments");
        }
        const js::BufferView* view = ViewOf(call.arguments[0]);
        int width = 0;
        int height = 0;
        if (view == nullptr) {
          // `(sw, sh)`: a fresh transparent-black buffer.
          if (!ToLong(call.arguments[0], width) || !ToLong(call.arguments[1], height)) {
            return call.Throw("TypeError", "the size is not representable");
          }
          if (width == 0 || height == 0) {
            return ThrowDom(call, "IndexSizeError", "the size is empty");
          }
          if (width < 0 || height < 0) {
            return ThrowDom(call, "IndexSizeError", "the size is negative");
          }
          const std::size_t needed =
              static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
          if (needed > kMaxImageDataBytes) {
            return call.Throw("RangeError", "the requested ImageData is too large");
          }
          return self->MakeImageData(width, height, std::vector<std::uint8_t>(needed, 0));
        }
        // `(data, sw)` or `(data, sw, sh)`: the buffer is *adopted*, not copied, which is what makes
        // `new ImageData(existing.data, w)` a view onto the same pixels a page has been filling.
        const std::size_t length = view->bytes->size() - std::min(view->offset, view->bytes->size());
        if (length % 4 != 0) {
          return ThrowDom(call, "InvalidStateError", "the buffer's length is not a multiple of 4");
        }
        if (!ToLong(call.arguments[1], width) || width <= 0) {
          return ThrowDom(call, "IndexSizeError", "the width is not positive");
        }
        const std::size_t pixels = length / 4;
        if (pixels % static_cast<std::size_t>(width) != 0) {
          return ThrowDom(call, "IndexSizeError", "the buffer does not divide by the width");
        }
        const std::size_t rows = pixels / static_cast<std::size_t>(width);
        if (call.arguments.size() > 2) {
          if (!ToLong(call.arguments[2], height) || height <= 0) {
            return ThrowDom(call, "IndexSizeError", "the height is not positive");
          }
          if (static_cast<std::size_t>(height) != rows) {
            return ThrowDom(call, "IndexSizeError", "the buffer does not match the given height");
          }
        } else {
          height = static_cast<int>(rows);
        }
        return self->AdoptImageData(width, height, call.arguments[0]);
      });
  if (constructor.IsObject()) {
    DefinePrototypeSlot(constructor.object, prototype);
    DefineNonEnumerable(prototype.object, "constructor", constructor);
    interpreter_->GlobalScope()->Declare("ImageData", constructor, false);
  }
  return prototype;
}

js::Value DomBindings::AdoptImageData(int width, int height, const js::Value& array) {
  const Value prototype = ImageDataPrototype();
  const Value image_data = interpreter_->NewObjectValue();
  if (!image_data.IsObject() || !prototype.IsObject()) {
    return Value::Undefined();
  }
  image_data.object->SetPrototype(prototype.object);
  image_data.object->SetHidden(kImageDataWidthSlot, Value::Number(static_cast<double>(width)));
  image_data.object->SetHidden(kImageDataHeightSlot, Value::Number(static_cast<double>(height)));
  image_data.object->SetHidden(kImageDataArraySlot, array);
  return image_data;
}

js::Value DomBindings::MakeImageData(int width, int height,
                                     const std::vector<std::uint8_t>& rgba) {
  // The `data` is a real `Uint8ClampedArray`, built through the real constructor, so the array a page
  // gets behaves like one -- indexable, iterable, with a `buffer`. A plain object with numeric keys
  // would pass a naive test and fail every real filter, which is the shape of stub ADR 0012 forbids.
  const Value* constructor = interpreter_->GlobalScope()->Lookup("Uint8ClampedArray");
  if (constructor == nullptr || !constructor->IsObject()) {
    return Value::Undefined();
  }
  const js::Result made = interpreter_->ConstructValue(
      *constructor, {Value::Number(static_cast<double>(rgba.size()))});
  if (made.completion == js::Completion::Throw || !made.value.IsObject()) {
    return Value::Undefined();
  }
  const js::BufferView* view = made.value.object->View();
  if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= rgba.size()) {
    std::copy(rgba.begin(), rgba.end(), view->bytes->begin());
  }
  return AdoptImageData(width, height, made.value);
}

}  // namespace microbrowser::bindings
