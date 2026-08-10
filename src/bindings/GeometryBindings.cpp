// The geometry a page can ask its own layout about.
//
// `getBoundingClientRect`, `getClientRects`, `offsetWidth`/`offsetHeight`,
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
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Fingerprint.h"

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

// `scrollTo` and `scrollBy` differ by one bit: whether the numbers are a
// destination or a displacement. Both exist on an element and on the window,
// which is why the table is here rather than inside one of them.
struct Method {
  const char* name;
  bool relative;
};

// Where a `scrollTo`/`scrollBy` call wants to end up.
struct ScrollTarget {
  float x = 0.0f;
  float y = 0.0f;
};

// The two argument forms of `scrollTo` and `scrollBy`, which every page writes
// one or the other of: two numbers, or one options object with `left` and `top`
// on it. An axis the caller did not name keeps its current value rather than
// going to zero -- `scrollTo({top: 0})` must not also scroll sideways.
ScrollTarget ScrollTargetFrom(js::NativeCall& call, const BoxGeometry& current, bool relative) {
  const auto number = [](const js::Value& value, float fallback) {
    if (value.IsUndefined()) {
      return fallback;
    }
    const double converted = js::ToNumber(value);
    return std::isfinite(converted) ? static_cast<float>(converted) : 0.0f;
  };
  const js::Value first = Argument(call.arguments, 0);
  const float base_x = relative ? current.scroll_x : 0.0f;
  const float base_y = relative ? current.scroll_y : 0.0f;
  if (first.IsObject()) {
    const js::Value* left = first.object->Get("left");
    const js::Value* top = first.object->Get("top");
    return ScrollTarget{
        base_x + number(left == nullptr ? js::Value::Undefined() : *left,
                        relative ? 0.0f : current.scroll_x - base_x),
        base_y + number(top == nullptr ? js::Value::Undefined() : *top,
                        relative ? 0.0f : current.scroll_y - base_y)};
  }
  return ScrollTarget{base_x + number(first, relative ? 0.0f : current.scroll_x - base_x),
                      base_y + number(Argument(call.arguments, 1),
                                      relative ? 0.0f : current.scroll_y - base_y)};
}

}  // namespace

js::Value MakeDomRect(js::Interpreter& interpreter, const GeometryRect& rect) {
  const Value result = interpreter.NewObjectValue();
  if (!result.IsObject()) {
    return Value::Undefined();
  }
  const auto number = [](float value) { return Value::Number(static_cast<double>(value)); };
  result.object->Set("x", number(rect.x));
  result.object->Set("y", number(rect.y));
  result.object->Set("width", number(rect.width));
  result.object->Set("height", number(rect.height));
  result.object->Set("left", number(rect.x));
  result.object->Set("top", number(rect.y));
  result.object->Set("right", number(rect.x + rect.width));
  result.object->Set("bottom", number(rect.y + rect.height));
  return result;
}

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
        return MakeDomRect(*owner->interpreter_, box);
      });
  if (rect_of.IsObject()) {
    rect_of.object->Set(kOwnerSlot, PointerValue(this));
    element_interface.object->Set("getBoundingClientRect", rect_of);
  }

  // `getClientRects`. CSSOM View: one DOMRect per CSS border box fragment, or
  // an empty list when the element has no box. ADR 0012 lists it with
  // `getBoundingClientRect`; youtube's overlay/dialog code calls it and threw
  // `getClientRects is not a function` (TD-0051). Fragmentation is still one
  // border box today — the honest empty/one answer, not a stub that lies.
  const Value rects_of = interpreter_->NewNativeValue(
      "getClientRects", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
          return Value::Undefined();
        }
        std::vector<Value> rects;
        if (const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self)) {
          rects.push_back(MakeDomRect(*owner->interpreter_, found->border_box));
        }
        return owner->interpreter_->NewArrayValue(std::move(rects));
      });
  if (rects_of.IsObject()) {
    rects_of.object->Set(kOwnerSlot, PointerValue(this));
    element_interface.object->Set("getClientRects", rects_of);
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

  InstallScroll(element_interface);
}

void DomBindings::InstallScroll(const js::Value& element_interface) {
  // `scrollTop` at 254 measured occurrences is the most-used member of this
  // whole family -- more than `getBoundingClientRect` -- because it is how a
  // feed restores its position, how a virtualised list decides which rows
  // exist, and how a chat log stays at the bottom. ADR 0018 §1.
  //
  // The four are one shape: read a box's scroll state, pick a number out of it.
  // The two offsets take a setter and the two sizes do not, which is exactly
  // what the specification says and is the difference between a page that can
  // scroll itself and one that silently cannot.
  struct Metric {
    const char* name;
    bool vertical;
    bool writable;
  };
  static constexpr Metric kMetrics[] = {
      {"scrollTop", true, true},
      {"scrollLeft", false, true},
      {"scrollHeight", true, false},
      {"scrollWidth", false, false},
  };
  for (const Metric& metric : kMetrics) {
    const bool vertical = metric.vertical;
    const bool writable = metric.writable;
    const Value getter =
        interpreter_->NewNativeValue(metric.name, [vertical, writable](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          dom::Node* self = NodeOf(call.self);
          if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
            return Value::Number(0.0);
          }
          const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self);
          if (!found.has_value()) {
            return Value::Number(0.0);
          }
          if (writable) {
            return Value::Number(static_cast<double>(vertical ? found->scroll_y
                                                              : found->scroll_x));
          }
          // The sizes are integers and the offsets are not: an offset is a
          // fractional position a page may have written, and rounding it on the
          // way out would make `el.scrollTop = el.scrollTop` move the box.
          return Value::Number(
              Rounded(vertical ? found->scroll_height : found->scroll_width));
        });
    if (!getter.IsObject()) {
      continue;
    }
    getter.object->Set(kOwnerSlot, PointerValue(this));
    js::Object* setter = nullptr;
    if (writable) {
      const Value assign =
          interpreter_->NewNativeValue(metric.name, [vertical](NativeCall& call) -> Value {
            DomBindings* owner = OwnerOf(call);
            dom::Node* self = NodeOf(call.self);
            if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
              return Value::Undefined();
            }
            // The other axis is read back rather than assumed zero: writing
            // `scrollTop` must not reset `scrollLeft`, and there is no way to
            // set one axis without naming both.
            const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self);
            if (!found.has_value()) {
              return Value::Undefined();
            }
            const double wanted = js::ToNumber(Argument(call.arguments, 0));
            // A NaN is zero, which is what the specification's conversion says
            // and what stops `el.scrollTop = undefined` from moving a box to a
            // position no arithmetic can recover from.
            const float value =
                std::isfinite(wanted) ? static_cast<float>(wanted) : 0.0f;
            owner->geometry_->SetScrollOffset(*self, vertical ? found->scroll_x : value,
                                              vertical ? value : found->scroll_y);
            return Value::Undefined();
          });
      if (assign.IsObject()) {
        assign.object->Set(kOwnerSlot, PointerValue(this));
        setter = assign.object;
      }
    }
    element_interface.object->DefineAccessor(metric.name, getter.object, setter);
  }

  // `scrollTo`, `scrollBy` and `scrollIntoView`. The first two are the same
  // write with the arithmetic done for the caller; the third is the one worth
  // implementing carefully, because it has to move *every* scrolling ancestor.
  static constexpr Method kMethods[] = {{"scrollTo", false}, {"scrollBy", true}};
  for (const Method& method : kMethods) {
    const bool relative = method.relative;
    const Value fn =
        interpreter_->NewNativeValue(method.name, [relative](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          dom::Node* self = NodeOf(call.self);
          if (owner == nullptr || self == nullptr || owner->geometry_ == nullptr) {
            return Value::Undefined();
          }
          const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*self);
          if (!found.has_value()) {
            return Value::Undefined();
          }
          const ScrollTarget wanted = ScrollTargetFrom(call, *found, relative);
          owner->geometry_->SetScrollOffset(*self, wanted.x, wanted.y);
          return Value::Undefined();
        });
    if (fn.IsObject()) {
      fn.object->Set(kOwnerSlot, PointerValue(this));
      element_interface.object->Set(method.name, fn);
    }
  }

  const Value into_view =
      interpreter_->NewNativeValue("scrollIntoView", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner != nullptr && self != nullptr && owner->geometry_ != nullptr) {
          // The argument -- `true`, `false`, or an options object naming a
          // block alignment -- is deliberately ignored rather than half
          // honoured: every value of it scrolls the element into the
          // scrollport, and the difference is where in the scrollport it lands.
          // Doing the start alignment for all of them is an approximation a
          // page recovers from; not scrolling at all is not.
          owner->geometry_->ScrollIntoView(*self);
        }
        return Value::Undefined();
      });
  if (into_view.IsObject()) {
    into_view.object->Set(kOwnerSlot, PointerValue(this));
    element_interface.object->Set("scrollIntoView", into_view);
  }

  InstallWindowScroll();
  InstallMatchMedia();
}

// `window.scrollX`/`scrollY` and `window.scrollTo`/`scrollBy`, all four of
// which are the document element's scroll state under another name. Installed
// beside the element ones so the two cannot drift: a page that reads
// `window.scrollY` and one that reads `document.documentElement.scrollTop` are
// asking the same question, and two implementations of it is how they come to
// disagree by a pixel.
void DomBindings::InstallWindowScroll() {
  if (interpreter_ == nullptr || document_ == nullptr) {
    return;
  }
  dom::Element* root = document_->DocumentElement();
  if (root == nullptr) {
    return;
  }
  js::Object* global = interpreter_->Global();

  // `scrollX`/`scrollY` and the two older names for them, which plenty of
  // shipped script still reads.
  struct Reading {
    const char* name;
    bool vertical;
  };
  static constexpr Reading kReadings[] = {{"scrollX", false},
                                          {"scrollY", true},
                                          {"pageXOffset", false},
                                          {"pageYOffset", true}};
  for (const Reading& reading : kReadings) {
    const bool vertical = reading.vertical;
    const Value getter =
        interpreter_->NewNativeValue(reading.name, [vertical](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          dom::Element* element = owner == nullptr || owner->document_ == nullptr
                                      ? nullptr
                                      : owner->document_->DocumentElement();
          if (owner == nullptr || element == nullptr || owner->geometry_ == nullptr) {
            return Value::Number(0.0);
          }
          const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*element);
          if (!found.has_value()) {
            return Value::Number(0.0);
          }
          return Value::Number(static_cast<double>(vertical ? found->scroll_y : found->scroll_x));
        });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      global->DefineAccessor(reading.name, getter.object, nullptr);
    }
  }

  // `innerWidth`/`innerHeight`, which are the scrollport and nothing else.
  //
  // They were simply absent, which for a browser that had a viewport all along
  // is an omission rather than a decision: a page that sizes a carousel from
  // `window.innerWidth` got `undefined` and laid it out at `NaN` pixels. They
  // are here rather than in InstallWindow because this is the file that is
  // allowed to ask the geometry seam a question, and the viewport is one.
  struct Extent {
    const char* name;
    bool vertical;
  };
  static constexpr Extent kExtents[] = {{"innerWidth", false}, {"innerHeight", true}};
  for (const Extent& extent : kExtents) {
    const bool vertical = extent.vertical;
    const Value getter = interpreter_->NewNativeValue(extent.name, [vertical](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->geometry_ == nullptr) {
        return Value::Number(0.0);
      }
      const GeometryRect viewport = owner->geometry_->QueryViewport();
      // **Quantised** (ADR 0029 §6). The window's exact pixel size is one of the highest-entropy
      // things a page can read without asking for anything -- a user resizes a window to a number
      // nobody else has -- and rounding down collapses it. Down rather than to-nearest, so a page
      // that lays out to the reported width fits inside the real one.
      return Value::Number(static_cast<double>(QuantizeViewportExtent(
          static_cast<int>(vertical ? viewport.height : viewport.width))));
    });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      global->DefineAccessor(extent.name, getter.object, nullptr);
    }
  }

  static constexpr Method kWindowMethods[] = {{"scrollTo", false}, {"scrollBy", true}};
  for (const Method& method : kWindowMethods) {
    const bool relative = method.relative;
    const Value fn =
        interpreter_->NewNativeValue(method.name, [relative](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          dom::Element* element = owner == nullptr || owner->document_ == nullptr
                                      ? nullptr
                                      : owner->document_->DocumentElement();
          if (owner == nullptr || element == nullptr || owner->geometry_ == nullptr) {
            return Value::Undefined();
          }
          const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*element);
          if (!found.has_value()) {
            return Value::Undefined();
          }
          const ScrollTarget wanted = ScrollTargetFrom(call, *found, relative);
          owner->geometry_->SetScrollOffset(*element, wanted.x, wanted.y);
          return Value::Undefined();
        });
    if (fn.IsObject()) {
      fn.object->Set(kOwnerSlot, PointerValue(this));
      global->Set(method.name, fn);
      interpreter_->GlobalScope()->Declare(method.name, fn, false);
    }
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
