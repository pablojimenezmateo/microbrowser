#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"

// `ArrayBuffer`, the typed arrays, and `DataView`.
//
// The three are one feature and are installed together: a buffer is bytes, a
// typed array is a window onto them with a element type, and a DataView is a
// window with no type that is told one per access. What makes them work here
// is in Heap.h -- `Object::ElementCount`, `GetElement` and `SetElement` consult
// a BufferView when there is one -- which means every generic Array.prototype
// method already works on a typed array without knowing it exists. Those
// methods are specified over array-likes; this is what makes one.
//
// **Every byte in here came from somewhere.** A buffer is what a page fills
// from a fetch, a canvas or a file, so every length is checked against the
// buffer's actual size at the point of use rather than trusted from when the
// view was made -- and the two can differ, because a buffer can be detached
// while a view still points at it.
//
// No BigInt64Array or BigUint64Array: there is no BigInt in this engine, and a
// typed array whose elements silently lost precision would be worse than one
// that does not exist.

namespace microbrowser::js {

namespace {

struct TypedKind {
  const char* name;
  ElementKind kind;
};

constexpr TypedKind kTypedArrays[] = {
    {"Int8Array", ElementKind::Int8},
    {"Uint8Array", ElementKind::Uint8},
    {"Uint8ClampedArray", ElementKind::Uint8Clamped},
    {"Int16Array", ElementKind::Int16},
    {"Uint16Array", ElementKind::Uint16},
    {"Int32Array", ElementKind::Int32},
    {"Uint32Array", ElementKind::Uint32},
    {"Float32Array", ElementKind::Float32},
    {"Float64Array", ElementKind::Float64},
};

// The generic Array.prototype methods that work unchanged on a typed array,
// because they read through ElementCount and GetElement and write through
// SetElement.
//
// Everything that changes the *length* is deliberately absent: push, pop,
// shift, unshift, splice and concat have no meaning on a fixed-length view,
// and inheriting them would give a page methods that silently did nothing.
constexpr const char* kSharedArrayMethods[] = {
    "at",         "every",      "fill",    "filter",     "find",        "findIndex",
    "findLast",   "findLastIndex", "forEach", "includes", "indexOf",    "join",
    "lastIndexOf", "map",       "reduce",  "reduceRight", "reverse",    "slice",
    "some",       "sort",       "toString", "entries",   "keys",        "values",
    "copyWithin", "toReversed", "toSorted", "with",
};

const BufferView* ViewOf(const Value& value) {
  return value.IsObject() ? value.object->View() : nullptr;
}

// A length or an offset a page supplied, checked. Anything that is not a
// non-negative integer within the cap is refused rather than clamped: a page
// asking for a four-gigabyte buffer has made a mistake, and a silently smaller
// one is a mistake it will not find.
bool ReadSize(NativeCall& call, const Value& value, std::size_t& out, bool& missing) {
  missing = value.IsUndefined();
  if (missing) {
    out = 0;
    return true;
  }
  const double number = ToNumber(value);
  if (!std::isfinite(number) || number < 0 ||
      number > static_cast<double>(kMaxAllocationLength)) {
    call.Throw("RangeError", "invalid length");
    return false;
  }
  out = static_cast<std::size_t>(std::trunc(number));
  return true;
}

}  // namespace

void Interpreter::InstallTypedArrays() {
  // --- ArrayBuffer ----------------------------------------------------------

  Object* buffer_prototype = NewObject();
  Object* buffer_constructor = NewNative("ArrayBuffer", [](NativeCall& call) {
    std::size_t length = 0;
    bool missing = false;
    if (!ReadSize(call, Argument(call.arguments, 0), length, missing)) {
      return Value::Undefined();
    }
    Object* buffer = ConstructionTarget(call);
    if (buffer == nullptr) {
      buffer = call.interpreter.GetHeap().AllocateObject(Object::Kind::ArrayBuffer);
      if (buffer == nullptr) {
        return call.Throw("RangeError", "out of memory");
      }
      const Value* prototype = call.callee == nullptr ? nullptr
                                                      : call.callee->GetOwn("prototype");
      if (prototype != nullptr && prototype->IsObject()) {
        buffer->SetPrototype(prototype->object);
      }
    }
    BufferView view;
    // Zero-filled, which the language guarantees and which is also the only
    // safe answer: handing a page uninitialised heap is handing it whatever
    // was there before.
    view.bytes = std::make_shared<std::vector<std::uint8_t>>(length, 0);
    view.length = length;
    view.kind = ElementKind::Uint8;
    buffer->MakeView(std::move(view));
    return Value::Obj(buffer);
  });
  if (buffer_prototype == nullptr || buffer_constructor == nullptr) {
    return;
  }
  buffer_prototype->SetPrototype(intrinsics().object_prototype);
  buffer_constructor->Set("prototype", Value::Obj(buffer_prototype));
  buffer_prototype->SetHidden("constructor", Value::Obj(buffer_constructor));
  MarksConstructedKind(buffer_constructor, Object::Kind::ArrayBuffer);
  realm_->global_scope->Declare("ArrayBuffer", Value::Obj(buffer_constructor), false);
  intrinsics().array_buffer_prototype = buffer_prototype;

  if (Object* byte_length = NewNative("byteLength", [](NativeCall& call) {
        const BufferView* view = ViewOf(call.self);
        return Value::Number(view == nullptr || view->bytes == nullptr
                                 ? 0.0
                                 : static_cast<double>(view->bytes->size()));
      })) {
    buffer_prototype->DefineAccessor("byteLength", byte_length, nullptr);
  }
  InstallNative(buffer_prototype, "slice", [](NativeCall& call) {
    const BufferView* view = ViewOf(call.self);
    if (view == nullptr || view->bytes == nullptr) {
      return call.Throw("TypeError", "not an ArrayBuffer");
    }
    const double size = static_cast<double>(view->bytes->size());
    const auto clamp = [size](double index) {
      const double at = std::isnan(index) ? 0.0 : std::trunc(index);
      return static_cast<std::size_t>(at < 0 ? std::max(size + at, 0.0) : std::min(at, size));
    };
    const std::size_t begin = clamp(ToNumber(Argument(call.arguments, 0)));
    const Value end_value = Argument(call.arguments, 1);
    const std::size_t end = end_value.IsUndefined() ? view->bytes->size() : clamp(ToNumber(end_value));
    Object* copy = call.interpreter.GetHeap().AllocateObject(Object::Kind::ArrayBuffer);
    if (copy == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    copy->SetPrototype(call.self.object->Prototype());
    BufferView made;
    made.bytes = std::make_shared<std::vector<std::uint8_t>>(
        view->bytes->begin() + static_cast<std::ptrdiff_t>(begin),
        view->bytes->begin() + static_cast<std::ptrdiff_t>(std::max(begin, end)));
    made.length = made.bytes->size();
    copy->MakeView(std::move(made));
    return Value::Obj(copy);
  });
  InstallNative(buffer_constructor, "isView", [](NativeCall& call) {
    const Value value = Argument(call.arguments, 0);
    // A view rather than a buffer: the difference is whether it looks at
    // somebody else's bytes.
    return Value::Bool(value.IsObject() && value.object->View() != nullptr &&
                       value.object->View()->buffer != nullptr);
  });

  // --- %TypedArray%.prototype ----------------------------------------------
  //
  // One prototype shared by all nine, which is what the spec has: the element
  // type is per-object, so nothing on it needs to know which type it is on.
  Object* typed_prototype = NewObject();
  if (typed_prototype == nullptr) {
    return;
  }
  typed_prototype->SetPrototype(intrinsics().object_prototype);
  intrinsics().typed_array_prototype = typed_prototype;

  // The generic methods, taken from Array.prototype rather than written again.
  // They are the same functions: each reads through ElementCount and
  // GetElement, and a typed array answers both.
  for (const char* name : kSharedArrayMethods) {
    if (const Value* method = intrinsics().array_prototype->GetOwn(name)) {
      typed_prototype->Set(name, *method);
    }
  }
  if (shared_.symbol_iterator != nullptr) {
    if (const Object::Property* iterator = intrinsics().array_prototype->GetOwnProperty(
            PropertyKey::Symbol(shared_.symbol_iterator))) {
      typed_prototype->Set(PropertyKey::Symbol(shared_.symbol_iterator), iterator->value);
    }
  }

  const auto accessor = [this, typed_prototype](const char* name,
                                                double (*read)(const BufferView&)) {
    if (Object* getter = NewNative(name, [read](NativeCall& call) {
          const BufferView* view = ViewOf(call.self);
          return Value::Number(view == nullptr ? 0.0 : read(*view));
        })) {
      typed_prototype->DefineAccessor(name, getter, nullptr);
    }
  };
  accessor("length", [](const BufferView& view) { return static_cast<double>(view.length); });
  accessor("byteOffset", [](const BufferView& view) { return static_cast<double>(view.offset); });
  accessor("byteLength", [](const BufferView& view) {
    return static_cast<double>(view.length * ElementSize(view.kind));
  });
  if (Object* buffer_of = NewNative("buffer", [](NativeCall& call) {
        const BufferView* view = ViewOf(call.self);
        return view == nullptr || view->buffer == nullptr ? Value::Undefined()
                                                          : Value::Obj(view->buffer);
      })) {
    typed_prototype->DefineAccessor("buffer", buffer_of, nullptr);
  }

  InstallNative(typed_prototype, "set", [](NativeCall& call) {
    const BufferView* view = ViewOf(call.self);
    if (view == nullptr) {
      return call.Throw("TypeError", "not a typed array");
    }
    const Value source = Argument(call.arguments, 0);
    const double offset = ToNumber(Argument(call.arguments, 1));
    const std::size_t at = std::isfinite(offset) && offset > 0
                               ? static_cast<std::size_t>(std::trunc(offset))
                               : 0;
    std::vector<Value> items;
    if (source.IsObject() && (source.object->GetKind() == Object::Kind::Array ||
                              source.object->View() != nullptr)) {
      // Read out first: the source can be a view over the *same* buffer, and
      // writing while reading would copy values this call already replaced.
      for (std::size_t i = 0; i < source.object->ElementCount(); ++i) {
        items.push_back(source.object->GetElement(i));
      }
    } else {
      const Result collected = call.interpreter.CollectIterable(source, items);
      if (collected.IsAbrupt()) {
        return call.ThrowValue(collected.value);
      }
    }
    if (at + items.size() > view->length) {
      return call.Throw("RangeError", "source is too long for this typed array");
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
      call.self.object->SetElement(at + i, items[i]);
    }
    return Value::Undefined();
  });

  InstallNative(typed_prototype, "subarray", [](NativeCall& call) {
    // A *view* over the same bytes, not a copy -- which is the whole
    // difference between this and `slice`, and the reason a page uses it.
    const BufferView* view = ViewOf(call.self);
    if (view == nullptr || view->bytes == nullptr) {
      return call.Throw("TypeError", "not a typed array");
    }
    const double size = static_cast<double>(view->length);
    const auto clamp = [size](double index) {
      const double value = std::isnan(index) ? 0.0 : std::trunc(index);
      return static_cast<std::size_t>(value < 0 ? std::max(size + value, 0.0)
                                                : std::min(value, size));
    };
    const std::size_t begin = clamp(ToNumber(Argument(call.arguments, 0)));
    const Value end_value = Argument(call.arguments, 1);
    const std::size_t end = end_value.IsUndefined() ? view->length : clamp(ToNumber(end_value));
    Object* made = call.interpreter.GetHeap().AllocateObject(Object::Kind::TypedArray);
    if (made == nullptr) {
      return call.Throw("RangeError", "out of memory");
    }
    made->SetPrototype(call.self.object->Prototype());
    BufferView window;
    window.bytes = view->bytes;
    window.buffer = view->buffer;
    window.offset = view->offset + begin * ElementSize(view->kind);
    window.length = end > begin ? end - begin : 0;
    window.kind = view->kind;
    made->MakeView(std::move(window));
    return Value::Obj(made);
  });

  // --- The nine constructors ------------------------------------------------

  for (const TypedKind& typed : kTypedArrays) {
    Object* prototype = NewObject();
    if (prototype == nullptr) {
      continue;
    }
    prototype->SetPrototype(typed_prototype);
    const ElementKind kind = typed.kind;
    Object* constructor = NewNative(typed.name, [kind](NativeCall& call) {
      Object* array = ConstructionTarget(call);
      if (array == nullptr) {
        array = call.interpreter.GetHeap().AllocateObject(Object::Kind::TypedArray);
        if (array == nullptr) {
          return call.Throw("RangeError", "out of memory");
        }
        const Value* prototype_value =
            call.callee == nullptr ? nullptr : call.callee->GetOwn("prototype");
        if (prototype_value != nullptr && prototype_value->IsObject()) {
          array->SetPrototype(prototype_value->object);
        }
      }
      const std::size_t width = ElementSize(kind);
      const Value first = Argument(call.arguments, 0);
      BufferView view;
      view.kind = kind;

      // Over an existing buffer: the view is a window and the bytes stay
      // shared, which is how two typed arrays see each other's writes.
      if (first.IsObject() && first.object->GetKind() == Object::Kind::ArrayBuffer) {
        const BufferView* backing = first.object->View();
        if (backing == nullptr || backing->bytes == nullptr) {
          return call.Throw("TypeError", "the buffer has been detached");
        }
        std::size_t offset = 0;
        bool missing = false;
        if (!ReadSize(call, Argument(call.arguments, 1), offset, missing)) {
          return Value::Undefined();
        }
        if (offset % width != 0) {
          return call.Throw("RangeError", "the offset must be a multiple of the element size");
        }
        if (offset > backing->bytes->size()) {
          return call.Throw("RangeError", "the offset is past the end of the buffer");
        }
        std::size_t length = 0;
        bool length_missing = false;
        if (!ReadSize(call, Argument(call.arguments, 2), length, length_missing)) {
          return Value::Undefined();
        }
        if (length_missing) {
          // The rest of the buffer, which has to divide evenly.
          const std::size_t remaining = backing->bytes->size() - offset;
          if (remaining % width != 0) {
            return call.Throw("RangeError",
                              "the buffer length is not a multiple of the element size");
          }
          length = remaining / width;
        } else if (offset + length * width > backing->bytes->size()) {
          return call.Throw("RangeError", "the view runs past the end of the buffer");
        }
        view.bytes = backing->bytes;
        view.buffer = first.object;
        view.offset = offset;
        view.length = length;
        array->MakeView(std::move(view));
        return Value::Obj(array);
      }

      // Everything else allocates its own buffer: a length, an array-like, or
      // any iterable.
      std::vector<Value> items;
      std::size_t length = 0;
      if (first.IsUndefined()) {
        length = 0;
      } else if (first.IsNumber()) {
        bool missing = false;
        if (!ReadSize(call, first, length, missing)) {
          return Value::Undefined();
        }
      } else if (first.IsObject()) {
        if (first.object->GetKind() == Object::Kind::Array ||
            first.object->View() != nullptr) {
          for (std::size_t i = 0; i < first.object->ElementCount(); ++i) {
            items.push_back(first.object->GetElement(i));
          }
        } else {
          const Result collected = call.interpreter.CollectIterable(first, items);
          if (collected.IsAbrupt()) {
            return call.ThrowValue(collected.value);
          }
        }
        length = items.size();
      }

      Object* buffer = call.interpreter.GetHeap().AllocateObject(Object::Kind::ArrayBuffer);
      if (buffer == nullptr) {
        return call.Throw("RangeError", "out of memory");
      }
      buffer->SetPrototype(call.interpreter.ArrayBufferPrototype());
      BufferView backing;
      backing.bytes = std::make_shared<std::vector<std::uint8_t>>(length * width, 0);
      backing.length = length * width;
      buffer->MakeView(std::move(backing));

      view.bytes = buffer->View()->bytes;
      view.buffer = buffer;
      view.length = length;
      array->MakeView(std::move(view));
      for (std::size_t i = 0; i < items.size(); ++i) {
        array->SetElement(i, items[i]);
      }
      return Value::Obj(array);
    });
    if (constructor == nullptr) {
      continue;
    }
    constructor->Set("prototype", Value::Obj(prototype));
    prototype->SetHidden("constructor", Value::Obj(constructor));
    const auto width = Value::Number(static_cast<double>(ElementSize(kind)));
    constructor->Set("BYTES_PER_ELEMENT", width);
    prototype->Set("BYTES_PER_ELEMENT", width);
    MarksConstructedKind(constructor, Object::Kind::TypedArray);
    // `from` and `of`, which build through this constructor rather than
    // through Array -- so `Uint8Array.from([1,2])` is a Uint8Array.
    InstallNative(constructor, "from", [](NativeCall& call) {
      std::vector<Value> items;
      const Value source = Argument(call.arguments, 0);
      if (source.IsObject() && (source.object->GetKind() == Object::Kind::Array ||
                                source.object->View() != nullptr)) {
        for (std::size_t i = 0; i < source.object->ElementCount(); ++i) {
          items.push_back(source.object->GetElement(i));
        }
      } else if (source.IsObject() && source.object->HasOwn("length")) {
        const double count = ToNumber(call.interpreter.GetPropertyValue(source, "length"));
        for (double i = 0; i < count && i < static_cast<double>(kMaxAllocationLength); ++i) {
          items.push_back(
              call.interpreter.GetPropertyValue(source, NumberToString(i)));
        }
      } else {
        const Result collected = call.interpreter.CollectIterable(source, items);
        if (collected.IsAbrupt()) {
          return call.ThrowValue(collected.value);
        }
      }
      const Value mapper = Argument(call.arguments, 1);
      if (mapper.IsObject() && mapper.object->IsCallable()) {
        for (std::size_t i = 0; i < items.size(); ++i) {
          const Result mapped = call.interpreter.CallFunction(
              mapper, Value::Undefined(),
              {items[i], Value::Number(static_cast<double>(i))});
          if (mapped.IsAbrupt()) {
            return call.ThrowValue(mapped.value);
          }
          items[i] = mapped.value;
        }
      }
      // Through the constructor this was *read off* -- `call.self` -- rather
      // than through `call.callee`, which is the `from` function itself.
      // Constructing through that is an unbounded recursion, and it is the
      // kind that only shows up when the method is actually called.
      const Value list = call.interpreter.NewArrayValue(std::move(items));
      const Result made = call.interpreter.ConstructValue(call.self, {list});
      return made.IsAbrupt() ? call.ThrowValue(made.value) : made.value;
    });
    InstallNative(constructor, "of", [](NativeCall& call) {
      const Value list = call.interpreter.NewArrayValue(call.arguments);
      const Result made = call.interpreter.ConstructValue(call.self, {list});
      return made.IsAbrupt() ? call.ThrowValue(made.value) : made.value;
    });
    realm_->global_scope->Declare(typed.name, Value::Obj(constructor), false);
  }

  // --- DataView -------------------------------------------------------------
  //
  // The same window with the type moved from the object to the call, and with
  // the byte order a page's to choose -- which is the whole reason it exists:
  // a network format says which order it is in and a typed array does not ask.

  Object* view_prototype = NewObject();
  Object* view_constructor = NewNative("DataView", [](NativeCall& call) {
    const Value first = Argument(call.arguments, 0);
    if (!first.IsObject() || first.object->GetKind() != Object::Kind::ArrayBuffer) {
      return call.Throw("TypeError", "DataView requires an ArrayBuffer");
    }
    const BufferView* backing = first.object->View();
    if (backing == nullptr || backing->bytes == nullptr) {
      return call.Throw("TypeError", "the buffer has been detached");
    }
    std::size_t offset = 0;
    bool missing = false;
    if (!ReadSize(call, Argument(call.arguments, 1), offset, missing)) {
      return Value::Undefined();
    }
    if (offset > backing->bytes->size()) {
      return call.Throw("RangeError", "the offset is past the end of the buffer");
    }
    std::size_t length = 0;
    bool length_missing = false;
    if (!ReadSize(call, Argument(call.arguments, 2), length, length_missing)) {
      return Value::Undefined();
    }
    if (length_missing) {
      length = backing->bytes->size() - offset;
    } else if (offset + length > backing->bytes->size()) {
      return call.Throw("RangeError", "the view runs past the end of the buffer");
    }
    Object* made = ConstructionTarget(call);
    if (made == nullptr) {
      made = call.interpreter.GetHeap().AllocateObject(Object::Kind::DataView);
      if (made == nullptr) {
        return call.Throw("RangeError", "out of memory");
      }
      const Value* prototype = call.callee == nullptr ? nullptr
                                                      : call.callee->GetOwn("prototype");
      if (prototype != nullptr && prototype->IsObject()) {
        made->SetPrototype(prototype->object);
      }
    }
    BufferView window;
    window.bytes = backing->bytes;
    window.buffer = first.object;
    window.offset = offset;
    // In bytes, unlike a typed array's -- a DataView has no element type to
    // divide by, which is the whole of what makes it different.
    window.length = length;
    window.kind = ElementKind::Uint8;
    made->MakeView(std::move(window));
    return Value::Obj(made);
  });
  if (view_prototype == nullptr || view_constructor == nullptr) {
    return;
  }
  view_prototype->SetPrototype(intrinsics().object_prototype);
  view_constructor->Set("prototype", Value::Obj(view_prototype));
  view_prototype->SetHidden("constructor", Value::Obj(view_constructor));
  MarksConstructedKind(view_constructor, Object::Kind::DataView);
  realm_->global_scope->Declare("DataView", Value::Obj(view_constructor), false);

  for (const char* name : {"byteLength", "byteOffset"}) {
    const bool is_length = std::string_view(name) == "byteLength";
    if (Object* getter = NewNative(name, [is_length](NativeCall& call) {
          const BufferView* view = ViewOf(call.self);
          if (view == nullptr) {
            return Value::Number(0.0);
          }
          return Value::Number(static_cast<double>(is_length ? view->length : view->offset));
        })) {
      view_prototype->DefineAccessor(name, getter, nullptr);
    }
  }
  if (Object* buffer_of = NewNative("buffer", [](NativeCall& call) {
        const BufferView* view = ViewOf(call.self);
        return view == nullptr || view->buffer == nullptr ? Value::Undefined()
                                                          : Value::Obj(view->buffer);
      })) {
    view_prototype->DefineAccessor("buffer", buffer_of, nullptr);
  }

  // The eight get/set pairs. Written once with the type as a parameter, so
  // that the bounds check and the byte-order handling exist in one place
  // rather than in sixteen.
  struct ViewAccess {
    const char* suffix;
    ElementKind kind;
  };
  static constexpr ViewAccess kAccesses[] = {
      {"Int8", ElementKind::Int8},       {"Uint8", ElementKind::Uint8},
      {"Int16", ElementKind::Int16},     {"Uint16", ElementKind::Uint16},
      {"Int32", ElementKind::Int32},     {"Uint32", ElementKind::Uint32},
      {"Float32", ElementKind::Float32}, {"Float64", ElementKind::Float64},
  };
  for (const ViewAccess& access : kAccesses) {
    const ElementKind kind = access.kind;
    const std::string getter_name = std::string("get") + access.suffix;
    const std::string setter_name = std::string("set") + access.suffix;
    InstallNative(view_prototype, getter_name.c_str(), [kind](NativeCall& call) {
      const BufferView* view = ViewOf(call.self);
      if (view == nullptr || view->bytes == nullptr) {
        return call.Throw("TypeError", "not a DataView");
      }
      const double index = ToNumber(Argument(call.arguments, 0));
      const std::size_t width = ElementSize(kind);
      if (!std::isfinite(index) || index < 0 ||
          index + static_cast<double>(width) > static_cast<double>(view->length)) {
        return call.Throw("RangeError", "the access is outside the view");
      }
      const std::size_t at = view->offset + static_cast<std::size_t>(index);
      if (at + width > view->bytes->size()) {
        return call.Throw("RangeError", "the access is outside the buffer");
      }
      // A DataView is big-endian unless told otherwise, which is the opposite
      // of a typed array and is the point: a wire format says which it is.
      const bool little = ToBoolean(Argument(call.arguments, 1));
      std::uint8_t scratch[8] = {};
      for (std::size_t i = 0; i < width; ++i) {
        scratch[i] = (*view->bytes)[at + (little ? i : width - 1 - i)];
      }
      const std::vector<std::uint8_t> ordered(scratch, scratch + width);
      return Value::Number(ReadElementBytes(ordered, 0, kind));
    });
    InstallNative(view_prototype, setter_name.c_str(), [kind](NativeCall& call) {
      const BufferView* view = ViewOf(call.self);
      if (view == nullptr || view->bytes == nullptr) {
        return call.Throw("TypeError", "not a DataView");
      }
      const double index = ToNumber(Argument(call.arguments, 0));
      const std::size_t width = ElementSize(kind);
      if (!std::isfinite(index) || index < 0 ||
          index + static_cast<double>(width) > static_cast<double>(view->length)) {
        return call.Throw("RangeError", "the access is outside the view");
      }
      const std::size_t at = view->offset + static_cast<std::size_t>(index);
      if (at + width > view->bytes->size()) {
        return call.Throw("RangeError", "the access is outside the buffer");
      }
      const bool little = ToBoolean(Argument(call.arguments, 2));
      std::vector<std::uint8_t> ordered(width, 0);
      WriteElementBytes(ordered, 0, kind, ToNumber(Argument(call.arguments, 1)));
      for (std::size_t i = 0; i < width; ++i) {
        (*view->bytes)[at + (little ? i : width - 1 - i)] = ordered[i];
      }
      return Value::Undefined();
    });
  }
}

}  // namespace microbrowser::js
