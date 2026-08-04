#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// Array.prototype and the Array constructor.
//
// In its own translation unit for the reason String.prototype is: it is the
// second-largest group of builtins, and Builtins.cpp is the file everything
// else lands in.
//
// Two rules run through all of it. A method called on something that is not an
// array answers as if it were empty rather than crashing -- `[].map.call(7,
// f)` is legal JavaScript. And a hole is not an undefined element: `map` and
// `slice` preserve holes, `filter` and `forEach` skip them, and `join` writes
// nothing for one. Getting that wrong is invisible until a page uses a sparse
// array, and then it is wrong everywhere at once.

namespace microbrowser::js {

namespace {

// The receiver as an array, or null. Null is not an error: the caller answers
// as if it were empty.
Object* Self(const NativeCall& call) {
  return call.self.IsObject() ? call.self.object : nullptr;
}

std::size_t LengthOf(const NativeCall& call) {
  const Object* self = Self(call);
  return self == nullptr ? 0 : self->ElementCount();
}

// A relative index, clamped. Negative counts back from the end, which is what
// makes `slice(-1)` and `at(-1)` idiomatic.
std::size_t Clamp(double index, std::size_t size) {
  const double limit = static_cast<double>(size);
  if (index < 0) {
    return static_cast<std::size_t>(std::max(0.0, limit + index));
  }
  return static_cast<std::size_t>(std::min(index, limit));
}

// Calls `callback(element, index, array)`, the shape every iterating method
// uses. False when it threw, with the reason left on `call`.
bool Visit(NativeCall& call, const Value& callback, const Value& receiver, std::size_t index,
           Value& out) {
  Object* self = Self(call);
  const Result called = call.interpreter.CallFunction(
      callback, receiver,
      {self->GetElement(index), Value::Number(static_cast<double>(index)), call.self});
  if (called.IsAbrupt()) {
    call.ThrowValue(called.value);
    return false;
  }
  out = called.value;
  return true;
}

bool IsCallable(const Value& value) {
  return value.IsObject() && value.object->IsCallable();
}

// A stable merge sort, written out rather than handed to std::sort.
//
// Two reasons, both load-bearing. The comparator is a page's function: it can
// throw, and std::sort cannot be stopped part-way; and it can be inconsistent
// -- `(a, b) => Math.random() - 0.5` is a real thing people write -- which
// makes std::sort read outside the range it was given. A merge sort is
// undisturbed by both, and the spec requires stability anyway.
bool MergeSort(NativeCall& call, std::vector<Value>& items, const Value& comparator) {
  const auto compare = [&call, &comparator](const Value& a, const Value& b, bool& less) {
    // Undefined sorts to the end whatever the comparator says, and is never
    // passed to it.
    if (a.IsUndefined() || b.IsUndefined()) {
      less = !a.IsUndefined() && b.IsUndefined();
      return true;
    }
    if (!IsCallable(comparator)) {
      // The default is a *string* comparison, which is why [1, 10, 2] sorts to
      // [1, 10, 2] and surprises everyone. Through the interpreter's
      // conversion, so an element with its own `toString` sorts by what it
      // says rather than by "[object Object]".
      std::string left;
      std::string right;
      const Result first = call.interpreter.ToStringOf(a, left);
      if (first.IsAbrupt()) {
        call.ThrowValue(first.value);
        return false;
      }
      const Result second = call.interpreter.ToStringOf(b, right);
      if (second.IsAbrupt()) {
        call.ThrowValue(second.value);
        return false;
      }
      less = left < right;
      return true;
    }
    const Result decided = call.interpreter.CallFunction(comparator, Value::Undefined(), {a, b});
    if (decided.IsAbrupt()) {
      call.ThrowValue(decided.value);
      return false;
    }
    less = ToNumber(decided.value) < 0.0;
    return true;
  };

  std::vector<Value> buffer(items.size());
  for (std::size_t width = 1; width < items.size(); width *= 2) {
    for (std::size_t start = 0; start < items.size(); start += 2 * width) {
      const std::size_t middle = std::min(start + width, items.size());
      const std::size_t end = std::min(start + 2 * width, items.size());
      std::size_t left = start;
      std::size_t right = middle;
      for (std::size_t at = start; at < end; ++at) {
        bool take_right = false;
        if (left >= middle) {
          take_right = true;
        } else if (right < end) {
          // `b < a` rather than `!(a < b)`: equal elements keep their order,
          // which is what stable means.
          bool less = false;
          if (!compare(items[right], items[left], less)) {
            return false;
          }
          take_right = less;
        }
        buffer[at] = take_right ? items[right++] : items[left++];
      }
    }
    items.swap(buffer);
  }
  return true;
}

}  // namespace

void Interpreter::InstallArrayPrototype() {
  Object* prototype = well_known_.array_prototype;
  const auto method = [this, prototype](const char* name, NativeFunction function) {
    InstallNative(prototype, name, std::move(function));
  };

  // --- Adding and removing --------------------------------------------------

  method("push", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return Value::Undefined();
    }
    if (self->ElementCount() + call.arguments.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "array is too long");
    }
    for (const Value& argument : call.arguments) {
      self->PushElement(argument);
    }
    return Value::Number(static_cast<double>(self->ElementCount()));
  });

  method("pop", [](NativeCall& call) {
    Object* self = Self(call);
    return self == nullptr || self->ElementCount() == 0 ? Value::Undefined() : self->PopElement();
  });

  method("shift", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr || self->ElementCount() == 0) {
      return Value::Undefined();
    }
    const Value first = self->GetElement(0);
    std::vector<Value> rest;
    std::vector<bool> present;
    for (std::size_t i = 1; i < self->ElementCount(); ++i) {
      rest.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    self->SetElements(std::move(rest), std::move(present));
    return first;
  });

  method("unshift", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return Value::Number(0);
    }
    if (self->ElementCount() + call.arguments.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "array is too long");
    }
    std::vector<Value> combined(call.arguments.begin(), call.arguments.end());
    std::vector<bool> present(call.arguments.size(), true);
    for (std::size_t i = 0; i < self->ElementCount(); ++i) {
      combined.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    self->SetElements(std::move(combined), std::move(present));
    return Value::Number(static_cast<double>(self->ElementCount()));
  });

  method("splice", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return call.interpreter.NewArrayValue({});
    }
    const std::size_t size = self->ElementCount();
    const std::size_t start = Clamp(ToNumber(Argument(call.arguments, 0)), size);
    // No second argument removes everything from `start`; an explicit one is
    // clamped. The two are different and a page relies on the difference.
    const std::size_t removable = size - start;
    const std::size_t count =
        call.arguments.size() < 2
            ? removable
            : static_cast<std::size_t>(
                  std::clamp(ToNumber(call.arguments[1]), 0.0,
                             static_cast<double>(removable)));

    std::vector<Value> removed;
    std::vector<Value> kept;
    std::vector<bool> present;
    for (std::size_t i = 0; i < start; ++i) {
      kept.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    for (std::size_t i = start; i < start + count; ++i) {
      removed.push_back(self->GetElement(i));
    }
    for (std::size_t i = 2; i < call.arguments.size(); ++i) {
      kept.push_back(call.arguments[i]);
      present.push_back(true);
    }
    for (std::size_t i = start + count; i < size; ++i) {
      kept.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    if (kept.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "array is too long");
    }
    self->SetElements(std::move(kept), std::move(present));
    return call.interpreter.NewArrayValue(std::move(removed));
  });

  // --- Reading --------------------------------------------------------------

  method("at", [](NativeCall& call) {
    Object* self = Self(call);
    const std::size_t size = LengthOf(call);
    double index = ToNumber(Argument(call.arguments, 0));
    if (index < 0) {
      index += static_cast<double>(size);
    }
    if (self == nullptr || index < 0 || index >= static_cast<double>(size)) {
      return Value::Undefined();
    }
    return self->GetElement(static_cast<std::size_t>(index));
  });

  // `join`, and the `toString` that is the same thing with a fixed separator.
  //
  // `toString` matters far past printing: it is what ToPrimitive reaches for,
  // so it is the reason `[] + {}` is "[object Object]" and `+[1]` is 1. Without
  // it an array inherits Object.prototype.toString and every one of those
  // answers is "[object Array]" instead.
  const auto join = [](NativeCall& call, const std::string& separator) {
    Object* self = Self(call);
    std::string joined;
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (i != 0) {
        joined += separator;
      }
      const Value element = self->GetElement(i);
      // A hole, a null and an undefined all contribute nothing -- which is why
      // `[null, 1].join('-')` is "-1" rather than "null-1".
      if (element.IsNullish()) {
        continue;
      }
      // Through the interpreter's conversion, not the pure one: an element
      // with its own `toString` has to run it, and it can throw.
      std::string text;
      const Result converted = call.interpreter.ToStringOf(element, text);
      if (converted.IsAbrupt()) {
        call.ThrowValue(converted.value);
        return std::string();
      }
      joined += text;
    }
    return joined;
  };
  method("join", [join](NativeCall& call) {
    const Value separator_value = Argument(call.arguments, 0);
    std::string separator = ",";
    if (!separator_value.IsUndefined()) {
      const Result converted = call.interpreter.ToStringOf(separator_value, separator);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
    }
    return Value::String(join(call, separator));
  });
  method("toString", [join](NativeCall& call) {
    return Value::String(join(call, ","));
  });
  method("toLocaleString", [join](NativeCall& call) {
    return Value::String(join(call, ","));
  });

  method("indexOf", [](NativeCall& call) {
    Object* self = Self(call);
    const Value needle = Argument(call.arguments, 0);
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (self->HasElement(i) && StrictEquals(self->GetElement(i), needle)) {
        return Value::Number(static_cast<double>(i));
      }
    }
    return Value::Number(-1);
  });

  method("lastIndexOf", [](NativeCall& call) {
    Object* self = Self(call);
    const Value needle = Argument(call.arguments, 0);
    for (std::size_t i = LengthOf(call); self != nullptr && i-- > 0;) {
      if (self->HasElement(i) && StrictEquals(self->GetElement(i), needle)) {
        return Value::Number(static_cast<double>(i));
      }
    }
    return Value::Number(-1);
  });

  method("includes", [](NativeCall& call) {
    Object* self = Self(call);
    const Value needle = Argument(call.arguments, 0);
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      const Value element = self->GetElement(i);
      // `includes` finds NaN where `indexOf` does not, which is the entire
      // reason it exists.
      if (StrictEquals(element, needle) ||
          (element.IsNumber() && needle.IsNumber() && std::isnan(element.number) &&
           std::isnan(needle.number))) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });

  method("slice", [](NativeCall& call) {
    Object* self = Self(call);
    const std::size_t size = LengthOf(call);
    const std::size_t begin = Clamp(ToNumber(Argument(call.arguments, 0)), size);
    const std::size_t end = call.arguments.size() < 2 || call.arguments[1].IsUndefined()
                                ? size
                                : Clamp(ToNumber(call.arguments[1]), size);
    std::vector<Value> out;
    std::vector<bool> present;
    for (std::size_t i = begin; i < end; ++i) {
      out.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));  // slice preserves holes
    }
    return call.interpreter.NewArrayValue(std::move(out), std::move(present));
  });

  method("concat", [](NativeCall& call) {
    std::vector<Value> out;
    std::vector<bool> present;
    const auto append = [&out, &present](const Value& value) {
      // An array argument is spread one level; anything else is appended
      // whole, including a string.
      if (value.IsObject() && value.object->GetKind() == Object::Kind::Array) {
        for (std::size_t i = 0; i < value.object->ElementCount(); ++i) {
          out.push_back(value.object->GetElement(i));
          present.push_back(value.object->HasElement(i));
        }
        return;
      }
      out.push_back(value);
      present.push_back(true);
    };
    append(call.self);
    for (const Value& argument : call.arguments) {
      append(argument);
    }
    if (out.size() > kMaxAllocationLength) {
      return call.Throw("RangeError", "array is too long");
    }
    return call.interpreter.NewArrayValue(std::move(out), std::move(present));
  });

  method("flat", [](NativeCall& call) {
    const Value depth_value = Argument(call.arguments, 0);
    const double depth = depth_value.IsUndefined() ? 1.0 : ToNumber(depth_value);
    std::vector<Value> out;
    // Recursion bounded by the requested depth, which is bounded below by the
    // element count -- a page cannot ask for unbounded C++ recursion here.
    const auto flatten = [&out](auto&& self_ref, const Object& array, double left) -> void {
      for (std::size_t i = 0; i < array.ElementCount(); ++i) {
        if (!array.HasElement(i)) {
          continue;  // flat drops holes, unlike slice
        }
        const Value element = array.GetElement(i);
        if (left >= 1.0 && element.IsObject() &&
            element.object->GetKind() == Object::Kind::Array) {
          self_ref(self_ref, *element.object, left - 1.0);
          continue;
        }
        out.push_back(element);
      }
    };
    if (Object* self = Self(call)) {
      flatten(flatten, *self, depth);
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });

  // --- Iterating ------------------------------------------------------------

  method("forEach", [](NativeCall& call) {
    Object* self = Self(call);
    const Value callback = Argument(call.arguments, 0);
    if (!IsCallable(callback)) {
      return call.Throw("TypeError", "forEach requires a function");
    }
    const Value receiver = Argument(call.arguments, 1);
    // The length is re-read each step, so a callback that shortens the array
    // is not visiting freed elements.
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (!self->HasElement(i)) {
        continue;  // forEach skips holes
      }
      Value ignored;
      if (!Visit(call, callback, receiver, i, ignored)) {
        return Value::Undefined();
      }
    }
    return Value::Undefined();
  });

  method("map", [](NativeCall& call) {
    Object* self = Self(call);
    const Value callback = Argument(call.arguments, 0);
    if (!IsCallable(callback)) {
      return call.Throw("TypeError", "map requires a function");
    }
    const Value receiver = Argument(call.arguments, 1);
    std::vector<Value> out;
    std::vector<bool> present;
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (!self->HasElement(i)) {
        out.push_back(Value::Undefined());
        present.push_back(false);  // map preserves holes rather than calling on them
        continue;
      }
      Value mapped;
      if (!Visit(call, callback, receiver, i, mapped)) {
        return Value::Undefined();
      }
      out.push_back(mapped);
      present.push_back(true);
    }
    return call.interpreter.NewArrayValue(std::move(out), std::move(present));
  });

  method("flatMap", [](NativeCall& call) {
    Object* self = Self(call);
    const Value callback = Argument(call.arguments, 0);
    if (!IsCallable(callback)) {
      return call.Throw("TypeError", "flatMap requires a function");
    }
    const Value receiver = Argument(call.arguments, 1);
    std::vector<Value> out;
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (!self->HasElement(i)) {
        continue;
      }
      Value mapped;
      if (!Visit(call, callback, receiver, i, mapped)) {
        return Value::Undefined();
      }
      if (mapped.IsObject() && mapped.object->GetKind() == Object::Kind::Array) {
        for (std::size_t j = 0; j < mapped.object->ElementCount(); ++j) {
          out.push_back(mapped.object->GetElement(j));
        }
        continue;
      }
      out.push_back(mapped);
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });

  method("filter", [](NativeCall& call) {
    Object* self = Self(call);
    const Value callback = Argument(call.arguments, 0);
    if (!IsCallable(callback)) {
      return call.Throw("TypeError", "filter requires a function");
    }
    const Value receiver = Argument(call.arguments, 1);
    std::vector<Value> out;
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      if (!self->HasElement(i)) {
        continue;
      }
      const Value element = self->GetElement(i);
      Value kept;
      if (!Visit(call, callback, receiver, i, kept)) {
        return Value::Undefined();
      }
      if (ToBoolean(kept)) {
        out.push_back(element);
      }
    }
    return call.interpreter.NewArrayValue(std::move(out));
  });

  // find, findIndex, findLast, findLastIndex, some and every are one walk with
  // two questions: which way it runs, and what it answers with.
  enum class Search { First, Last, Some, Every };
  const auto searcher = [](Search kind, bool want_index) {
    return [kind, want_index](NativeCall& call) {
      Object* self = Self(call);
      const Value callback = Argument(call.arguments, 0);
      if (!IsCallable(callback)) {
        return call.Throw("TypeError", "a predicate is required");
      }
      const Value receiver = Argument(call.arguments, 1);
      const std::size_t size = LengthOf(call);
      for (std::size_t step = 0; self != nullptr && step < size; ++step) {
        const std::size_t i = kind == Search::Last ? size - 1 - step : step;
        if (i >= self->ElementCount()) {
          continue;
        }
        // find and its relatives visit holes as undefined; some and every skip
        // them.
        if (!self->HasElement(i) && (kind == Search::Some || kind == Search::Every)) {
          continue;
        }
        Value answer;
        if (!Visit(call, callback, receiver, i, answer)) {
          return Value::Undefined();
        }
        const bool matched = ToBoolean(answer);
        if (kind == Search::Every) {
          if (!matched) {
            return Value::Bool(false);
          }
          continue;
        }
        if (!matched) {
          continue;
        }
        if (kind == Search::Some) {
          return Value::Bool(true);
        }
        return want_index ? Value::Number(static_cast<double>(i)) : self->GetElement(i);
      }
      switch (kind) {
        case Search::Some:
          return Value::Bool(false);
        case Search::Every:
          // Vacuously true on an empty array, which is the answer that
          // surprises people and the one the spec gives.
          return Value::Bool(true);
        case Search::First:
        case Search::Last:
          break;
      }
      return want_index ? Value::Number(-1) : Value::Undefined();
    };
  };
  method("find", searcher(Search::First, false));
  method("findIndex", searcher(Search::First, true));
  method("findLast", searcher(Search::Last, false));
  method("findLastIndex", searcher(Search::Last, true));
  method("some", searcher(Search::Some, false));
  method("every", searcher(Search::Every, false));

  const auto reducer = [](bool from_end) {
    return [from_end](NativeCall& call) {
      Object* self = Self(call);
      const Value callback = Argument(call.arguments, 0);
      if (!IsCallable(callback)) {
        return call.Throw("TypeError", "reduce requires a function");
      }
      const std::size_t size = LengthOf(call);
      std::size_t step = 0;
      Value accumulator;
      if (call.arguments.size() >= 2) {
        accumulator = call.arguments[1];
      } else {
        // No seed: the first present element is the seed. An empty array with
        // no seed is a TypeError rather than undefined, which is what catches
        // `[].reduce((a, b) => a + b)`.
        while (step < size && !self->HasElement(from_end ? size - 1 - step : step)) {
          ++step;
        }
        if (step >= size) {
          return call.Throw("TypeError", "reduce of an empty array with no initial value");
        }
        accumulator = self->GetElement(from_end ? size - 1 - step : step);
        ++step;
      }
      for (; step < size; ++step) {
        const std::size_t i = from_end ? size - 1 - step : step;
        if (i >= self->ElementCount() || !self->HasElement(i)) {
          continue;
        }
        const Result next = call.interpreter.CallFunction(
            callback, Value::Undefined(),
            {accumulator, self->GetElement(i), Value::Number(static_cast<double>(i)), call.self});
        if (next.IsAbrupt()) {
          return call.ThrowValue(next.value);
        }
        accumulator = next.value;
      }
      return accumulator;
    };
  };
  method("reduce", reducer(false));
  method("reduceRight", reducer(true));

  // --- Rearranging ----------------------------------------------------------

  method("reverse", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return call.self;
    }
    std::vector<Value> out;
    std::vector<bool> present;
    for (std::size_t i = self->ElementCount(); i-- > 0;) {
      out.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    self->SetElements(std::move(out), std::move(present));
    return call.self;  // in place, and returns the same array
  });

  method("sort", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return call.self;
    }
    const Value comparator = Argument(call.arguments, 0);
    if (!comparator.IsUndefined() && !IsCallable(comparator)) {
      return call.Throw("TypeError", "the comparator is not a function");
    }
    // Holes sort to the very end, after the undefineds, and are not passed to
    // the comparator either.
    std::vector<Value> items;
    std::size_t holes = 0;
    for (std::size_t i = 0; i < self->ElementCount(); ++i) {
      if (self->HasElement(i)) {
        items.push_back(self->GetElement(i));
      } else {
        ++holes;
      }
    }
    if (!MergeSort(call, items, comparator)) {
      return Value::Undefined();  // the comparator threw; `call` carries it
    }
    std::vector<bool> present(items.size(), true);
    items.resize(items.size() + holes);
    present.resize(items.size(), false);
    self->SetElements(std::move(items), std::move(present));
    return call.self;
  });

  method("fill", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return call.self;
    }
    const std::size_t size = self->ElementCount();
    const Value filler = Argument(call.arguments, 0);
    const std::size_t begin = Clamp(ToNumber(Argument(call.arguments, 1)), size);
    const std::size_t end = call.arguments.size() < 3 || call.arguments[2].IsUndefined()
                                ? size
                                : Clamp(ToNumber(call.arguments[2]), size);
    for (std::size_t i = begin; i < end; ++i) {
      self->SetElement(i, filler);
    }
    return call.self;
  });

  method("copyWithin", [](NativeCall& call) {
    Object* self = Self(call);
    if (self == nullptr) {
      return call.self;
    }
    const std::size_t size = self->ElementCount();
    const std::size_t target = Clamp(ToNumber(Argument(call.arguments, 0)), size);
    const std::size_t begin = Clamp(ToNumber(Argument(call.arguments, 1)), size);
    const std::size_t end = call.arguments.size() < 3 || call.arguments[2].IsUndefined()
                                ? size
                                : Clamp(ToNumber(call.arguments[2]), size);
    // Copied out first: the source and the destination can overlap, and
    // writing in place would read a value this call already replaced.
    std::vector<Value> slice;
    std::vector<bool> present;
    for (std::size_t i = begin; i < end; ++i) {
      slice.push_back(self->GetElement(i));
      present.push_back(self->HasElement(i));
    }
    for (std::size_t i = 0; i < slice.size() && target + i < size; ++i) {
      if (present[i]) {
        self->SetElement(target + i, slice[i]);
      }
    }
    return call.self;
  });

  // --- The copying forms ----------------------------------------------------
  //
  // Four methods that answer what `sort`, `reverse`, `splice` and an indexed
  // write would have answered, without changing the receiver. The point of
  // them is exactly that: a page can sort a list it does not own.

  const auto elements_of = [](const NativeCall& call) {
    std::vector<Value> out;
    const Object* self = Self(call);
    for (std::size_t i = 0; self != nullptr && i < self->ElementCount(); ++i) {
      out.push_back(self->GetElement(i));
    }
    return out;
  };
  method("toReversed", [elements_of](NativeCall& call) {
    std::vector<Value> out = elements_of(call);
    std::reverse(out.begin(), out.end());
    return call.interpreter.NewArrayValue(std::move(out));
  });
  method("with", [elements_of](NativeCall& call) {
    std::vector<Value> out = elements_of(call);
    const double index = ToNumber(Argument(call.arguments, 0));
    const double at = index < 0 ? index + static_cast<double>(out.size()) : index;
    if (at < 0 || at >= static_cast<double>(out.size())) {
      return call.Throw("RangeError", "index is out of range");
    }
    out[static_cast<std::size_t>(at)] = Argument(call.arguments, 1);
    return call.interpreter.NewArrayValue(std::move(out));
  });
  method("toSpliced", [elements_of](NativeCall& call) {
    std::vector<Value> out = elements_of(call);
    const std::size_t start = Clamp(ToNumber(Argument(call.arguments, 0)), out.size());
    const std::size_t removed =
        call.arguments.size() < 2
            ? out.size() - start
            : std::min(out.size() - start,
                       Clamp(std::max(0.0, ToNumber(call.arguments[1])), out.size()));
    std::vector<Value> inserted(call.arguments.begin() +
                                    static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                        2, call.arguments.size())),
                                call.arguments.end());
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(start),
              out.begin() + static_cast<std::ptrdiff_t>(start + removed));
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(start), inserted.begin(),
               inserted.end());
    return call.interpreter.NewArrayValue(std::move(out));
  });
  method("toSorted", [elements_of](NativeCall& call) {
    // Through `sort` on a copy rather than a second sort: the comparator
    // contract -- and the stability the sort promises -- should have one
    // implementation.
    const Value copy = call.interpreter.NewArrayValue(elements_of(call));
    const Value sort = call.interpreter.GetPropertyValue(copy, "sort");
    const Result sorted = call.interpreter.CallFunction(sort, copy, call.arguments);
    return sorted.IsAbrupt() ? call.ThrowValue(sorted.value) : copy;
  });

  // --- The constructor ------------------------------------------------------

  Object* constructor = NewNative("Array", [](NativeCall& call) {
    // `class Stack extends Array` reaches here with the instance already
    // allocated -- as an array, because Construct read the root's mark -- and
    // wants it filled in rather than replaced.
    if (Object* target = ConstructionTarget(call)) {
      if (call.arguments.size() == 1 && call.arguments[0].IsNumber()) {
        const double length = call.arguments[0].number;
        if (length < 0 || length != std::floor(length) ||
            length > static_cast<double>(kMaxAllocationLength)) {
          return call.Throw("RangeError", "invalid array length");
        }
        target->SetElements(std::vector<Value>(static_cast<std::size_t>(length)),
                            std::vector<bool>(static_cast<std::size_t>(length), false));
      } else if (!call.arguments.empty()) {
        target->SetElements(call.arguments, {});
      }
      return Value::Obj(target);
    }
    // `Array(5)` is five holes; `Array(1, 2)` is two elements. One argument
    // means length, which is the language's oldest wart.
    if (call.arguments.size() == 1 && call.arguments[0].IsNumber()) {
      const double length = call.arguments[0].number;
      if (length < 0 || length != std::floor(length) ||
          length > static_cast<double>(kMaxAllocationLength)) {
        return call.Throw("RangeError", "invalid array length");
      }
      const auto size = static_cast<std::size_t>(length);
      return call.interpreter.NewArrayValue(std::vector<Value>(size, Value::Undefined()),
                                            std::vector<bool>(size, false));
    }
    return call.interpreter.NewArrayValue(call.arguments);
  });
  if (constructor == nullptr) {
    return;
  }
  constructor->Set("prototype", Value::Obj(prototype));
  prototype->Set("constructor", Value::Obj(constructor));
  // What `new` on this produces, so that Construct allocates an array for a
  // subclass rather than a plain object with array methods on it -- which is
  // an object whose `length` is undefined and whose `push` writes nowhere.
  MarksConstructedKind(constructor, Object::Kind::Array);
  global_scope_->Declare("Array", Value::Obj(constructor), false);

  InstallNative(constructor, "isArray", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    // Through the proxy, deliberately: a proxy over an array is an array to
    // the language, and a page's feature test must not be what reveals a
    // wrapper.
    return Value::Bool(value.IsObject() &&
                       value.object->TargetKind() == Object::Kind::Array);
  });
  InstallNative(constructor, "of", [](NativeCall& call) {
    return call.interpreter.NewArrayValue(call.arguments);
  });
  InstallNative(constructor, "from", [](NativeCall& call) {
    const Value source = Argument(call.arguments, 0);
    std::vector<Value> items;
    // Anything iterable, which is most of what a page passes; an array-like
    // with a `length` is the other half and is handled after.
    const Result collected = call.interpreter.CollectIterable(source, items);
    if (collected.IsAbrupt()) {
      if (!source.IsObject()) {
        return call.ThrowValue(collected.value);
      }
      items.clear();
      const double length = ToNumber(call.interpreter.GetPropertyValue(source, "length"));
      if (std::isnan(length) || length < 0) {
        return call.interpreter.NewArrayValue({});
      }
      const auto size = static_cast<std::size_t>(
          std::min(length, static_cast<double>(kMaxAllocationLength)));
      for (std::size_t i = 0; i < size; ++i) {
        items.push_back(call.interpreter.GetPropertyValue(source, std::to_string(i)));
      }
    }
    const Value mapper = Argument(call.arguments, 1);
    if (!IsCallable(mapper)) {
      return call.interpreter.NewArrayValue(std::move(items));
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
      const Result mapped = call.interpreter.CallFunction(
          mapper, Value::Undefined(), {items[i], Value::Number(static_cast<double>(i))});
      if (mapped.IsAbrupt()) {
        return call.ThrowValue(mapped.value);
      }
      items[i] = mapped.value;
    }
    return call.interpreter.NewArrayValue(std::move(items));
  });
}

}  // namespace microbrowser::js
