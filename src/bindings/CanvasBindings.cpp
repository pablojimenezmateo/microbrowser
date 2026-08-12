// `<canvas>` and its 2D context, as a page sees them.
//
// ADR 0029 §2, session 36. Every method here turns into one `bindings::CanvasOp` and crosses to
// `src/engine`, which executes it against a real `gfx::Painter`. Nothing in this file draws, parses a
// colour, or holds graphics state -- the state lives with the painter, because two copies of a graphics
// state is how a `restore()` restores something the painter never had.
//
// **The context is one object per canvas and it is cached.** `getContext('2d')` called twice must return
// the *same* object, which is the specification's rule and one pages rely on: a library that stashes the
// context and another that asks for it again have to be talking about one thing.

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
  // place every spelling of an attribute write converges. The accessor pair
  // that used to live here owned the resize, so it happened for
  // `canvas.width = 50` and not for `canvas.setAttribute('width', '50')`, and
  // it parsed with `ToNumber` rather than with HTML's rules -- so `width="50px"`
  // was a zero-width canvas where every other browser draws 50.

  const Value get_context = interpreter_->NewNativeValue("getContext", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(call.self);
    if (owner == nullptr || owner->canvas_ == nullptr || node == nullptr || !node->IsElement()) {
      return Value::Null();
    }
    auto& element = static_cast<dom::Element&>(*node);
    if (!owner->canvas_->IsCanvas(element)) {
      return Value::Null();
    }
    const std::string kind = js::ToString(Argument(call.arguments, 0));
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
    get_context.object->Set(kOwnerSlot, PointerValue(this));
    target.object->Set("getContext", get_context);
  }

  // `toDataURL` is deliberately absent rather than returning something. It needs a PNG *encoder*, and
  // this browser has a decoder only -- so a `toDataURL` here would either return an empty string, which
  // a page saving an image would treat as success, or a fabricated URL. ADR 0012's rule: absent, so a
  // page that checks finds nothing and can say so to its user.
}

js::Value DomBindings::MakeCanvasContext(const js::Value& canvas) {
  const Value context = interpreter_->NewObjectValue();
  if (!context.IsObject() || canvas_ == nullptr) {
    return Value::Undefined();
  }
  context.object->SetHidden(kContextCanvasSlot, canvas);
  // `ctx.canvas` is part of the API and pages use it to read the size back.
  context.object->Set("canvas", canvas);

  // One native per method, each carrying the command kind it sends. A table rather than a function per
  // method, because every one of them is "collect the numbers, send the command" -- and forty functions
  // that differ only in a tag is forty places for a tag to be wrong.
  struct Method {
    const char* name;
    CanvasOp::Kind kind;
    int arity;
  };
  static constexpr Method kMethods[] = {
      {"save", CanvasOp::Kind::Save, 0},
      {"restore", CanvasOp::Kind::Restore, 0},
      {"beginPath", CanvasOp::Kind::BeginPath, 0},
      {"closePath", CanvasOp::Kind::ClosePath, 0},
      {"moveTo", CanvasOp::Kind::MoveTo, 2},
      {"lineTo", CanvasOp::Kind::LineTo, 2},
      {"quadraticCurveTo", CanvasOp::Kind::QuadraticCurveTo, 4},
      {"bezierCurveTo", CanvasOp::Kind::BezierCurveTo, 6},
      {"arcTo", CanvasOp::Kind::ArcTo, 5},
      {"rect", CanvasOp::Kind::Rect, 4},
      {"fillRect", CanvasOp::Kind::FillRect, 4},
      {"strokeRect", CanvasOp::Kind::StrokeRect, 4},
      {"clearRect", CanvasOp::Kind::ClearRect, 4},
      {"resetTransform", CanvasOp::Kind::ResetTransform, 0},
      {"setTransform", CanvasOp::Kind::SetTransform, 6},
      {"transform", CanvasOp::Kind::Transform, 6},
  };
  for (const Method& method : kMethods) {
    const Value native = interpreter_->NewNativeValue(method.name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* tag = call.callee == nullptr ? nullptr : call.callee->GetOwn("#op");
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr || tag == nullptr) {
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
      native.object->Set(kOwnerSlot, PointerValue(this));
      native.object->SetHidden("#op", Value::Number(static_cast<double>(method.kind)));
      context.object->Set(method.name, native);
    }
  }

  // `translate`, `scale` and `rotate` are `transform` with the matrix worked out -- expressed that way
  // rather than as three more commands, so there is one place that multiplies matrices.
  const auto shorthand = [this, &context](const char* name) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#shorthand");
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr || which == nullptr) {
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
      native.object->Set(kOwnerSlot, PointerValue(this));
      native.object->SetHidden("#shorthand", Value::String(name));
      context.object->Set(name, native);
    }
  };
  shorthand("translate");
  shorthand("scale");
  shorthand("rotate");

  // `fill`, `stroke` and `clip`, which take an optional fill rule rather than coordinates.
  const auto painting = [this, &context](const char* name, CanvasOp::Kind kind) {
    const Value native = interpreter_->NewNativeValue(name, [kind](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
      if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      CanvasOp op;
      op.kind = kind;
      op.flag = js::ToString(Argument(call.arguments, 0)) == "evenodd";
      owner->canvas_->ExecuteCanvasOp(*element, op);
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      context.object->Set(name, native);
    }
  };
  painting("fill", CanvasOp::Kind::Fill);
  painting("stroke", CanvasOp::Kind::Stroke);
  painting("clip", CanvasOp::Kind::Clip);

  // `arc`, whose last argument is a direction flag rather than a number.
  const Value arc = interpreter_->NewNativeValue("arc", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Element* element = CanvasOf(call.self);
    if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
      return Value::Undefined();
    }
    CanvasOp op;
    op.kind = CanvasOp::Kind::Arc;
    op.a = Number(call.arguments, 0);
    op.b = Number(call.arguments, 1);
    op.c = Number(call.arguments, 2);
    op.d = Number(call.arguments, 3);
    op.e = Number(call.arguments, 4);
    op.flag = js::ToBoolean(Argument(call.arguments, 5));
    owner->canvas_->ExecuteCanvasOp(*element, op);
    return Value::Undefined();
  });
  if (arc.IsObject()) {
    arc.object->Set(kOwnerSlot, PointerValue(this));
    context.object->Set("arc", arc);
  }

  // `fillText` and `strokeText`, which take a string first.
  const auto text_method = [this, &context](const char* name, CanvasOp::Kind kind) {
    const Value native = interpreter_->NewNativeValue(name, [kind](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = CanvasOf(call.self);
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
      native.object->Set(kOwnerSlot, PointerValue(this));
      context.object->Set(name, native);
    }
  };
  text_method("fillText", CanvasOp::Kind::FillText);
  text_method("strokeText", CanvasOp::Kind::StrokeText);

  const Value measure = interpreter_->NewNativeValue("measureText", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Element* element = CanvasOf(call.self);
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
    measure.object->Set(kOwnerSlot, PointerValue(this));
    context.object->Set("measureText", measure);
  }

  // The string- and number-valued properties, as accessors that send a command. Their *values* are read
  // back from a hidden slot rather than from the far side, because the specification says a read
  // returns what was assigned -- including for an assignment that was ignored, where it returns the
  // previous value. Keeping the shadow here is what makes that observable behaviour match.
  struct Property {
    const char* name;
    CanvasOp::Kind kind;
    bool numeric;
    // The initial value as a number for the numeric ones and as text for the rest. Two fields rather
    // than one string parsed back, because parsing a literal this file wrote is a `std::stod` the
    // architecture lint correctly refuses -- it throws, and it reads the decimal separator from the
    // process locale.
    double number;
    const char* text;
  };
  static constexpr Property kProperties[] = {
      {"fillStyle", CanvasOp::Kind::SetFillColor, false, 0.0, "#000000"},
      {"strokeStyle", CanvasOp::Kind::SetStrokeColor, false, 0.0, "#000000"},
      {"lineWidth", CanvasOp::Kind::SetLineWidth, true, 1.0, ""},
      {"lineCap", CanvasOp::Kind::SetLineCap, false, 0.0, "butt"},
      {"lineJoin", CanvasOp::Kind::SetLineJoin, false, 0.0, "miter"},
      {"miterLimit", CanvasOp::Kind::SetMiterLimit, true, 10.0, ""},
      {"globalAlpha", CanvasOp::Kind::SetGlobalAlpha, true, 1.0, ""},
      {"font", CanvasOp::Kind::SetFont, false, 0.0, "10px sans-serif"},
      {"textAlign", CanvasOp::Kind::SetTextAlign, false, 0.0, "start"},
      {"textBaseline", CanvasOp::Kind::SetTextBaseline, false, 0.0, "alphabetic"},
  };
  for (const Property& property : kProperties) {
    const std::string slot = std::string("#prop-") + property.name;
    context.object->SetHidden(slot, property.numeric ? Value::Number(property.number)
                                                     : Value::String(property.text));
    const Value get = interpreter_->NewNativeValue(property.name, [slot](NativeCall& call) -> Value {
      const Value* stored = call.self.IsObject() ? call.self.object->GetOwn(slot) : nullptr;
      return stored == nullptr ? Value::Undefined() : *stored;
    });
    const Value set = interpreter_->NewNativeValue(
        property.name, [slot, property](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          dom::Element* element = CanvasOf(call.self);
          if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr ||
              !call.self.IsObject()) {
            return Value::Undefined();
          }
          CanvasOp op;
          op.kind = property.kind;
          if (property.numeric) {
            op.a = js::ToNumber(Argument(call.arguments, 0));
            if (!std::isfinite(op.a)) {
              return Value::Undefined();  // ignored, and the shadow keeps the old value
            }
            call.self.object->SetHidden(slot, Value::Number(op.a));
          } else {
            op.text = js::ToString(Argument(call.arguments, 0));
            call.self.object->SetHidden(slot, Value::String(op.text));
          }
          owner->canvas_->ExecuteCanvasOp(*element, op);
          return Value::Undefined();
        });
    if (get.IsObject() && set.IsObject()) {
      get.object->Set(kOwnerSlot, PointerValue(this));
      set.object->Set(kOwnerSlot, PointerValue(this));
      context.object->DefineAccessor(property.name, get.object, set.object);
    }
  }

  InstallImageData(context);
  return context;
}

void DomBindings::InstallImageData(const js::Value& context) {
  if (!context.IsObject() || canvas_ == nullptr) {
    return;
  }
  const Value get_image_data =
      interpreter_->NewNativeValue("getImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr) {
          return Value::Undefined();
        }
        // **The taint check, and it throws rather than answering.** A tainted canvas has had
        // cross-origin pixels drawn into it, and a page reading them would be reading an image it was
        // never allowed to see. `SecurityError` is the specified name and the one a page switches on.
        if (owner->canvas_->CanvasIsTainted(*element)) {
          return ThrowDom(call, "SecurityError", "the canvas has been tainted by cross-origin data");
        }
        const int x = static_cast<int>(js::ToNumber(Argument(call.arguments, 0)));
        const int y = static_cast<int>(js::ToNumber(Argument(call.arguments, 1)));
        const int width = static_cast<int>(js::ToNumber(Argument(call.arguments, 2)));
        const int height = static_cast<int>(js::ToNumber(Argument(call.arguments, 3)));
        if (width <= 0 || height <= 0) {
          // Zero is an `IndexSizeError` in the specification, and it is worth throwing rather than
          // handing back an empty buffer: a page that asked for zero pixels has a bug, and an empty
          // `data` array would let it run on.
          return ThrowDom(call, "IndexSizeError", "the source rectangle is empty");
        }
        const std::vector<std::uint8_t> pixels =
            owner->canvas_->ReadCanvasPixels(*element, x, y, width, height);
        if (pixels.empty()) {
          return call.Throw("Error", "the requested region is too large to read");
        }
        return owner->MakeImageData(width, height, pixels);
      });
  if (get_image_data.IsObject()) {
    get_image_data.object->Set(kOwnerSlot, PointerValue(this));
    context.object->Set("getImageData", get_image_data);
  }

  const Value put_image_data =
      interpreter_->NewNativeValue("putImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Element* element = CanvasOf(call.self);
        const Value data = Argument(call.arguments, 0);
        if (owner == nullptr || owner->canvas_ == nullptr || element == nullptr ||
            !data.IsObject()) {
          return Value::Undefined();
        }
        const Value* width_value = data.object->Get("width");
        const Value* height_value = data.object->Get("height");
        const Value* array = data.object->Get("data");
        if (width_value == nullptr || height_value == nullptr || array == nullptr ||
            !array->IsObject()) {
          return call.Throw("TypeError", "putImageData expects an ImageData");
        }
        const int width = static_cast<int>(js::ToNumber(*width_value));
        const int height = static_cast<int>(js::ToNumber(*height_value));
        const js::BufferView* view = array->object->View();
        if (width <= 0 || height <= 0 || view == nullptr || view->bytes == nullptr) {
          return Value::Undefined();
        }
        const std::size_t needed =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        if (view->offset > view->bytes->size() ||
            needed > view->bytes->size() - view->offset) {
          return call.Throw("TypeError", "the ImageData's buffer is too small for its size");
        }
        const std::vector<std::uint8_t> rgba(
            view->bytes->begin() + static_cast<std::ptrdiff_t>(view->offset),
            view->bytes->begin() + static_cast<std::ptrdiff_t>(view->offset + needed));
        owner->canvas_->WriteCanvasPixels(*element,
                                          static_cast<int>(js::ToNumber(Argument(call.arguments, 1))),
                                          static_cast<int>(js::ToNumber(Argument(call.arguments, 2))),
                                          width, height, rgba);
        return Value::Undefined();
      });
  if (put_image_data.IsObject()) {
    put_image_data.object->Set(kOwnerSlot, PointerValue(this));
    context.object->Set("putImageData", put_image_data);
  }

  const Value create_image_data =
      interpreter_->NewNativeValue("createImageData", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr) {
          return Value::Undefined();
        }
        const int width = static_cast<int>(js::ToNumber(Argument(call.arguments, 0)));
        const int height = static_cast<int>(js::ToNumber(Argument(call.arguments, 1)));
        if (width <= 0 || height <= 0) {
          return ThrowDom(call, "IndexSizeError", "the size is empty");
        }
        const std::size_t needed =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        // A bound, because a page chooses the numbers: `createImageData(1e5, 1e5)` is 40GB.
        if (needed > 64u * 1024u * 1024u) {
          return call.Throw("Error", "the requested ImageData is too large");
        }
        return owner->MakeImageData(width, height, std::vector<std::uint8_t>(needed, 0));
      });
  if (create_image_data.IsObject()) {
    create_image_data.object->Set(kOwnerSlot, PointerValue(this));
    context.object->Set("createImageData", create_image_data);
  }
}

js::Value DomBindings::MakeImageData(int width, int height,
                                     const std::vector<std::uint8_t>& rgba) {
  // An `ImageData`: `width`, `height`, and `data` as a `Uint8ClampedArray`. Built through the real
  // typed-array constructor, so the array a page gets behaves like one -- indexable, iterable, with a
  // `buffer`. A plain object with numeric keys would pass a naive test and fail every real filter,
  // which is the shape of stub ADR 0012 forbids.
  const Value image_data = interpreter_->NewObjectValue();
  if (!image_data.IsObject()) {
    return Value::Undefined();
  }
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
  image_data.object->Set("width", Value::Number(static_cast<double>(width)));
  image_data.object->Set("height", Value::Number(static_cast<double>(height)));
  image_data.object->Set("data", made.value);
  return image_data;
}

}  // namespace microbrowser::bindings
