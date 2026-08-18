// `<canvas>` and its 2D context, as a page sees them.
//
// ADR 0029 §2, session 36. Every method here turns into one `bindings::CanvasOp` and crosses to
// `src/engine`, which executes it against a real `gfx::Painter`. Nothing in this file draws, parses a
// colour, or holds graphics state.
//
// **The state is read back rather than shadowed**, and that is the change that made `save()` and
// `restore()` correct. The first version kept a hidden slot per property on the context and answered
// getters from it, which meant `restore()` put the painter's colour back and left `ctx.fillStyle`
// saying the other one -- a getter that disagrees with what the next `fill()` draws. There is one
// copy of the state, it lives with the painter, and a getter asks it. `CanvasSurface::CanvasStateText`
// is that question.
//
// **The context is a real interface with a real prototype.** `CanvasRenderingContext2D.prototype` is
// where every method lives, so a page can add one, replace one, or `delete` one, and `ctx instanceof
// CanvasRenderingContext2D` answers. The first version built forty natives per canvas as own
// properties, which made all of that false and cost a canvas forty allocations.

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

constexpr const char* kContextSlot = "#canvas-context";
constexpr const char* kContextCanvasSlot = "#context-canvas";
// The gradients and patterns this context has handed out, by handle, so that reading `fillStyle`
// back gives the *same object* the page assigned -- which the specification requires and which is
// what a page comparing `ctx.fillStyle === myGradient` depends on. The handle is the far side's
// state, so `save()`/`restore()` restores which one is selected for free.
constexpr const char* kPaintRegistrySlot = "#canvas-paints";
constexpr const char* kPaintCounterSlot = "#canvas-paint-counter";
constexpr const char* kPaintHandleSlot = "#paint-handle";

double Number(const std::vector<Value>& arguments, std::size_t index, double fallback = 0.0) {
  if (index >= arguments.size()) {
    return fallback;
  }
  const double value = js::ToNumber(arguments[index]);
  // NaN reaches the far side and is refused there, per command. Not filtered here: the specification's
  // rule is that a non-finite argument makes the *call* a no-op, and which arguments a command has is
  // the command's business rather than this function's.
  return value;
}

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

// Web IDL's argument count check, which is a `TypeError` *before* anything else happens.
//
// Not decoration: `2d.conformance.requirements.missingargs` asserts it for thirty methods, and the
// reason it matters outside a test is that a page calling `ctx.fillRect(x, y, w)` has a bug, and a
// browser that filled a zero-height rectangle would hide it.
bool Arity(NativeCall& call, std::size_t required, const char* name, Value& thrown) {
  if (call.arguments.size() >= required) {
    return true;
  }
  thrown = call.Throw("TypeError",
                      std::string(name) + " requires " + std::to_string(required) +
                          " arguments, but only " + std::to_string(call.arguments.size()) +
                          " were passed");
  return false;
}

// The methods that are "collect the numbers, send the command". Everything whose arguments are
// positional doubles is one row here rather than one function, because forty functions differing only
// in a tag is forty places for the tag to be wrong.
struct Method {
  const char* name;
  CanvasOp::Kind kind;
  std::size_t required;
};

constexpr Method kNumericMethods[] = {
    {"save", CanvasOp::Kind::Save, 0},
    {"restore", CanvasOp::Kind::Restore, 0},
    {"reset", CanvasOp::Kind::Reset, 0},
    {"beginPath", CanvasOp::Kind::BeginPath, 0},
    {"closePath", CanvasOp::Kind::ClosePath, 0},
    {"moveTo", CanvasOp::Kind::MoveTo, 2},
    {"lineTo", CanvasOp::Kind::LineTo, 2},
    {"quadraticCurveTo", CanvasOp::Kind::QuadraticCurveTo, 4},
    {"bezierCurveTo", CanvasOp::Kind::BezierCurveTo, 6},
    {"rect", CanvasOp::Kind::Rect, 4},
    {"fillRect", CanvasOp::Kind::FillRect, 4},
    {"strokeRect", CanvasOp::Kind::StrokeRect, 4},
    {"clearRect", CanvasOp::Kind::ClearRect, 4},
    {"resetTransform", CanvasOp::Kind::ResetTransform, 0},
    {"transform", CanvasOp::Kind::Transform, 6},
};

// The state properties, as (name, command, kind). The command is the key on both sides: the setter
// sends it and the getter asks for it back, so there is no second table to fall out of step with the
// first.
struct Property {
  const char* name;
  CanvasOp::Kind kind;
  bool numeric;
};

constexpr Property kProperties[] = {
    {"fillStyle", CanvasOp::Kind::SetFillColor, false},
    {"strokeStyle", CanvasOp::Kind::SetStrokeColor, false},
    {"lineWidth", CanvasOp::Kind::SetLineWidth, true},
    {"lineCap", CanvasOp::Kind::SetLineCap, false},
    {"lineJoin", CanvasOp::Kind::SetLineJoin, false},
    {"miterLimit", CanvasOp::Kind::SetMiterLimit, true},
    {"globalAlpha", CanvasOp::Kind::SetGlobalAlpha, true},
    {"font", CanvasOp::Kind::SetFont, false},
    {"textAlign", CanvasOp::Kind::SetTextAlign, false},
    {"textBaseline", CanvasOp::Kind::SetTextBaseline, false},
};

// The handle a gradient object carries, or zero when the value is not one of ours.
std::uint32_t PaintHandleOf(const Value& value) {
  if (!value.IsObject()) {
    return 0;
  }
  const Value* handle = value.object->GetOwn(kPaintHandleSlot);
  if (handle == nullptr) {
    return 0;
  }
  const double number = js::ToNumber(*handle);
  return number > 0.0 ? static_cast<std::uint32_t>(number) : 0;
}

// The next handle for this context, and the registry it is recorded in.
std::uint32_t MintPaintHandle(const Value& context) {
  const Value* counter = context.object->GetOwn(kPaintCounterSlot);
  const auto next =
      static_cast<std::uint32_t>((counter == nullptr ? 0.0 : js::ToNumber(*counter)) + 1.0);
  context.object->SetHidden(kPaintCounterSlot, Value::Number(static_cast<double>(next)));
  return next;
}

}  // namespace

void DomBindings::InstallCanvas(const js::Value& target) {
  if (canvas_ == nullptr || !target.IsObject()) {
    // ADR 0012's rule again: no surface behind this layer means `<canvas>` has no `getContext` at all,
    // and a page that feature-detects it draws its fallback. A `getContext` returning an object whose
    // methods did nothing would be the worst of both -- the page would draw into nothing and show it.
    return;
  }

  // `width` and `height` are *not* here, and that is a fix rather than a gap.
  // They are ordinary reflected `unsigned long`s with the defaults 300 and 150
  // (ReflectionTable.cpp), and the resize hangs off the attribute *write*
  // instead -- `DomBindings::ResizeCanvasIfDimension`, reached from the one
  // place every spelling of an attribute write converges.

  const Value get_context = interpreter_->NewNativeValue("getContext", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(call.self);
    if (owner == nullptr || owner->canvas_ == nullptr || node == nullptr || !node->IsElement()) {
      return Value::Null();
    }
    if (call.arguments.empty()) {
      // Web IDL: `contextId` is not optional. A page that calls `getContext()` has a bug and a null
      // return would send it into `ctx.fillRect` on null one line later.
      return call.Throw("TypeError", "getContext requires a context id");
    }
    auto& element = static_cast<dom::Element&>(*node);
    if (!owner->canvas_->IsCanvas(element)) {
      return Value::Null();
    }
    const std::string kind = js::ToString(call.arguments[0]);
    if (kind != "2d") {
      // **Null, not an object.** `getContext('webgl')` returning null is how a page learns there is no
      // WebGL here and takes its 2D or its no-canvas path; returning something would send it down a
      // path that fails later and less clearly. ADR 0029 keeps WebGL out.
      return Value::Null();
    }
    // Cached: the same canvas must hand back the same context object every time.
    if (const Value* existing = call.self.object->GetOwn(kContextSlot)) {
      if (existing->IsObject()) {
        return *existing;
      }
    }
    const Value context = owner->MakeCanvasContext(call.self);
    if (context.IsObject()) {
      call.self.object->SetHidden(kContextSlot, context);
    }
    return context;
  });
  if (get_context.IsObject()) {
    get_context.object->Set(kOwnerSlot, OwnerValue(this));
    target.object->Set("getContext", get_context);
  }

  // `toDataURL` is deliberately absent rather than returning something. It needs a PNG *encoder*, and
  // this browser has a decoder only -- so a `toDataURL` here would either return an empty string, which
  // a page saving an image would treat as success, or a fabricated URL. ADR 0012's rule: absent, so a
  // page that checks finds nothing and can say so to its user.
}

js::Value DomBindings::CanvasContextPrototype() {
  const Value prototype = MakeInterface("CanvasRenderingContext2D", Value::Undefined());
  if (!prototype.IsObject()) {
    return Value::Undefined();
  }
  if (prototype.object->HasOwn("fill")) {
    return prototype;  // already built
  }

  // `ctx.canvas`, readonly. An accessor over the hidden slot rather than an own data property, because
  // `2d.canvas.host.readonly` assigns to it and expects the assignment to be ignored.
  const Value canvas_get = interpreter_->NewNativeValue("canvas", [](NativeCall& call) -> Value {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    const Value* canvas = call.self.object->GetOwn(kContextCanvasSlot);
    return canvas == nullptr ? Value::Undefined() : *canvas;
  });
  if (canvas_get.IsObject()) {
    prototype.object->DefineAccessor("canvas", canvas_get.object, nullptr);
  }

  for (const Method& method : kNumericMethods) {
    const Value native = interpreter_->NewNativeValue(method.name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* tag = call.callee == nullptr ? nullptr : call.callee->GetOwn("#op");
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      if (tag == nullptr || arity == nullptr) {
        return Value::Undefined();
      }
      Value thrown;
      const Value* name = call.callee->GetOwn("#name");
      if (!Arity(call, static_cast<std::size_t>(js::ToNumber(*arity)),
                 name == nullptr ? "this method" : js::ToString(*name).c_str(), thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      op.kind = static_cast<CanvasOp::Kind>(static_cast<int>(js::ToNumber(*tag)));
      op.a = Number(call.arguments, 0);
      op.b = Number(call.arguments, 1);
      op.c = Number(call.arguments, 2);
      op.d = Number(call.arguments, 3);
      op.e = Number(call.arguments, 4);
      op.f = Number(call.arguments, 5);
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->SetHidden("#op", Value::Number(static_cast<double>(method.kind)));
      native.object->SetHidden("#arity", Value::Number(static_cast<double>(method.required)));
      native.object->SetHidden("#name", Value::String(method.name));
      DefineNonEnumerable(prototype.object, method.name, native);
    }
  }

  InstallCanvasTransforms(prototype);
  InstallCanvasPaths(prototype);
  InstallCanvasText(prototype);
  InstallCanvasProperties(prototype);
  InstallCanvasPaintSources(prototype);
  InstallImageData(prototype);
  return prototype;
}

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
      dom::Element* element = CanvasOf(call.self);
      const Value* tag = call.callee == nullptr ? nullptr : call.callee->GetOwn("#op");
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      const Value* named = call.callee == nullptr ? nullptr : call.callee->GetOwn("#name");
      if (tag == nullptr || arity == nullptr) {
        return Value::Undefined();
      }
      Value thrown;
      if (!Arity(call, static_cast<std::size_t>(js::ToNumber(*arity)),
                 named == nullptr ? "this method" : js::ToString(*named).c_str(), thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      op.kind = static_cast<CanvasOp::Kind>(static_cast<int>(js::ToNumber(*tag)));
      op.a = Number(call.arguments, 0);
      op.b = Number(call.arguments, 1);
      op.c = Number(call.arguments, 2);
      op.d = Number(call.arguments, 3);
      op.e = Number(call.arguments, 4);
      op.f = Number(call.arguments, 5);
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

void DomBindings::InstallCanvasProperties(const js::Value& prototype) {
  for (const Property& property : kProperties) {
    const Value get =
        interpreter_->NewNativeValue(property.name, [property](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          dom::Element* element = CanvasOf(call.self);
          if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
            return Value::Undefined();
          }
          // **Asked, not remembered.** See this file's header comment: one copy of the state.
          if (property.numeric) {
            return Value::Number(owner->canvas_->CanvasStateNumber(*element, property.kind));
          }
          if (property.kind == CanvasOp::Kind::SetFillColor ||
              property.kind == CanvasOp::Kind::SetStrokeColor) {
            // A gradient reads back as the object the page assigned, not as a colour. Which one is
            // selected is the far side's state, so this is a lookup rather than a second copy.
            const CanvasOp::Kind paint = property.kind == CanvasOp::Kind::SetFillColor
                                             ? CanvasOp::Kind::SetFillPaint
                                             : CanvasOp::Kind::SetStrokePaint;
            const auto handle =
                static_cast<std::uint32_t>(owner->canvas_->CanvasStateNumber(*element, paint));
            if (handle != 0) {
              if (const Value* registry = call.self.object->GetOwn(kPaintRegistrySlot)) {
                if (registry->IsObject()) {
                  if (const Value* found = registry->object->GetOwn(std::to_string(handle))) {
                    return *found;
                  }
                }
              }
            }
          }
          return Value::String(owner->canvas_->CanvasStateText(*element, property.kind));
        });
    const Value set =
        interpreter_->NewNativeValue(property.name, [property](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          dom::Element* element = CanvasOf(call.self);
          if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
            return Value::Undefined();
          }
          CanvasOp op;
          op.kind = property.kind;
          if (property.numeric) {
            op.a = js::ToNumber(Argument(call.arguments, 0));
          } else {
            const Value given = Argument(call.arguments, 0);
            const bool is_style = property.kind == CanvasOp::Kind::SetFillColor ||
                                  property.kind == CanvasOp::Kind::SetStrokeColor;
            if (is_style) {
              if (const std::uint32_t handle = PaintHandleOf(given)) {
                CanvasOp select;
                select.kind = property.kind == CanvasOp::Kind::SetFillColor
                                  ? CanvasOp::Kind::SetFillPaint
                                  : CanvasOp::Kind::SetStrokePaint;
                select.handle = handle;
                owner->canvas_->ExecuteCanvasOp(*element, select);
                return Value::Undefined();
              }
              // Not a string and not one of ours: *ignored*, per the specification. Stringifying it
              // would make `ctx.fillStyle = {}` the colour `[object Object]`, which parses as
              // nothing and would look identical to a page that had assigned a gradient we lost.
              if (!given.IsString()) {
                return Value::Undefined();
              }
            }
            op.text = js::ToString(given);
          }
          owner->canvas_->ExecuteCanvasOp(*element, op);
          return Value::Undefined();
        });
    if (get.IsObject() && set.IsObject()) {
      get.object->Set(kOwnerSlot, OwnerValue(this));
      set.object->Set(kOwnerSlot, OwnerValue(this));
      prototype.object->DefineAccessor(property.name, get.object, set.object);
    }
  }
}

void DomBindings::InstallCanvasTransforms(const js::Value& prototype) {
  // `translate`, `scale` and `rotate` are `transform` with the matrix worked out -- expressed that way
  // rather than as three more commands, so there is one place that multiplies matrices.
  const auto shorthand = [this, &prototype](const char* name, std::size_t required) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#shorthand");
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      if (which == nullptr || arity == nullptr) {
        return Value::Undefined();
      }
      Value thrown;
      if (!Arity(call, static_cast<std::size_t>(js::ToNumber(*arity)), js::ToString(*which).c_str(),
                 thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      const std::string what = js::ToString(*which);
      CanvasOp op;
      op.kind = CanvasOp::Kind::Transform;
      if (what == "translate") {
        op.a = 1.0;
        op.d = 1.0;
        op.e = Number(call.arguments, 0);
        op.f = Number(call.arguments, 1);
      } else if (what == "scale") {
        op.a = Number(call.arguments, 0, 1.0);
        op.d = Number(call.arguments, 1, 1.0);
      } else {
        const double radians = Number(call.arguments, 0);
        op.a = std::cos(radians);
        op.b = std::sin(radians);
        op.c = -std::sin(radians);
        op.d = std::cos(radians);
      }
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->SetHidden("#shorthand", Value::String(name));
      native.object->SetHidden("#arity", Value::Number(static_cast<double>(required)));
      DefineNonEnumerable(prototype.object, name, native);
    }
  };
  shorthand("translate", 2);
  shorthand("scale", 2);
  shorthand("rotate", 1);

  // `setTransform` has two forms and the *empty* one is legal: Web IDL's second overload takes an
  // optional `DOMMatrix2DInit`, so `setTransform()` is the identity. One argument is neither overload
  // and is a TypeError, which is what separates this from the table above.
  const Value set_transform =
      interpreter_->NewNativeValue("setTransform", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        CanvasOp op;
        op.kind = CanvasOp::Kind::SetTransform;
        if (call.arguments.empty()) {
          op.a = 1.0;
          op.d = 1.0;
        } else if (call.arguments.size() == 1) {
          const Value init = call.arguments[0];
          if (!init.IsObject()) {
            return call.Throw("TypeError", "setTransform expects six numbers or a matrix");
          }
          // A `DOMMatrix2DInit`: the `a`..`f` spelling and the `m11`..`m42` spelling name the same six
          // numbers, and a dictionary may use either.
          const auto member = [&init](const char* first, const char* second, double fallback) {
            if (const Value* value = init.object->Get(first)) {
              return js::ToNumber(*value);
            }
            if (const Value* value = init.object->Get(second)) {
              return js::ToNumber(*value);
            }
            return fallback;
          };
          op.a = member("a", "m11", 1.0);
          op.b = member("b", "m12", 0.0);
          op.c = member("c", "m21", 0.0);
          op.d = member("d", "m22", 1.0);
          op.e = member("e", "m41", 0.0);
          op.f = member("f", "m42", 0.0);
        } else if (call.arguments.size() < 6) {
          return call.Throw("TypeError", "setTransform requires six numbers");
        } else {
          op.a = Number(call.arguments, 0);
          op.b = Number(call.arguments, 1);
          op.c = Number(call.arguments, 2);
          op.d = Number(call.arguments, 3);
          op.e = Number(call.arguments, 4);
          op.f = Number(call.arguments, 5);
        }
        owner->canvas_->ExecuteCanvasOp(*element, op);
        return Value::Undefined();
      });
  if (set_transform.IsObject()) {
    set_transform.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "setTransform", set_transform);
  }

  // `getTransform` returns a `DOMMatrix`, and there is no `DOMMatrix` in this browser. What it returns
  // is an object carrying the six numbers under both spellings -- which is what every caller reads --
  // and it is *not* called DOMMatrix, so a page that checks `instanceof` is told the truth.
  const Value get_transform =
      interpreter_->NewNativeValue("getTransform", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        const std::vector<double> matrix = owner->canvas_->CanvasTransform(*element);
        if (matrix.size() != 6) {
          return Value::Undefined();
        }
        const Value out = call.interpreter.NewObjectValue();
        if (!out.IsObject()) {
          return Value::Undefined();
        }
        static constexpr const char* kShort[] = {"a", "b", "c", "d", "e", "f"};
        static constexpr const char* kLong[] = {"m11", "m12", "m21", "m22", "m41", "m42"};
        for (std::size_t i = 0; i < matrix.size(); ++i) {
          out.object->Set(kShort[i], Value::Number(matrix[i]));
          out.object->Set(kLong[i], Value::Number(matrix[i]));
        }
        return out;
      });
  if (get_transform.IsObject()) {
    get_transform.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "getTransform", get_transform);
  }
}

void DomBindings::InstallCanvasPaths(const js::Value& prototype) {
  // `fill`, `stroke` and `clip`, which take an optional fill rule rather than coordinates.
  const auto painting = [this, &prototype](const char* name, CanvasOp::Kind kind) {
    const Value native = interpreter_->NewNativeValue(name, [kind](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      const Value rule = Argument(call.arguments, 0);
      if (!rule.IsUndefined()) {
        const std::string text = js::ToString(rule);
        if (text != "nonzero" && text != "evenodd") {
          // A fill rule is an enumeration, and Web IDL rejects a value outside it rather than
          // defaulting -- a page with a typo would otherwise silently get non-zero.
          return call.Throw("TypeError", "the fill rule must be 'nonzero' or 'evenodd'");
        }
      }
      CanvasOp op;
      op.kind = kind;
      op.flag = js::ToString(rule) == "evenodd";
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      DefineNonEnumerable(prototype.object, name, native);
    }
  };
  painting("fill", CanvasOp::Kind::Fill);
  painting("stroke", CanvasOp::Kind::Stroke);
  painting("clip", CanvasOp::Kind::Clip);

  // `arc` and `ellipse`, which share one command: an ellipse is an arc with two radii and a rotation,
  // and the counter-clockwise sweep rule is subtle enough that two constructions would be two chances
  // to get it wrong. `arcTo` joins them here because it also ends in an arc.
  const auto arc_method = [this, &prototype](const char* name, std::size_t required) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arc");
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      if (which == nullptr || arity == nullptr) {
        return Value::Undefined();
      }
      Value thrown;
      const std::string what = js::ToString(*which);
      if (!Arity(call, static_cast<std::size_t>(js::ToNumber(*arity)), what.c_str(), thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      if (what == "arcTo") {
        op.kind = CanvasOp::Kind::ArcTo;
        op.a = Number(call.arguments, 0);
        op.b = Number(call.arguments, 1);
        op.c = Number(call.arguments, 2);
        op.d = Number(call.arguments, 3);
        op.e = Number(call.arguments, 4);
        if (op.e < 0.0) {
          return ThrowDom(call, "IndexSizeError", "the radius is negative");
        }
      } else if (what == "arc") {
        op.kind = CanvasOp::Kind::Arc;
        op.a = Number(call.arguments, 0);
        op.b = Number(call.arguments, 1);
        op.c = Number(call.arguments, 2);
        op.f = op.c;
        op.d = Number(call.arguments, 3);
        op.e = Number(call.arguments, 4);
        op.flag = js::ToBoolean(Argument(call.arguments, 5));
        if (op.c < 0.0) {
          return ThrowDom(call, "IndexSizeError", "the radius is negative");
        }
      } else {
        op.kind = CanvasOp::Kind::Arc;
        op.a = Number(call.arguments, 0);
        op.b = Number(call.arguments, 1);
        op.c = Number(call.arguments, 2);
        op.f = Number(call.arguments, 3);
        op.g = Number(call.arguments, 4);
        op.d = Number(call.arguments, 5);
        op.e = Number(call.arguments, 6);
        op.flag = js::ToBoolean(Argument(call.arguments, 7));
        if (op.c < 0.0 || op.f < 0.0) {
          return ThrowDom(call, "IndexSizeError", "a radius is negative");
        }
      }
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->SetHidden("#arc", Value::String(name));
      native.object->SetHidden("#arity", Value::Number(static_cast<double>(required)));
      DefineNonEnumerable(prototype.object, name, native);
    }
  };
  arc_method("arc", 5);
  arc_method("arcTo", 5);
  arc_method("ellipse", 7);

  const Value round_rect =
      interpreter_->NewNativeValue("roundRect", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        Value thrown;
        if (!Arity(call, 4, "roundRect", thrown)) {
          return thrown;
        }
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        CanvasOp op;
        op.kind = CanvasOp::Kind::RoundRect;
        op.a = Number(call.arguments, 0);
        op.b = Number(call.arguments, 1);
        op.c = Number(call.arguments, 2);
        op.d = Number(call.arguments, 3);
        Value radii_error;
        if (!DomBindings::ReadCornerRadii(call, Argument(call.arguments, 4), op.c, op.d, op.numbers,
                                          radii_error)) {
          return radii_error;
        }
        if (!std::isfinite(op.a) || !std::isfinite(op.b) || !std::isfinite(op.c) ||
            !std::isfinite(op.d)) {
          return Value::Undefined();  // a non-finite rectangle makes the call a no-op
        }
        owner->canvas_->ExecuteCanvasOp(*element, op);
        return Value::Undefined();
      });
  if (round_rect.IsObject()) {
    round_rect.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "roundRect", round_rect);
  }

  // `isPointInPath` and `isPointInStroke`: questions rather than commands, answered by the far side
  // against the path it is holding.
  const auto hit = [this, &prototype](const char* name, bool stroke, std::size_t required) {
    const Value native = interpreter_->NewNativeValue(name, [stroke](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* arity = call.callee == nullptr ? nullptr : call.callee->GetOwn("#arity");
      if (arity == nullptr) {
        return Value::Bool(false);
      }
      Value thrown;
      const Value* called = call.callee->GetOwn("#name");
      if (!Arity(call, static_cast<std::size_t>(js::ToNumber(*arity)),
                 called == nullptr ? "this method" : js::ToString(*called).c_str(), thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Bool(false);
      }
      const double x = Number(call.arguments, 0);
      const double y = Number(call.arguments, 1);
      bool even_odd = false;
      if (!stroke && call.arguments.size() > 2) {
        const std::string rule = js::ToString(call.arguments[2]);
        if (rule != "nonzero" && rule != "evenodd") {
          return call.Throw("TypeError", "the fill rule must be 'nonzero' or 'evenodd'");
        }
        even_odd = rule == "evenodd";
      }
      return Value::Bool(owner->canvas_->CanvasHitTest(*element, x, y, stroke, even_odd));
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->SetHidden("#arity", Value::Number(static_cast<double>(required)));
      native.object->SetHidden("#name", Value::String(name));
      DefineNonEnumerable(prototype.object, name, native);
    }
  };
  hit("isPointInPath", false, 2);
  hit("isPointInStroke", true, 2);
}

bool DomBindings::ReadCornerRadii(js::NativeCall& call, const js::Value& given, double width,
                                  double height, std::vector<double>& out, js::Value& thrown) {
  // The specification's radii argument: a number, a list of one to four numbers, or absent. Each entry
  // may also be a `DOMPointInit` with `x` and `y`, which is how an elliptical corner is written.
  //
  // What comes out is always eight numbers -- four corners times (x, y) -- already normalised against
  // the rectangle, because the clamping rule is defined once in the specification and doing it here
  // means the geometry never has to know how many radii the page wrote.
  std::vector<std::pair<double, double>> corners;
  const auto read_one = [&call, &corners](const Value& value) -> bool {
    if (value.IsObject()) {
      const Value* x = value.object->Get("x");
      const Value* y = value.object->Get("y");
      if (x != nullptr || y != nullptr) {
        corners.emplace_back(x == nullptr ? 0.0 : js::ToNumber(*x),
                             y == nullptr ? 0.0 : js::ToNumber(*y));
        return true;
      }
    }
    const double radius = js::ToNumber(value);
    corners.emplace_back(radius, radius);
    return true;
  };
  if (given.IsUndefined()) {
    corners.emplace_back(0.0, 0.0);
  } else if (given.IsObject() && given.object->GetKind() == js::Object::Kind::Array) {
    // An array's elements do not live in its property map, which is why this asks `ElementCount`
    // rather than reading a `length` property -- the first version read `GetOwn("length")`, got null
    // for every real array, and fell through to `ToNumber([0, 0, 0, 20])`, which is NaN. Every
    // `roundRect` with an array of radii silently drew nothing.
    const std::size_t count = given.object->ElementCount();
    if (count < 1 || count > 4) {
      thrown = call.Throw("RangeError", "roundRect takes one to four radii");
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (!read_one(given.object->GetElement(i))) {
        return false;
      }
    }
  } else {
    read_one(given);
  }
  for (const auto& [rx, ry] : corners) {
    if (!std::isfinite(rx) || !std::isfinite(ry)) {
      out.clear();
      return true;  // a non-finite radius makes the whole call a no-op, not a throw
    }
    if (rx < 0.0 || ry < 0.0) {
      thrown = call.Throw("RangeError", "a corner radius is negative");
      return false;
    }
  }
  // One radius applies to all four corners; two are (top-left + bottom-right, top-right +
  // bottom-left); three are top-left, (top-right + bottom-left), bottom-right; four are in order.
  std::pair<double, double> tl, tr, br, bl;
  switch (corners.size()) {
    case 1:
      tl = tr = br = bl = corners[0];
      break;
    case 2:
      tl = br = corners[0];
      tr = bl = corners[1];
      break;
    case 3:
      tl = corners[0];
      tr = bl = corners[1];
      br = corners[2];
      break;
    default:
      tl = corners[0];
      tr = corners[1];
      br = corners[2];
      bl = corners[3];
      break;
  }
  // The overlap clamp: adjacent radii on one edge may not exceed it, and the *smallest* scale factor
  // across all four edges is applied to every corner. Per-edge scaling would produce a shape whose
  // corners no longer match, which is the classic wrong `border-radius`.
  const double w = std::abs(width);
  const double h = std::abs(height);
  double scale = 1.0;
  const auto limit = [&scale](double sum, double extent) {
    if (sum > 0.0 && extent >= 0.0 && sum * scale > extent) {
      scale = extent / sum;
    }
  };
  limit(tl.first + tr.first, w);
  limit(bl.first + br.first, w);
  limit(tl.second + bl.second, h);
  limit(tr.second + br.second, h);
  out = {tl.first * scale, tl.second * scale, tr.first * scale, tr.second * scale,
         br.first * scale, br.second * scale, bl.first * scale, bl.second * scale};
  return true;
}

void DomBindings::InstallCanvasText(const js::Value& prototype) {
  // `fillText` and `strokeText`, which take a string first.
  const auto text_method = [this, &prototype](const char* name, CanvasOp::Kind kind) {
    const Value native = interpreter_->NewNativeValue(name, [kind](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      Value thrown;
      if (!Arity(call, 3, "fillText/strokeText", thrown)) {
        return thrown;
      }
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      op.kind = kind;
      op.text = js::ToString(Argument(call.arguments, 0));
      op.a = Number(call.arguments, 1);
      op.b = Number(call.arguments, 2);
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      DefineNonEnumerable(prototype.object, name, native);
    }
  };
  text_method("fillText", CanvasOp::Kind::FillText);
  text_method("strokeText", CanvasOp::Kind::StrokeText);

  const Value measure = interpreter_->NewNativeValue("measureText", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Element* element = CanvasOf(call.self);
    Value thrown;
    if (!Arity(call, 1, "measureText", thrown)) {
      return thrown;
    }
    if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
      return Value::Undefined();
    }
    const Value metrics = call.interpreter.NewObjectValue();
    if (metrics.IsObject()) {
      metrics.object->Set(
          "width", Value::Number(owner->canvas_->MeasureCanvasText(
                       *element, js::ToString(Argument(call.arguments, 0)))));
    }
    return metrics;
  });
  if (measure.IsObject()) {
    measure.object->Set(kOwnerSlot, OwnerValue(this));
    DefineNonEnumerable(prototype.object, "measureText", measure);
  }
}

js::Value DomBindings::MakeCanvasContext(const js::Value& canvas) {
  const Value prototype = CanvasContextPrototype();
  const Value context = interpreter_->NewObjectValue();
  if (!context.IsObject() || !prototype.IsObject() || canvas_ == nullptr) {
    return Value::Undefined();
  }
  context.object->SetPrototype(prototype.object);
  context.object->SetHidden(kContextCanvasSlot, canvas);
  return context;
}

}  // namespace microbrowser::bindings
