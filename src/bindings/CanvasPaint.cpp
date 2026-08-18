// The canvas paint sources: gradients, patterns, and `drawImage`.
//
// ADR 0029 §2. Its own translation unit because it is the half of the 2D context that needs *pixels*
// rather than geometry -- a gradient is a colour per pixel, a pattern and a `drawImage` are somebody
// else's pixels -- and because `CanvasBindings.cpp` was over its module's line cap without it.
//
// **A gradient object is a handle, not a copy.** `src/bindings` cannot hold a `gfx::Paint`, so this
// mints a number, tells the far side what it names, and hands back an object carrying it. Which
// handle is selected as the fill is far-side state, which is what makes `save()` and `restore()`
// carry gradients with no second copy of anything on this side -- the only thing kept here is the
// map from handle to the *object the page was given*, because the specification says reading
// `fillStyle` back returns that same object.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/Canvas.h"
#include "bindings/CanvasSupport.h"
#include "bindings/DomBindings.h"
#include "dom/Node.h"
#include "js/Interpreter.h"
#include "js/Value.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// The element behind a `CanvasImageSource`, or null when the value is not one.
//
// The tags this browser can produce pixels for, and deliberately not "any element with a wrapper":
// `drawImage(div, 0, 0)` is a `TypeError` in every browser, and accepting it would draw nothing
// while telling the page it had worked.
dom::Element* CanvasImageSourceOf(const Value& value) {
  dom::Node* node = NodeOf(value);
  if (node == nullptr || !node->IsElement()) {
    return nullptr;
  }
  auto& element = static_cast<dom::Element&>(*node);
  const std::string& tag = element.TagName();
  if (tag != "img" && tag != "canvas" && tag != "video" && tag != "svg") {
    return nullptr;
  }
  return &element;
}

// The next handle for this context. Per context rather than global, so one page's handles cannot
// collide with another's and a handle is meaningless outside the canvas that minted it.
std::uint32_t MintPaintHandle(const Value& context) {
  const Value* counter = context.object->GetOwn(kPaintCounterSlot);
  const auto next =
      static_cast<std::uint32_t>((counter == nullptr ? 0.0 : js::ToNumber(*counter)) + 1.0);
  context.object->SetHidden(kPaintCounterSlot, Value::Number(static_cast<double>(next)));
  return next;
}

}  // namespace

void DomBindings::InstallCanvasPaintSources(const js::Value& prototype) {
  // Eagerly, not at the first `createLinearGradient`. `window.CanvasGradient.prototype` is what a
  // page patches to extend every gradient it will ever make, and it patches it *before* making one.
  InstallCanvasGradientInterface();
  // `createLinearGradient`, `createRadialGradient` and `createConicGradient`. One native for the
  // three, because they differ only in how many numbers they take and which command they send.
  struct Maker {
    const char* name;
    CanvasOp::Kind kind;
    std::size_t required;
  };
  static constexpr Maker kMakers[] = {
      {"createLinearGradient", CanvasOp::Kind::CreateLinearGradient, 4},
      {"createRadialGradient", CanvasOp::Kind::CreateRadialGradient, 6},
      {"createConicGradient", CanvasOp::Kind::CreateConicGradient, 3},
  };
  for (const Maker& maker : kMakers) {
    const Value native = interpreter_->NewNativeValue(maker.name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOfContext(call.self);
      const Value* tag = call.callee == nullptr ? nullptr : call.callee->GetOwn("#op");
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      const Value* named = call.callee == nullptr ? nullptr : call.callee->GetOwn("#name");
      if (tag == nullptr || arity == nullptr) {
        return Value::Undefined();
      }
      Value thrown;
      if (!CanvasArity(call, static_cast<std::size_t>(js::ToNumber(*arity)),
                 named == nullptr ? "this method" : js::ToString(*named).c_str(), thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      op.kind = static_cast<CanvasOp::Kind>(static_cast<int>(js::ToNumber(*tag)));
      op.a = CanvasNumber(call.arguments, 0);
      op.b = CanvasNumber(call.arguments, 1);
      op.c = CanvasNumber(call.arguments, 2);
      op.d = CanvasNumber(call.arguments, 3);
      op.e = CanvasNumber(call.arguments, 4);
      op.f = CanvasNumber(call.arguments, 5);
      if (!std::isfinite(op.a) || !std::isfinite(op.b) || !std::isfinite(op.c) ||
          !std::isfinite(op.d) || !std::isfinite(op.e) || !std::isfinite(op.f)) {
        // A non-finite coordinate is *not* a silent no-op here, unlike a drawing command: the call
        // has to return an object, and one built from a NaN would paint nothing for the rest of the
        // page's life with no way to find out why.
        return ThrowDom(call, "NotSupportedError", "a gradient coordinate is not finite");
      }
      if (op.kind == CanvasOp::Kind::CreateRadialGradient && (op.c < 0.0 || op.f < 0.0)) {
        return ThrowDom(call, "IndexSizeError", "a gradient radius is negative");
      }
      return owner->MakeCanvasGradient(call.self, *element, op);
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->SetHidden("#op", Value::Number(static_cast<double>(maker.kind)));
      native.object->SetHidden("#arity", Value::Number(static_cast<double>(maker.required)));
      native.object->SetHidden("#name", Value::String(maker.name));
      DefineNonEnumerable(prototype.object, maker.name, native);
    }
  }

  const Value create_pattern =
      interpreter_->NewNativeValue("createPattern", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOfContext(call.self);
        if (call.arguments.size() < 2) {
          return call.Throw("TypeError", "createPattern requires two arguments");
        }
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        dom::Element* source = CanvasImageSourceOf(call.arguments[0]);
        if (source == nullptr) {
          return call.Throw("TypeError", "the pattern source is not an image");
        }
        const Value repetition = call.arguments[1];
        std::string repeat = repetition.IsNull() ? "repeat" : js::ToString(repetition);
        if (repeat.empty()) {
          repeat = "repeat";  // the specification's rule for the empty string, not a typo
        }
        if (repeat != "repeat" && repeat != "repeat-x" && repeat != "repeat-y" &&
            repeat != "no-repeat") {
          return ThrowDom(call, "SyntaxError", "the repetition is not one of the four values");
        }
        const auto [width, height] = owner->canvas_->CanvasSourceSize(source);
        if (width <= 0 || height <= 0) {
          // A source with no pixels yet -- an `<img>` still loading -- is `null` rather than an
          // object, which is what a page checks before using one.
          return Value::Null();
        }
        CanvasOp op;
        op.kind = CanvasOp::Kind::CreatePattern;
        op.source = source;
        op.text = repeat;
        return owner->MakeCanvasGradient(call.self, *element, op);
      });
  if (create_pattern.IsObject()) {
    create_pattern.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "createPattern", create_pattern);
  }

  const Value draw_image =
      interpreter_->NewNativeValue("drawImage", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOfContext(call.self);
        // Three, five or nine arguments and nothing between: Web IDL has three overloads and a
        // four-argument call matches none of them.
        if (call.arguments.size() != 3 && call.arguments.size() != 5 &&
            call.arguments.size() != 9) {
          return call.Throw("TypeError", "drawImage takes three, five or nine arguments");
        }
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        dom::Element* source = CanvasImageSourceOf(call.arguments[0]);
        if (source == nullptr) {
          return call.Throw("TypeError", "the draw source is not an image");
        }
        const auto [natural_width, natural_height] = owner->canvas_->CanvasSourceSize(source);
        if (natural_width <= 0 || natural_height <= 0) {
          // An image with no pixels draws nothing. Not a throw: an `<img>` that has not loaded is
          // the common case, and the specification says the call returns without drawing.
          return Value::Undefined();
        }
        CanvasOp op;
        op.kind = CanvasOp::Kind::DrawImage;
        op.source = source;
        if (call.arguments.size() == 9) {
          op.a = CanvasNumber(call.arguments, 1);
          op.b = CanvasNumber(call.arguments, 2);
          op.c = CanvasNumber(call.arguments, 3);
          op.d = CanvasNumber(call.arguments, 4);
          op.e = CanvasNumber(call.arguments, 5);
          op.f = CanvasNumber(call.arguments, 6);
          op.g = CanvasNumber(call.arguments, 7);
          op.h = CanvasNumber(call.arguments, 8);
        } else {
          op.a = 0.0;
          op.b = 0.0;
          op.c = natural_width;
          op.d = natural_height;
          op.e = CanvasNumber(call.arguments, 1);
          op.f = CanvasNumber(call.arguments, 2);
          op.g = call.arguments.size() == 5 ? CanvasNumber(call.arguments, 3) : natural_width;
          op.h = call.arguments.size() == 5 ? CanvasNumber(call.arguments, 4) : natural_height;
        }
        owner->canvas_->ExecuteCanvasOp(*element, op);
        return Value::Undefined();
      });
  if (draw_image.IsObject()) {
    draw_image.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "drawImage", draw_image);
  }
}

js::Value DomBindings::InstallCanvasGradientInterface() {
  const Value prototype = MakeInterface("CanvasGradient", Value::Undefined());
  if (prototype.IsObject() && !prototype.object->HasOwn("addColorStop")) {
    const Value add = interpreter_->NewNativeValue("addColorStop", [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      if (call.arguments.size() < 2) {
        return call.Throw("TypeError", "addColorStop requires two arguments");
      }
      if (owner == nullptr || owner->canvas_ == nullptr || !call.self.IsObject()) {
        return Value::Undefined();
      }
      const Value* context_value = call.self.object->GetOwn(kContextCanvasSlot);
      dom::Node* node = context_value == nullptr ? nullptr : NodeOf(*context_value);
      if (node == nullptr || !node->IsElement()) {
        return Value::Undefined();
      }
      const double offset = js::ToNumber(call.arguments[0]);
      // Two different failures and they are not the same throw. A non-finite offset fails Web IDL's
      // `double` conversion, which is a `TypeError` before the method's own steps run; an offset
      // outside [0, 1] passes that and fails the method, which is an `IndexSizeError`.
      if (!std::isfinite(offset)) {
        return call.Throw("TypeError", "the stop offset is not a finite number");
      }
      if (offset < 0.0 || offset > 1.0) {
        return ThrowDom(call, "IndexSizeError", "the stop offset is outside [0, 1]");
      }
      const std::string color = js::ToString(call.arguments[1]);
      if (!owner->canvas_->CanvasParsesColor(color)) {
        // A `SyntaxError` rather than a dropped stop: a page that misspelled a colour has drawn a
        // gradient it did not describe, and silence there is a bug it cannot see.
        return ThrowDom(call, "SyntaxError", "the stop colour does not parse");
      }
      CanvasOp stop;
      stop.kind = CanvasOp::Kind::AddColorStop;
      stop.handle = PaintHandleOf(call.self);
      stop.a = offset;
      stop.text = color;
      owner->canvas_->ExecuteCanvasOp(static_cast<dom::Element&>(*node), stop);
      return Value::Undefined();
    });
    if (add.IsObject()) {
      add.object->Set(kOwnerSlot, OwnerValue(this));
      DefineNonEnumerable(prototype.object, "addColorStop", add);
    }
  }
  return prototype;
}

js::Value DomBindings::MakeCanvasGradient(const js::Value& context, dom::Element& element,
                                          CanvasOp op) {
  const Value prototype = InstallCanvasGradientInterface();
  const Value gradient = interpreter_->NewObjectValue();
  if (!prototype.IsObject() || !gradient.IsObject() || !context.IsObject()) {
    return Value::Undefined();
  }
  op.handle = MintPaintHandle(context);
  canvas_->ExecuteCanvasOp(element, op);
  gradient.object->SetPrototype(prototype.object);
  gradient.object->SetHidden(kPaintHandleSlot, Value::Number(static_cast<double>(op.handle)));
  // The canvas element, so `addColorStop` can reach the surface holding the gradient. The gradient
  // object rather than the context, because a context is reachable from the canvas and not the
  // other way round.
  const Value* canvas = context.object->GetOwn(kContextCanvasSlot);
  if (canvas != nullptr) {
    gradient.object->SetHidden(kContextCanvasSlot, *canvas);
  }
  // Recorded so that reading `fillStyle` back returns this object rather than an equal one.
  const Value* existing = context.object->GetOwn(kPaintRegistrySlot);
  Value registry = existing == nullptr ? Value::Undefined() : *existing;
  if (!registry.IsObject()) {
    registry = interpreter_->NewObjectValue();
    if (!registry.IsObject()) {
      return gradient;
    }
    context.object->SetHidden(kPaintRegistrySlot, registry);
  }
  registry.object->SetHidden(std::to_string(op.handle), gradient);
  return gradient;
}

}  // namespace microbrowser::bindings
