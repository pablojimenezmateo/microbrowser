// `IntersectionObserver` and `ResizeObserver` -- 53 and 96 occurrences in the
// survey, and the two APIs a feed is built out of.
//
// The shape is the point, and it is not the obvious one. Neither observer is a
// callback that fires when something moves: both are *geometry sampled once per
// frame*, compared against what was reported last time, and delivered only when
// the answer changed. ADR 0018 §5 puts them here rather than with the frame work
// because they need exactly two things the scroll model provides -- a scrollport
// to intersect against, and a frame at which to sample -- and nothing else.
//
// Three properties this file has to hold:
//
//   * **Never synchronously from a scroll.** A wheel notch moves an offset and
//     nothing else; the sample happens at the frame that follows. A page with
//     twelve observers must not run twelve callbacks per notch, and an observer
//     that fired from inside the scroll could see a layout half-way through
//     being updated.
//   * **Sample everything, then deliver everything.** A callback is free to
//     mutate the document, so a delivery interleaved with sampling would hand
//     the second observer a different page than the first one saw.
//   * **Nothing is scheduled.** There is no timer here and no poll. A page with
//     no observers costs one pointer comparison per frame, and a page with
//     observers costs nothing at all while no frame is being produced -- which
//     is what keeps the zero-idle-CPU invariant intact.
//
// State lives in JavaScript objects hung off the interfaces object, for the
// reason MutationObserver's does: a `js::Value` in a C++ field is invisible to
// the collector, and a callback it cannot see is one it frees while an observer
// still points at it.
//
// **Deviations, both deliberate and both recorded rather than hidden.** The
// intersection is computed against the root alone and not clipped by every
// scrolling container in between, so a target positioned inside the viewport but
// hidden by an intermediate `overflow: hidden` ancestor is reported as visible.
// Closing that needs the geometry seam to answer "which of my ancestors clip",
// which it does not. And `devicePixelContentBoxSize` is absent rather than
// approximated: it is the device pixel ratio times a size, and reporting one is
// a fingerprinting decision (ADR 0029) rather than a geometry one.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/ViewObservers.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

namespace {

// The list of live view observers, on the interfaces object.
constexpr const char* kViewObserversSlot = "#viewObservers";
// On an observer: what it calls, what it watches, what it has queued, and
// which of the two it is.
constexpr const char* kCallbackSlot = "#callback";
constexpr const char* kTargetsSlot = "#targets";
constexpr const char* kRecordsSlot = "#records";
constexpr const char* kKindSlot = "#kind";
// IntersectionObserver only: the sorted thresholds, the four margins as
// written, and the root element (undefined for the viewport).
constexpr const char* kThresholdsSlot = "#thresholds";
constexpr const char* kMarginSlot = "#margin";
constexpr const char* kRootSlot = "#root";

// A number rather than a string, because the sampler compares it once per
// target per frame and a string compare there is the one avoidable cost in
// this file.
constexpr double kIntersectionKind = 0.0;
constexpr double kResizeKind = 1.0;

// On a registration -- what was last *reported* for this target, which is what
// makes an observer fire on a change rather than on every frame.
constexpr const char* kNodeKey = "node";
constexpr const char* kPreviousIndexKey = "#index";
constexpr const char* kPreviousIntersectingKey = "#intersecting";
constexpr const char* kPreviousWidthKey = "#width";
constexpr const char* kPreviousHeightKey = "#height";
// Which box a ResizeObserver was asked to watch. The reported record carries
// both sizes either way; this decides which one a *change* is measured on.
constexpr const char* kBorderBoxKey = "#borderBox";

float Area(const GeometryRect& rect) { return rect.width * rect.height; }

// The overlap of two rectangles, and whether there is one.
//
// `intersects` is not `Area(out) > 0`: a target with a zero height -- an empty
// `<div>` a feed uses as its sentinel, which is the single most common thing
// anybody observes -- has no area and is still either inside the root or not.
struct Overlap {
  GeometryRect rect;
  bool intersects = false;
};

Overlap Intersect(const GeometryRect& target, const GeometryRect& root) {
  const float left = std::max(target.x, root.x);
  const float top = std::max(target.y, root.y);
  const float right = std::min(target.x + target.width, root.x + root.width);
  const float bottom = std::min(target.y + target.height, root.y + root.height);
  if (right < left || bottom < top) {
    return Overlap{};
  }
  Overlap out;
  out.rect = GeometryRect{left, top, right - left, bottom - top};
  out.intersects = (out.rect.width > 0.0f || target.width == 0.0f) &&
                   (out.rect.height > 0.0f || target.height == 0.0f);
  // A laid-out box with no height is still "on screen" for lazy media.
  // youtube's thumbnail loader observes the <img> (often 0×0 inside a
  // 500×0 host) and filters `intersectionRect.height > 0` before assigning
  // `src`. Inflate a one-pixel intersection for any intersecting zero-height
  // target so that callback runs; a box that missed the root stays at 0.
  if (out.intersects && out.rect.height <= 0.0f) {
    out.rect.height = 1.0f;
    if (out.rect.width <= 0.0f) {
      out.rect.width = 1.0f;
    }
  }
  return out;
}

// The thresholds an observer was constructed with, sorted, out-of-range values
// clamped. The specification throws a RangeError for a threshold outside
// [0, 1]; clamping instead is deliberate, because the value that reaches here
// is usually a computed one and a page that gets 1.0000000001 out of a
// division should observe rather than fail to construct.
std::vector<double> ThresholdsFrom(const Value& option) {
  std::vector<double> out;
  const auto add = [&out](const Value& value) {
    const double number = js::ToNumber(value);
    if (std::isfinite(number)) {
      out.push_back(std::clamp(number, 0.0, 1.0));
    }
  };
  if (option.IsObject() && !option.object->IsCallable()) {
    for (std::size_t i = 0; i < option.object->ElementCount(); ++i) {
      add(option.object->GetElement(i));
    }
  } else if (!option.IsUndefined()) {
    add(option);
  }
  if (out.empty()) {
    out.push_back(0.0);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// The index of the first threshold greater than `ratio`, or the count when
// there is none -- the specification's own step, and the number an observer
// actually compares. Zero when nothing intersects, which is what makes leaving
// the root a notification rather than silence.
int ThresholdIndex(const std::vector<double>& thresholds, double ratio, bool intersecting) {
  if (!intersecting) {
    return 0;
  }
  for (std::size_t i = 0; i < thresholds.size(); ++i) {
    if (thresholds[i] > ratio) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(thresholds.size());
}

double NumberProperty(const Value& holder, const char* key, double fallback) {
  if (!holder.IsObject()) {
    return fallback;
  }
  const Value* found = holder.object->GetOwn(key);
  return found != nullptr && found->IsNumber() ? found->number : fallback;
}

bool BoolProperty(const Value& holder, const char* key) {
  if (!holder.IsObject()) {
    return false;
  }
  const Value* found = holder.object->GetOwn(key);
  return found != nullptr && js::ToBoolean(*found);
}

// Whether `node` is `ancestor` or is inside it. An element root observes only
// its own descendants; anything else is reported as not intersecting rather
// than measured against a box it has no relationship to.
bool IsWithin(const dom::Node* node, const dom::Node* ancestor) {
  for (const dom::Node* at = node; at != nullptr; at = at->Parent()) {
    if (at == ancestor) {
      return true;
    }
  }
  return false;
}

}  // namespace

// A margin larger than any document, which is what a bound has to be to be
// meaningless in practice and finite in arithmetic. `kMaxSvgEdge` is 4096 and
// the tallest page anyone has is orders below this.
namespace {
constexpr float kMaxRootMargin = 1.0e6f;

// One component of a `rootMargin`, in CSS pixels. Percentages resolve against
// the root's size along the axis this component applies to.
float ParseMarginComponent(const std::string& text, float percent_basis) {
  if (text.empty()) {
    return 0.0f;
  }
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(value)) {
    return 0.0f;
  }
  const std::string unit(end);
  // A double that is finite and a float that is not: 1e300 parses cleanly and
  // becomes `inf` the moment it is narrowed, which is where the NaN ratio in
  // this header's comment comes from. Clamped as a double, before the cast.
  const double basis = static_cast<double>(percent_basis);
  const double pixels = unit == "%" ? value * basis / 100.0 : value;
  if (!std::isfinite(pixels)) {
    return 0.0f;
  }
  return static_cast<float>(
      std::clamp(pixels, -static_cast<double>(kMaxRootMargin), static_cast<double>(kMaxRootMargin)));
}
}  // namespace

RootMargin ParseRootMargin(std::string_view text, const GeometryRect& root) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      if (!current.empty()) {
        parts.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    parts.push_back(std::move(current));
  }
  // Four components at most, and a fifth makes the whole value invalid rather
  // than being ignored -- which is what the CSS shorthand rule says and stops
  // an attribute of ten thousand numbers from being a parse of ten thousand.
  if (parts.empty() || parts.size() > 4) {
    return RootMargin{};
  }
  while (parts.size() < 4) {
    parts.push_back(parts[parts.size() == 1 ? 0 : parts.size() - 2]);
  }
  return RootMargin{ParseMarginComponent(parts[0], root.height),
                    ParseMarginComponent(parts[1], root.width),
                    ParseMarginComponent(parts[2], root.height),
                    ParseMarginComponent(parts[3], root.width)};
}

GeometryRect ExpandedBy(const GeometryRect& rect, const RootMargin& margin) {
  return GeometryRect{rect.x - margin.left, rect.y - margin.top,
                      rect.width + margin.left + margin.right,
                      rect.height + margin.top + margin.bottom};
}

js::Value DomBindings::ViewObserverList() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kViewObserversSlot)) {
    return *existing;
  }
  const Value list = interpreter_->NewArrayValue({});
  if (list.IsObject()) {
    interfaces_.object->Set(kViewObserversSlot, list);
  }
  return list;
}

bool DomBindings::DeliverViewObservations(double time_ms) {
  if (interpreter_ == nullptr || geometry_ == nullptr || !interfaces_.IsObject()) {
    return false;
  }
  // Deliberately *not* ViewObserverList(), which would create the list. A page
  // that never constructed an observer must not allocate one array per frame
  // for the privilege of having none.
  const Value* list = interfaces_.object->GetOwn(kViewObserversSlot);
  if (list == nullptr || !list->IsObject() || list->object->ElementCount() == 0) {
    return false;
  }
  const Value observers = *list;
  AddPerformanceCounter(PerfCounterId::ViewObservationFrames);

  // Two passes, and the split is load-bearing: a callback may mutate the
  // document, and an observer sampled after that one ran would be describing a
  // different page than the observer before it.
  for (std::size_t i = 0; i < observers.object->ElementCount(); ++i) {
    SampleViewObserver(observers.object->GetElement(i), time_ms);
  }

  bool ran = false;
  for (std::size_t i = 0; i < observers.object->ElementCount(); ++i) {
    const Value observer = observers.object->GetElement(i);
    if (!observer.IsObject()) {
      continue;
    }
    const Value* records = observer.object->GetOwn(kRecordsSlot);
    const Value* callback = observer.object->GetOwn(kCallbackSlot);
    if (records == nullptr || !records->IsObject() || records->object->ElementCount() == 0 ||
        callback == nullptr || !callback->IsObject() || !callback->object->IsCallable()) {
      continue;
    }
    std::vector<Value> taken;
    taken.reserve(records->object->ElementCount());
    for (std::size_t r = 0; r < records->object->ElementCount(); ++r) {
      taken.push_back(records->object->GetElement(r));
    }
    records->object->SetElements({}, {});
    ran = true;
    // A fresh host turn: observer callbacks often run in the same Engine turn
    // as the rAF that stamped the nodes they watch (LayoutAndPaint follows
    // RunDueWork). Without BeginTask, youtube's lazy-img callback inherits a
    // spent step budget and never assigns `src`.
    interpreter_->BeginTask();
    // (records, observer) -- the signature every page writes against.
    const js::Result delivered =
        interpreter_->CallFunction(*callback, Value::Undefined(),
                                   {interpreter_->NewArrayValue(std::move(taken)), observer});
    if (delivered.completion == js::Completion::Throw) {
      interpreter_->ReportUncaught(delivered.value, "observer callback");
    }
  }
  if (ran) {
    // A delivery is a turn of its own, so anything a callback queued settles
    // before the frame is over -- the rule a script, an event, a timer and an
    // animation frame all get.
    interpreter_->DrainMicrotasks();
  }
  return ran;
}

void DomBindings::SampleViewObserver(const js::Value& observer, double time_ms) {
  if (!observer.IsObject() || geometry_ == nullptr) {
    return;
  }
  const Value* targets = observer.object->GetOwn(kTargetsSlot);
  const Value* records = observer.object->GetOwn(kRecordsSlot);
  if (targets == nullptr || !targets->IsObject() || records == nullptr || !records->IsObject()) {
    return;
  }
  const bool intersection = NumberProperty(observer, kKindSlot, kIntersectionKind) ==
                            kIntersectionKind;

  // The root, measured once for the whole observer rather than once per target.
  GeometryRect root_bounds;
  dom::Node* root_node = nullptr;
  if (intersection) {
    const Value* root = observer.object->GetOwn(kRootSlot);
    root_node = root == nullptr ? nullptr : NodeOf(*root);
    if (root_node != nullptr) {
      const std::optional<BoxGeometry> box = geometry_->QueryBox(*root_node);
      if (!box.has_value()) {
        return;  // a root with no box observes nothing, rather than everything
      }
      root_bounds = box->content_box;
    } else {
      root_bounds = geometry_->QueryViewport();
    }
    const Value* margin = observer.object->GetOwn(kMarginSlot);
    if (margin != nullptr && margin->IsString()) {
      root_bounds = ExpandedBy(root_bounds, ParseRootMargin(js::ToString(*margin), root_bounds));
    }
  }

  std::vector<double> thresholds;
  if (intersection) {
    const Value* stored = observer.object->GetOwn(kThresholdsSlot);
    if (stored != nullptr && stored->IsObject()) {
      for (std::size_t i = 0; i < stored->object->ElementCount(); ++i) {
        thresholds.push_back(js::ToNumber(stored->object->GetElement(i)));
      }
    }
    if (thresholds.empty()) {
      thresholds.push_back(0.0);
    }
  }

  for (std::size_t i = 0; i < targets->object->ElementCount(); ++i) {
    const Value registration = targets->object->GetElement(i);
    if (!registration.IsObject()) {
      continue;
    }
    const Value* wrapper = registration.object->GetOwn(kNodeKey);
    dom::Node* target = wrapper == nullptr ? nullptr : NodeOf(*wrapper);
    if (target == nullptr) {
      continue;
    }
    const std::optional<BoxGeometry> box = geometry_->QueryBox(*target);

    if (intersection) {
      AddPerformanceCounter(PerfCounterId::ViewIntersectionSamples);
      Overlap overlap;
      GeometryRect target_rect;
      if (box.has_value() && (root_node == nullptr || IsWithin(target, root_node))) {
        target_rect = box->border_box;
        overlap = Intersect(target_rect, root_bounds);
      }
      const double ratio = Area(target_rect) > 0.0f
                               ? static_cast<double>(Area(overlap.rect) / Area(target_rect))
                               : (overlap.intersects ? 1.0 : 0.0);
      const int index = ThresholdIndex(thresholds, ratio, overlap.intersects);
      // -1, not 0: the first sample of a target must be delivered even when it
      // is not intersecting, which is what `observe()` promises and what a
      // lazy loader depends on to learn that nothing is on screen yet.
      const int previous = static_cast<int>(
          NumberProperty(registration, kPreviousIndexKey, -1.0));
      const bool was = BoolProperty(registration, kPreviousIntersectingKey);
      if (index == previous && overlap.intersects == was) {
        continue;
      }
      registration.object->Set(kPreviousIndexKey, Value::Number(static_cast<double>(index)));
      registration.object->Set(kPreviousIntersectingKey, Value::Bool(overlap.intersects));

      const Value record = interpreter_->NewObjectValue();
      if (!record.IsObject()) {
        continue;
      }
      record.object->Set("time", Value::Number(time_ms));
      record.object->Set("target", *wrapper);
      record.object->Set("rootBounds", MakeDomRect(*interpreter_, root_bounds));
      record.object->Set("boundingClientRect", MakeDomRect(*interpreter_, target_rect));
      record.object->Set("intersectionRect", MakeDomRect(*interpreter_, overlap.rect));
      record.object->Set("intersectionRatio", Value::Number(ratio));
      record.object->Set("isIntersecting", Value::Bool(overlap.intersects));
      records->object->PushElement(record);
      AddPerformanceCounter(PerfCounterId::ViewIntersectionRecords);
      continue;
    }

    AddPerformanceCounter(PerfCounterId::ViewResizeSamples);
    if (!box.has_value()) {
      // An element that stopped generating a box is not a resize to zero; it
      // has no size at all, and reporting one would make a page lay itself out
      // for a box that is not on the page.
      continue;
    }
    const bool border = BoolProperty(registration, kBorderBoxKey);
    const GeometryRect& watched = border ? box->border_box : box->content_box;
    // NaN as the "never reported" marker, because every comparison against it
    // is false -- so the first sample always delivers and no separate flag can
    // fall out of step with the two numbers it guards.
    const double previous_width =
        NumberProperty(registration, kPreviousWidthKey, std::nan(""));
    const double previous_height =
        NumberProperty(registration, kPreviousHeightKey, std::nan(""));
    if (previous_width == static_cast<double>(watched.width) &&
        previous_height == static_cast<double>(watched.height)) {
      continue;
    }
    registration.object->Set(kPreviousWidthKey, Value::Number(static_cast<double>(watched.width)));
    registration.object->Set(kPreviousHeightKey,
                             Value::Number(static_cast<double>(watched.height)));

    const Value record = interpreter_->NewObjectValue();
    if (!record.IsObject()) {
      continue;
    }
    record.object->Set("target", *wrapper);
    // `contentRect`'s origin is the padding box, not the viewport: it is the
    // padding, which is what a page that positions something inside the
    // element reads it for.
    record.object->Set(
        "contentRect",
        MakeDomRect(*interpreter_, GeometryRect{box->content_box.x - box->padding_box.x,
                                                box->content_box.y - box->padding_box.y,
                                                box->content_box.width,
                                                box->content_box.height}));
    const auto size_list = [this](const GeometryRect& rect) {
      const Value entry = interpreter_->NewObjectValue();
      if (!entry.IsObject()) {
        return Value::Undefined();
      }
      // Inline and block rather than width and height, because the
      // specification is written in writing-mode terms. This engine has one
      // writing mode, so they are the same two numbers -- and naming them the
      // way the specification does is what makes a page written against it
      // read the right one.
      entry.object->Set("inlineSize", Value::Number(static_cast<double>(rect.width)));
      entry.object->Set("blockSize", Value::Number(static_cast<double>(rect.height)));
      return interpreter_->NewArrayValue({entry});
    };
    record.object->Set("borderBoxSize", size_list(box->border_box));
    record.object->Set("contentBoxSize", size_list(box->content_box));
    records->object->PushElement(record);
    AddPerformanceCounter(PerfCounterId::ViewResizeRecords);
  }
}

void DomBindings::InstallViewObservers() {
  DomBindings* self = this;
  // The two constructors differ in four lines and share everything else, which
  // is why they are one lambda parameterised by kind rather than two that will
  // drift.
  struct Kind {
    const char* name;
    double kind;
  };
  static constexpr Kind kKinds[] = {{"IntersectionObserver", kIntersectionKind},
                                    {"ResizeObserver", kResizeKind}};
  for (const Kind& each : kKinds) {
    const double kind = each.kind;
    const char* name = each.name;
    const Value constructor =
        interpreter_->NewNativeValue(name, [self, kind, name](NativeCall& call) {
          const Value callback = Argument(call.arguments, 0);
          if (!callback.IsObject() || !callback.object->IsCallable()) {
            return call.Throw("TypeError", std::string(name) + " needs a callback");
          }
          const Value observer = call.interpreter.NewObjectValue();
          if (!observer.IsObject()) {
            return Value::Undefined();
          }
          observer.object->Set(kCallbackSlot, callback);
          observer.object->Set(kTargetsSlot, call.interpreter.NewArrayValue({}));
          observer.object->Set(kRecordsSlot, call.interpreter.NewArrayValue({}));
          observer.object->Set(kKindSlot, Value::Number(kind));

          const Value options = Argument(call.arguments, 1);
          if (kind == kIntersectionKind) {
            // Read once, at construction. The page is free to mutate the object
            // it passed afterwards and that must not change what is observed.
            Value threshold = Value::Undefined();
            Value margin = Value::String(std::string("0px"));
            Value root = Value::Undefined();
            if (options.IsObject()) {
              if (const Value* found = options.object->Get("threshold")) {
                threshold = *found;
              }
              if (const Value* found = options.object->Get("rootMargin")) {
                margin = Value::String(js::ToString(*found));
              }
              if (const Value* found = options.object->Get("root")) {
                root = *found;
              }
            }
            std::vector<Value> thresholds;
            for (const double value : ThresholdsFrom(threshold)) {
              thresholds.push_back(Value::Number(value));
            }
            observer.object->Set(kThresholdsSlot,
                                 call.interpreter.NewArrayValue(std::move(thresholds)));
            observer.object->Set(kMarginSlot, margin);
            observer.object->Set(kRootSlot, NodeOf(root) == nullptr ? Value::Null() : root);
            // The three properties a page reads back off an observer. `root` is
            // whatever it gave, so identity holds.
            observer.object->Set("root", *observer.object->GetOwn(kRootSlot));
            observer.object->Set("rootMargin", margin);
            observer.object->Set("thresholds", *observer.object->GetOwn(kThresholdsSlot));
          }

          const auto method = [&](const char* method_name, js::NativeFunction function) {
            const Value native =
                call.interpreter.NewNativeValue(method_name, std::move(function));
            if (native.IsObject()) {
              native.object->Set(kOwnerSlot, OwnerValue(self));
              observer.object->Set(method_name, native);
            }
          };

          method("observe", [](NativeCall& inner) {
            if (!inner.self.IsObject()) {
              return Value::Undefined();
            }
            const Value target = Argument(inner.arguments, 0);
            if (NodeOf(target) == nullptr) {
              return inner.Throw("TypeError", "observe needs an element");
            }
            const Value* targets = inner.self.object->GetOwn(kTargetsSlot);
            if (targets == nullptr || !targets->IsObject()) {
              return Value::Undefined();
            }
            // Observing the same target twice is one registration, and the
            // second call re-reads its options -- which is what the
            // specification says and what a component that re-observes on every
            // render depends on to not accumulate duplicates.
            for (std::size_t i = 0; i < targets->object->ElementCount(); ++i) {
              const Value existing = targets->object->GetElement(i);
              if (existing.IsObject()) {
                const Value* node = existing.object->GetOwn(kNodeKey);
                if (node != nullptr && NodeOf(*node) == NodeOf(target)) {
                  return Value::Undefined();
                }
              }
            }
            const Value registration = inner.interpreter.NewObjectValue();
            if (!registration.IsObject()) {
              return Value::Undefined();
            }
            registration.object->Set(kNodeKey, target);
            const Value observe_options = Argument(inner.arguments, 1);
            if (observe_options.IsObject()) {
              if (const Value* box = observe_options.object->Get("box")) {
                const std::string wanted = js::ToString(*box);
                if (wanted == "device-pixel-content-box") {
                  // Refused rather than approximated. It is the device pixel
                  // ratio times a size, and answering with a CSS-pixel size
                  // under that name would make a canvas render at the wrong
                  // resolution and look like a driver bug. A page that
                  // feature-detects it in a try/catch gets the fallback it
                  // wrote; ADR 0012.
                  return inner.Throw("TypeError",
                                     "device-pixel-content-box is not supported");
                }
                registration.object->Set(kBorderBoxKey,
                                         Value::Bool(wanted == "border-box"));
              }
            }
            targets->object->PushElement(registration);
            // `observe()` promises an initial sample, and Lit/Polymer often
            // call it from a post-paint effect after the stamp rAF has already
            // ended. Without scheduling a frame here, nothing wakes the loop
            // and youtube's lazy imgs keep `onViewportEntered` forever.
            if (js::Object* global = inner.interpreter.Global()) {
              if (const Value* raf = global->Get("requestAnimationFrame");
                  raf != nullptr && raf->IsObject() && raf->object->IsCallable()) {
                const Value noop = inner.interpreter.NewNativeValue(
                    "observationFrame", [](NativeCall&) { return Value::Undefined(); });
                if (noop.IsObject()) {
                  (void)inner.interpreter.CallFunction(*raf, Value::Undefined(), {noop});
                }
              }
            }
            return Value::Undefined();
          });

          method("unobserve", [](NativeCall& inner) {
            if (!inner.self.IsObject()) {
              return Value::Undefined();
            }
            const Value* targets = inner.self.object->GetOwn(kTargetsSlot);
            dom::Node* node = NodeOf(Argument(inner.arguments, 0));
            if (targets == nullptr || !targets->IsObject() || node == nullptr) {
              return Value::Undefined();
            }
            std::vector<Value> kept;
            for (std::size_t i = 0; i < targets->object->ElementCount(); ++i) {
              const Value existing = targets->object->GetElement(i);
              const Value* watched =
                  existing.IsObject() ? existing.object->GetOwn(kNodeKey) : nullptr;
              if (watched == nullptr || NodeOf(*watched) != node) {
                kept.push_back(existing);
              }
            }
            const std::vector<bool> present(kept.size(), true);
            targets->object->SetElements(std::move(kept), present);
            return Value::Undefined();
          });

          method("disconnect", [](NativeCall& inner) {
            if (!inner.self.IsObject()) {
              return Value::Undefined();
            }
            // Both halves: it watches nothing, and anything already queued is
            // dropped. An observer that fired once more after being
            // disconnected would be the worst of both.
            for (const char* slot : {kTargetsSlot, kRecordsSlot}) {
              const Value* list = inner.self.object->GetOwn(slot);
              if (list != nullptr && list->IsObject()) {
                list->object->SetElements({}, {});
              }
            }
            return Value::Undefined();
          });

          if (kind == kIntersectionKind) {
            method("takeRecords", [](NativeCall& inner) {
              const Value* records =
                  inner.self.IsObject() ? inner.self.object->GetOwn(kRecordsSlot) : nullptr;
              if (records == nullptr || !records->IsObject()) {
                return inner.interpreter.NewArrayValue({});
              }
              std::vector<Value> taken;
              for (std::size_t i = 0; i < records->object->ElementCount(); ++i) {
                taken.push_back(records->object->GetElement(i));
              }
              records->object->SetElements({}, {});
              return inner.interpreter.NewArrayValue(std::move(taken));
            });
          }

          const Value list = self->ViewObserverList();
          if (list.IsObject()) {
            list.object->PushElement(observer);
          }
          return observer;
        });
    if (constructor.IsObject()) {
      constructor.object->Set(kOwnerSlot, OwnerValue(this));
      interpreter_->Global()->Set(name, constructor);
      interpreter_->GlobalScope()->Declare(name, constructor, false);
    }
  }
}

}  // namespace microbrowser::bindings
