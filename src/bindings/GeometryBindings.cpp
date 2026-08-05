// The geometry a page can ask its own layout about.
//
// `getBoundingClientRect`, `offsetWidth`/`offsetHeight`,
// `clientWidth`/`clientHeight`, and `getComputedStyle` -- 891 measured
// occurrences across the survey's 16.2MB of application script, which is the
// largest single category in it and larger than events, networking and storage
// put together. ADR 0015 is why they are here rather than in `src/engine`, and
// why they answer through a `GeometrySource` rather than by reaching into a box
// tree this module is not allowed to see.
//
// Nothing in this file knows what a box is. It asks a question and formats an
// answer, and every value it receives is a copy that outlives nothing.

#include <cmath>
#include <string>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

dom::Element* ElementOf(const js::Value& value) {
  dom::Node* node = NodeOf(value);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

// `offsetWidth` and its three siblings are integers, and the specification says
// *rounded*, not truncated. A 100.6px box that reported 100 would make a page
// that lays itself out from the number lose a pixel per element, which is the
// kind of drift that shows up as a horizontal scrollbar and nothing else.
double Rounded(float value) {
  return static_cast<double>(std::lround(value));
}

}  // namespace

void DomBindings::InstallGeometry(const js::Value& element_interface) {
  if (geometry_ == nullptr || !element_interface.IsObject()) {
    return;
  }

  // `getBoundingClientRect`. The border box in viewport coordinates, as an
  // object with the eight members every framework reads -- `x`/`y` and
  // `top`/`right`/`bottom`/`left` are the same four numbers under two names,
  // and a page that gets one set and not the other silently computes zero.
  const Value rect_of = interpreter_->NewNativeValue(
      "getBoundingClientRect", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
          return Value::Undefined();
        }
        // An element with no box answers all zeros, which is what the
        // specification says and is an honest answer rather than an evasion:
        // a `display: none` element genuinely has no geometry.
        GeometryRect box;
        if (const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self)) {
          box = found->border_box;
        }
        const Value result = owner->interpreter_->NewObjectValue();
        if (!result.IsObject()) {
          return Value::Undefined();
        }
        const auto number = [](float value) {
          return Value::Number(static_cast<double>(value));
        };
        result.object->Set("x", number(box.x));
        result.object->Set("y", number(box.y));
        result.object->Set("width", number(box.width));
        result.object->Set("height", number(box.height));
        result.object->Set("left", number(box.x));
        result.object->Set("top", number(box.y));
        result.object->Set("right", number(box.x + box.width));
        result.object->Set("bottom", number(box.y + box.height));
        return result;
      });
  if (rect_of.IsObject()) {
    rect_of.object->Set(kOwnerSlot, PointerValue(this));
    element_interface.object->Set("getBoundingClientRect", rect_of);
  }

  // The four integer metrics. `offset*` is the border box and `client*` is the
  // padding box: the difference is the border, and a page that measures a
  // bordered element with the wrong one is off by exactly its border width --
  // small, consistent, and invisible until something is centred by hand.
  //
  // Neither subtracts a scrollbar, because nothing here draws one yet. When
  // ADR 0018's scrollbars arrive, `client*` is where they come out of.
  struct Metric {
    const char* name;
    bool padding_box;
    bool vertical;
  };
  static constexpr Metric kMetrics[] = {
      {"offsetWidth", false, false},
      {"offsetHeight", false, true},
      {"clientWidth", true, false},
      {"clientHeight", true, true},
  };
  for (const Metric& metric : kMetrics) {
    const bool padding_box = metric.padding_box;
    const bool vertical = metric.vertical;
    const Value getter =
        interpreter_->NewNativeValue(metric.name, [padding_box, vertical](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          dom::Node* self = NodeOf(call.self);
          if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
            return Value::Number(0.0);
          }
          const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self);
          if (!found.has_value()) {
            return Value::Number(0.0);
          }
          const GeometryRect& box = padding_box ? found->padding_box : found->border_box;
          return Value::Number(Rounded(vertical ? box.height : box.width));
        });
    if (!getter.IsObject()) {
      continue;
    }
    getter.object->Set(kOwnerSlot, PointerValue(this));
    // No setter. These are read-only in the specification, and a page that
    // assigns one is writing into a value the layout decides.
    element_interface.object->DefineAccessor(metric.name, getter.object, nullptr);
  }
}

void DomBindings::InstallComputedStyle() {
  if (geometry_ == nullptr) {
    return;
  }
  const Value get_computed_style =
      interpreter_->NewNativeValue("getComputedStyle", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr) {
          return Value::Undefined();
        }
        dom::Element* element = ElementOf(Argument(call.arguments, 0));
        if (element == nullptr) {
          // `getComputedStyle(null)` is a TypeError in the specification, and
          // the interpreter has no way to raise one from here -- undefined is
          // the answer a page then trips over at the property read, which is
          // one call later and still points at its own bug.
          return Value::Undefined();
        }
        return owner->MakeComputedStyle(*element);
      });
  if (!get_computed_style.IsObject()) {
    return;
  }
  get_computed_style.object->Set(kOwnerSlot, PointerValue(this));
  interpreter_->Global()->Set("getComputedStyle", get_computed_style);
  interpreter_->GlobalScope()->Declare("getComputedStyle", get_computed_style, false);
}

js::Value DomBindings::MakeComputedStyle(dom::Element& element) {
  // A `Proxy`, for the reason `element.style` is one: there are several hundred
  // CSS property names and no way to enumerate in advance which ones a page
  // will read. The difference from `element.style` is that this one has no
  // `set` trap at all -- a computed style is read-only, and a page that assigns
  // to one is doing something that has never worked anywhere.
  const Value target = interpreter_->NewObjectValue();
  if (!target.IsObject()) {
    return target;
  }
  target.object->Set(kNodeSlot, PointerValue(&element));

  const Value handler = interpreter_->NewObjectValue();
  if (!handler.IsObject()) {
    return handler;
  }

  // The one rule that turns a JavaScript property name into a CSS one, and the
  // same one `element.style` uses: `backgroundColor` is `background-color`.
  const auto to_css_name = [](const std::string& name) {
    std::string out;
    for (const char c : name) {
      if (c >= 'A' && c <= 'Z') {
        out.push_back('-');
        out.push_back(static_cast<char>(c - 'A' + 'a'));
        continue;
      }
      out.push_back(c);
    }
    return out;
  };

  const Value getter = interpreter_->NewNativeValue("get", [to_css_name](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Element* self = ElementOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
      return Value::Undefined();
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Undefined();  // no protocol hooks on a style declaration
    }
    const std::string written = js::ToString(key);
    if (written == "getPropertyValue") {
      // The method form, made on demand rather than kept on the target: a page
      // reads it once per query and the alternative is a property on every
      // computed style whether or not anything asks.
      //
      // The element travels on the function object rather than in `self`,
      // because `self` at the call is the *Proxy* and a proxy has no own
      // properties of its own to read a node out of -- which is the same
      // reason the bindings pointer travels there. A capture would work and is
      // refused for the reason BindingSupport gives: a raw pointer in a
      // capture is a lifetime the collector cannot see.
      const Value method = owner->interpreter_->NewNativeValue(
          "getPropertyValue", [](NativeCall& inner) -> Value {
            DomBindings* method_owner = OwnerOf(inner);
            dom::Node* node = inner.callee == nullptr
                                  ? nullptr
                                  : NodeOf(Value::Obj(inner.callee));
            if (method_owner == nullptr || node == nullptr || !node->IsElement() ||
                method_owner->geometry_ == nullptr) {
              return Value::String(std::string());
            }
            const std::string property = js::ToString(Argument(inner.arguments, 0));
            const std::optional<std::string> value = method_owner->geometry_->QueryUsedValue(
                static_cast<dom::Element&>(*node), property);
            return Value::String(value.value_or(std::string()));
          });
      if (method.IsObject()) {
        method.object->Set(kOwnerSlot, PointerValue(owner));
        method.object->Set(kNodeSlot, PointerValue(self));
      }
      return method;
    }
    const std::optional<std::string> value =
        owner->geometry_->QueryUsedValue(*self, to_css_name(written));
    // A property this engine has no answer for reads back as the empty string,
    // which is what the specification says an unsupported property does. It is
    // a string either way: a page writes
    // `if (getComputedStyle(el).display === 'none')` and an `undefined` there
    // compares false in a way nothing reports.
    return Value::String(value.value_or(std::string()));
  });
  if (!getter.IsObject()) {
    return Value::Undefined();
  }
  getter.object->Set(kOwnerSlot, PointerValue(this));
  handler.object->Set("get", getter);

  js::Value* constructor = interpreter_->GlobalScope()->Lookup("Proxy");
  if (constructor == nullptr || !constructor->IsObject()) {
    return Value::Undefined();
  }
  const js::Result made =
      interpreter_->CallFunction(*constructor, Value::Undefined(), {target, handler});
  return made.IsAbrupt() ? Value::Undefined() : made.value;
}

}  // namespace microbrowser::bindings
