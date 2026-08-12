#include "js/StructuredClone.h"

#include <cstring>
#include <unordered_map>

#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/RegExp.h"

namespace microbrowser::js {

namespace {

// The tag byte in front of every value. Numbered explicitly and never reordered:
// these bytes are the format, and ADR 0021's storage will one day read a value a
// previous run of the browser wrote.
enum class Tag : std::uint8_t {
  Undefined = 0,
  Null = 1,
  True = 2,
  False = 3,
  Number = 4,
  String = 5,
  BigInt = 6,
  // A value seen before, by index into the order objects were written. This is
  // what preserves a cycle, and what makes a value that appears twice
  // deserialize to one object rather than two.
  Reference = 7,
  Array = 8,
  Object = 9,
  Date = 10,
  RegExp = 11,
  Map = 12,
  Set = 13,
  ArrayBuffer = 14,
  TypedArray = 15,
  DataView = 16,
  // An array hole: `[1, , 3]` has one, and an array that came back with
  // `undefined` in it instead is a different array to `in` and to `forEach`.
  Hole = 17,
};

// The typed array kinds, as a byte. The names are what the constructor is
// called, because deserialization reconstructs through the page's own
// constructor rather than by building a view by hand.
constexpr const char* kTypedArrayNames[] = {
    "Int8Array",  "Uint8Array",   "Uint8ClampedArray", "Int16Array",   "Uint16Array",
    "Int32Array", "Uint32Array",  "Float32Array",      "Float64Array",
};

// How deep a graph either half will walk. Shared, because a stream this build
// wrote has to be one this build can read.
constexpr int kMaxDepth = 128;

// Increments for the length of a scope. A plain `++`/`--` pair around a body
// with eight `return`s in it is eight chances to forget one.
class DepthGuard {
 public:
  explicit DepthGuard(int& depth) : depth_(depth) { ++depth_; }
  ~DepthGuard() { --depth_; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;

 private:
  int& depth_;
};

class Writer {
 public:
  explicit Writer(Interpreter& interpreter) : interpreter_(interpreter) {}

  bool Write(const Value& value) {
    switch (value.type) {
      case ValueType::Undefined:
        return Byte(Tag::Undefined);
      case ValueType::Null:
        return Byte(Tag::Null);
      case ValueType::Boolean:
        return Byte(value.boolean ? Tag::True : Tag::False);
      case ValueType::Number:
        Byte(Tag::Number);
        return Double(value.number);
      case ValueType::String:
        Byte(Tag::String);
        return Text(value.AsString());
      case ValueType::BigInt:
        // As its decimal text, which is exact and is already the equality the
        // type has -- see ValueKey in Collections.h, which keys a Map the same
        // way for the same reason.
        Byte(Tag::BigInt);
        return Text(BigIntText(value));
      case ValueType::Symbol:
        // Not clonable. A symbol's identity is the whole of what it is, and
        // bytes cannot carry an identity.
        return false;
      case ValueType::Object:
        break;
    }
    return WriteObject(value.object);
  }

  std::vector<std::uint8_t> Take() { return std::move(bytes_); }

 private:
  bool Byte(Tag tag) {
    bytes_.push_back(static_cast<std::uint8_t>(tag));
    return true;
  }
  bool RawByte(std::uint8_t byte) {
    bytes_.push_back(byte);
    return true;
  }
  bool Count(std::size_t count) {
    // Fixed 8 bytes, little-endian. Not a varint: the format is read by exactly
    // one program and a length that is always the same width is one fewer thing
    // to get wrong at the boundary.
    for (int i = 0; i < 8; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(count) >> (i * 8)) &
                                                 0xFFU));
    }
    return true;
  }
  bool Double(double number) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &number, sizeof(bits));
    return Count(static_cast<std::size_t>(bits));
  }
  bool Text(const std::string& text) {
    Count(text.size());
    bytes_.insert(bytes_.end(), text.begin(), text.end());
    return true;
  }

  bool WriteObject(Object* object) {
    if (object == nullptr) {
      return Byte(Tag::Null);
    }
    // The same bound the reader has, and for the same reason: a page builds the
    // graph, this walk recurses, and ADR 0009's rule is a refusal rather than a
    // stack overflow. A cycle is *not* what this catches -- `seen_` handles
    // those -- it is a legitimately deep chain, which a `for` loop writes in one
    // line.
    if (depth_ >= kMaxDepth) {
      return false;
    }
    if (const auto seen = seen_.find(object); seen != seen_.end()) {
      Byte(Tag::Reference);
      return Count(seen->second);
    }
    if (object->IsCallable()) {
      // A function is not clonable, and this is the refusal that matters most:
      // a page that puts a callback in `history.state` has to find out at the
      // call rather than a navigation later.
      return false;
    }
    if (!object->IsSerializable()) {
      // A host object -- a `URL`, a `Node`, a `URLSearchParams`. Its properties are a facade over
      // something outside this heap, so a copy of them is a plain object that looks right until a
      // method is called on it. The binding layer sets the flag; this module knows only that
      // something outside said no.
      return false;
    }
    if (object->GetKind() == Object::Kind::Proxy || object->GetKind() == Object::Kind::Error) {
      // A proxy's behaviour is its handler, which is a function. An Error's
      // `stack` is a string but its identity is not, and the specification's
      // rules for cloning one arrived after this file did -- refusing is the
      // honest answer rather than a half-Error.
      return false;
    }

    // Registered *before* the members are written, so a cycle finds it.
    const std::size_t index = seen_.size();
    seen_.emplace(object, index);

    const DepthGuard guard(depth_);
    switch (object->GetKind()) {
      case Object::Kind::Array:
        return WriteArray(object);
      case Object::Kind::ArrayBuffer:
        return WriteBuffer(object, Tag::ArrayBuffer);
      case Object::Kind::TypedArray:
        return WriteTypedArray(object);
      case Object::Kind::DataView:
        return WriteBuffer(object, Tag::DataView);
      default:
        break;
    }
    if (const Value* time = object->GetOwn("#time")) {
      Byte(Tag::Date);
      return Double(ToNumber(*time));
    }
    if (const RegExp* pattern = interpreter_.GetHeap().FindRegExp(object)) {
      Byte(Tag::RegExp);
      Text(pattern->Source());
      return Text(pattern->Flags().Text());
    }
    if (const Value* entries = object->GetOwn("#entries")) {
      // A Map or a Set. They differ in the shape of an entry -- a pair against a
      // single value -- and both keep it in `#entries`, so the tag is decided by
      // asking whether the first entry is a pair. An empty one is written as a
      // Set, which is wrong for an empty Map; so the kind is taken from the
      // prototype's `constructor` name instead, which is what a page can also
      // see.
      return WriteCollection(object, *entries);
    }
    return WritePlain(object);
  }

  bool WriteArray(Object* object) {
    Byte(Tag::Array);
    Count(object->ElementCount());
    for (std::size_t i = 0; i < object->ElementCount(); ++i) {
      if (!object->HasElement(i)) {
        Byte(Tag::Hole);
        continue;
      }
      if (!Write(object->GetElement(i))) {
        return false;
      }
    }
    // An array can also carry named properties, and a page that put one there
    // meant it. Written after the elements so that a reader can build the array
    // first.
    return WriteNamed(object);
  }

  bool WritePlain(Object* object) {
    Byte(Tag::Object);
    return WriteNamed(object);
  }

  bool WriteNamed(Object* object) {
    std::vector<std::string> keys;
    for (const std::string& key : object->EnumerableKeys()) {
      const Object::Property* property = object->GetOwnProperty(key);
      if (property == nullptr || property->IsAccessor()) {
        // An accessor is a function pair. The specification's algorithm reads
        // the *value*, so this could call the getter -- and a getter that
        // navigates while a history entry is being written is exactly the
        // re-entrancy this layer must not have.
        continue;
      }
      keys.push_back(key);
    }
    Count(keys.size());
    for (const std::string& key : keys) {
      Text(key);
      const Value* value = object->GetOwn(key);
      if (!Write(value == nullptr ? Value::Undefined() : *value)) {
        return false;
      }
    }
    return true;
  }

  bool WriteCollection(Object* object, const Value& entries) {
    const bool is_map = ConstructorNamed(object, "Map");
    if (!is_map && !ConstructorNamed(object, "Set")) {
      // Something else keeping state in `#entries`. Not clonable, because this
      // layer does not know what it is.
      return false;
    }
    Byte(is_map ? Tag::Map : Tag::Set);
    const std::size_t count = entries.IsObject() ? entries.object->ElementCount() : 0;
    Count(count);
    // An entry is an array either way: a pair for a Map and a one-element array
    // for a Set (see Collections.cpp, which builds both with NewArrayValue). A
    // deleted entry is a hole Compact has not swept yet, and it is written as
    // one so the reader skips it rather than adding `undefined` to the
    // collection.
    for (std::size_t i = 0; i < count; ++i) {
      const Value entry = entries.object->GetElement(i);
      const std::size_t needed = is_map ? 2 : 1;
      if (!entry.IsObject() || entry.object->ElementCount() < needed) {
        Byte(Tag::Hole);
        continue;
      }
      if (!Write(entry.object->GetElement(0))) {
        return false;
      }
      if (is_map && !Write(entry.object->GetElement(1))) {
        return false;
      }
    }
    return true;
  }

  bool ConstructorNamed(Object* object, const char* name) {
    const Object* prototype = object->Prototype();
    if (prototype == nullptr) {
      return false;
    }
    const Value* constructor = prototype->GetOwn("constructor");
    if (constructor == nullptr || !constructor->IsObject()) {
      return false;
    }
    const Value* function_name = constructor->object->GetOwn("name");
    return function_name != nullptr && function_name->type == ValueType::String &&
           function_name->AsString() == name;
  }

  bool WriteBuffer(Object* object, Tag tag) {
    const BufferView* view = object->View();
    if (view == nullptr) {
      return false;
    }
    Byte(tag);
    return WriteBytes(*view);
  }

  bool WriteTypedArray(Object* object) {
    const BufferView* view = object->View();
    if (view == nullptr) {
      return false;
    }
    std::uint8_t kind = 0;
    bool found = false;
    for (std::uint8_t i = 0; i < std::size(kTypedArrayNames); ++i) {
      if (ConstructorNamed(object, kTypedArrayNames[i])) {
        kind = i;
        found = true;
        break;
      }
    }
    if (!found) {
      // A BigInt64Array or BigUint64Array, which this engine does not have (see
      // docs/js-conformance-roadmap.md), or a subclass whose name was changed.
      return false;
    }
    Byte(Tag::TypedArray);
    RawByte(kind);
    return WriteBytes(*view);
  }

  bool WriteBytes(const BufferView& view);

  Interpreter& interpreter_;
  std::vector<std::uint8_t> bytes_;
  // Object identity to the index it was written at. Not a set: a Reference
  // carries the index, so the reader can rebuild the same sharing.
  std::unordered_map<const Object*, std::size_t> seen_;
  int depth_ = 0;
};

// Where a half-built graph is rooted while it is being built.
//
// Everything the reader allocates lives in C++ locals and in `objects_` until it
// is attached to its parent, and **a `js::Value` in a C++ field is invisible to
// the collector**. Deserializing calls into the page -- `new Map`, `map.set`,
// `new Uint8Array` -- and the collector runs at every call, so without this an
// object read two members ago is freed while the vector still points at it. The
// module has had that bug once already (see src/bindings/MODULE.deps); this is
// the same fix, a JavaScript array hung off something that is already a root.
constexpr const char* kRootsKey = "#structured-clone-roots";

class Reader {
 public:
  Reader(Interpreter& interpreter, const std::vector<std::uint8_t>& bytes)
      : interpreter_(interpreter), bytes_(bytes) {
    roots_ = interpreter_.NewArrayValue({});
    interpreter_.Global()->SetHidden(kRootsKey, roots_);
  }
  ~Reader() { interpreter_.Global()->Delete(kRootsKey); }
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Nothing when the bytes do not describe a value. Every read is bounds
  // checked and the failure is sticky: a truncated stream stops producing
  // rather than producing something shorter than it says it is.
  std::optional<Value> Read();

 private:
  bool Byte(std::uint8_t& out) {
    if (at_ >= bytes_.size()) {
      failed_ = true;
      return false;
    }
    out = bytes_[at_++];
    return true;
  }
  bool Count(std::size_t& out) {
    if (failed_ || at_ + 8 > bytes_.size()) {
      failed_ = true;
      return false;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(bytes_[at_ + static_cast<std::size_t>(i)]) << (i * 8);
    }
    at_ += 8;
    out = static_cast<std::size_t>(value);
    return true;
  }
  bool Double(double& out) {
    std::size_t bits = 0;
    if (!Count(bits)) {
      return false;
    }
    const std::uint64_t raw = static_cast<std::uint64_t>(bits);
    std::memcpy(&out, &raw, sizeof(out));
    return true;
  }
  bool Text(std::string& out) {
    std::size_t length = 0;
    if (!Count(length)) {
      return false;
    }
    // The bound is the stream itself: a length longer than what is left is a
    // truncated or hostile stream, and allocating it first is how a bad length
    // becomes an allocation the size of the number in it.
    if (length > bytes_.size() - at_) {
      failed_ = true;
      return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes_.data() + at_), length);
    at_ += length;
    return true;
  }

  // Registers `value` as the next referenceable object *before* its members are
  // read, which is what lets a cycle close -- and roots it, which is what keeps
  // it alive until its parent holds it.
  void Remember(const Value& value) {
    objects_.push_back(value);
    Root(value);
  }
  // Roots without making referenceable. An ArrayBuffer built only so that a
  // typed array can be constructed over it is not a value the stream can point
  // at, and must still survive the constructor call that consumes it.
  void Root(const Value& value) {
    if (roots_.IsObject() && value.IsObject()) {
      roots_.object->PushElement(value);
    }
  }

  std::optional<Value> ReadNamed(const Value& target);
  std::optional<Value> ReadBytes(const char* constructor_name, std::uint8_t kind, bool typed);
  Value Construct(const char* name, std::vector<Value> arguments);

  Interpreter& interpreter_;
  const std::vector<std::uint8_t>& bytes_;
  std::size_t at_ = 0;
  bool failed_ = false;
  std::vector<Value> objects_;
  // Also a property of the global object, which is what makes the collector see
  // it. The C++ copy is a convenience, not the reference that matters.
  Value roots_;
  int depth_ = 0;
};

bool Writer::WriteBytes(const BufferView& view) {
  if (view.bytes == nullptr) {
    return false;
  }
  // `length` is in elements for a typed array and in bytes for a DataView or an
  // ArrayBuffer -- the same asymmetry the language has between `length` and
  // `byteLength` -- so the byte count goes through ElementSize either way,
  // which is 1 for the byte-shaped kinds.
  const std::size_t begin = view.offset;
  const std::size_t length = view.length * ElementSize(view.kind);
  if (begin > view.bytes->size() || length > view.bytes->size() - begin) {
    return false;
  }
  Count(length);
  bytes_.insert(bytes_.end(), view.bytes->begin() + static_cast<std::ptrdiff_t>(begin),
                view.bytes->begin() + static_cast<std::ptrdiff_t>(begin + length));
  return true;
}

Value Reader::Construct(const char* name, std::vector<Value> arguments) {
  const Value* found = interpreter_.GlobalScope()->Lookup(name);
  if (found == nullptr) {
    return Value::Undefined();
  }
  // By value, for the reason the collection's `add` is copied: constructing
  // allocates, allocating can collect, and a pointer into a binding is not
  // something to hold across that.
  const Value constructor = *found;
  // Through the page's own constructor, because a Map's index lives beside the
  // heap and a Map assembled without one answers `get` with undefined for a key
  // it contains. See StructuredClone.h.
  const Result result = interpreter_.ConstructValue(constructor, arguments);
  return result.completion == Completion::Throw ? Value::Undefined() : result.value;
}

std::optional<Value> Reader::ReadNamed(const Value& target) {
  std::size_t count = 0;
  if (!Count(count)) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::string key;
    if (!Text(key)) {
      return std::nullopt;
    }
    const std::optional<Value> value = Read();
    if (!value.has_value()) {
      return std::nullopt;
    }
    if (target.IsObject()) {
      target.object->Set(key, *value);
    }
  }
  return target;
}

std::optional<Value> Reader::ReadBytes(const char* constructor_name, std::uint8_t kind,
                                       bool typed) {
  std::size_t length = 0;
  if (!Count(length) || length > bytes_.size() - at_) {
    failed_ = true;
    return std::nullopt;
  }
  const Value buffer = Construct("ArrayBuffer", {Value::Number(static_cast<double>(length))});
  if (!buffer.IsObject() || buffer.object->View() == nullptr ||
      buffer.object->View()->bytes == nullptr) {
    return std::nullopt;
  }
  std::vector<std::uint8_t>& storage = *buffer.object->View()->bytes;
  if (storage.size() < length) {
    return std::nullopt;
  }
  if (length > 0) {
    std::memcpy(storage.data(), bytes_.data() + at_, length);
  }
  at_ += length;
  if (constructor_name == nullptr && !typed) {
    // An ArrayBuffer is the buffer itself; there is no view over it.
    Remember(buffer);
    return buffer;
  }
  // Rooted but not Remembered: the buffer is not a value the stream can point
  // at, and it still has to survive the constructor call that consumes it.
  Root(buffer);
  const char* name = typed ? kTypedArrayNames[kind] : constructor_name;
  const Value view = Construct(name, {buffer});
  Remember(view);
  return view;
}

std::optional<Value> Reader::Read() {
  if (failed_ || depth_ >= kMaxDepth) {
    failed_ = true;
    return std::nullopt;
  }
  std::uint8_t raw = 0;
  if (!Byte(raw)) {
    return std::nullopt;
  }
  const Tag tag = static_cast<Tag>(raw);
  switch (tag) {
    case Tag::Undefined:
    case Tag::Hole:
      return Value::Undefined();
    case Tag::Null:
      return Value::Null();
    case Tag::True:
      return Value::Bool(true);
    case Tag::False:
      return Value::Bool(false);
    case Tag::Number: {
      double number = 0.0;
      return Double(number) ? std::optional<Value>(Value::Number(number)) : std::nullopt;
    }
    case Tag::String: {
      std::string text;
      return Text(text) ? std::optional<Value>(Value::String(std::move(text))) : std::nullopt;
    }
    case Tag::BigInt: {
      std::string text;
      if (!Text(text)) {
        return std::nullopt;
      }
      // Through the page's own BigInt, so the digits are built by the one
      // parser that knows how.
      const Value* function = interpreter_.GlobalScope()->Lookup("BigInt");
      if (function == nullptr) {
        return Value::Undefined();
      }
      const Result result =
          interpreter_.CallFunction(*function, Value::Undefined(), {Value::String(text)});
      return result.completion == Completion::Throw ? Value::Undefined() : result.value;
    }
    case Tag::Reference: {
      std::size_t index = 0;
      if (!Count(index) || index >= objects_.size()) {
        failed_ = true;
        return std::nullopt;
      }
      return objects_[index];
    }
    case Tag::Date: {
      double time = 0.0;
      if (!Double(time)) {
        return std::nullopt;
      }
      const Value date = Construct("Date", {Value::Number(time)});
      Remember(date);
      return date;
    }
    case Tag::RegExp: {
      std::string source;
      std::string flags;
      if (!Text(source) || !Text(flags)) {
        return std::nullopt;
      }
      const Value pattern =
          Construct("RegExp", {Value::String(source), Value::String(flags)});
      Remember(pattern);
      return pattern;
    }
    case Tag::ArrayBuffer:
      return ReadBytes(nullptr, 0, false);
    case Tag::DataView:
      return ReadBytes("DataView", 0, false);
    case Tag::TypedArray: {
      std::uint8_t kind = 0;
      if (!Byte(kind) || kind >= std::size(kTypedArrayNames)) {
        failed_ = true;
        return std::nullopt;
      }
      return ReadBytes(kTypedArrayNames[kind], kind, true);
    }
    case Tag::Array: {
      std::size_t count = 0;
      if (!Count(count)) {
        return std::nullopt;
      }
      // Not reserved from `count`: the length is a number in a stream and
      // reserving from it is how a four-byte stream asks for four gigabytes.
      // The elements themselves bound it -- each one costs at least a tag byte.
      if (count > bytes_.size() - at_) {
        failed_ = true;
        return std::nullopt;
      }
      const Value array = interpreter_.NewArrayValue({});
      Remember(array);
      const DepthGuard guard(depth_);
      for (std::size_t i = 0; i < count; ++i) {
        if (at_ < bytes_.size() && static_cast<Tag>(bytes_[at_]) == Tag::Hole) {
          ++at_;
          if (array.IsObject()) {
            array.object->ResizeElements(i + 1);
          }
          continue;
        }
        const std::optional<Value> element = Read();
        if (!element.has_value()) {
          return std::nullopt;
        }
        if (array.IsObject()) {
          array.object->SetElement(i, *element);
        }
      }
      return ReadNamed(array);
    }
    case Tag::Object: {
      const Value object = interpreter_.NewObjectValue();
      Remember(object);
      const DepthGuard guard(depth_);
      return ReadNamed(object);
    }
    case Tag::Map:
    case Tag::Set: {
      std::size_t count = 0;
      if (!Count(count) || count > bytes_.size() - at_) {
        failed_ = true;
        return std::nullopt;
      }
      const bool is_map = tag == Tag::Map;
      const Value collection = Construct(is_map ? "Map" : "Set", {});
      Remember(collection);
      if (!collection.IsObject()) {
        return std::nullopt;
      }
      // A copy, not the pointer `Get` returns: that points into a property
      // table, and every call below can allocate, collect and rehash. This is
      // the same rule the rest of the module follows about C++ references into
      // the heap.
      const Value* found = collection.object->Get(is_map ? "set" : "add");
      const Value add = found == nullptr ? Value::Undefined() : *found;
      const DepthGuard guard(depth_);
      for (std::size_t i = 0; i < count; ++i) {
        if (at_ < bytes_.size() && static_cast<Tag>(bytes_[at_]) == Tag::Hole) {
          ++at_;
          continue;
        }
        const std::optional<Value> first = Read();
        if (!first.has_value()) {
          return std::nullopt;
        }
        std::vector<Value> arguments{*first};
        if (is_map) {
          const std::optional<Value> second = Read();
          if (!second.has_value()) {
            return std::nullopt;
          }
          arguments.push_back(*second);
        }
        if (add.IsObject()) {
          // Through the page's `set`/`add`, which is what maintains the index --
          // a Map's index lives beside the heap, and one assembled without it
          // answers `get` with undefined for a key it contains.
          interpreter_.CallFunction(add, collection, arguments);
        }
      }
      return collection;
    }
  }
  failed_ = true;
  return std::nullopt;
}

}  // namespace

std::optional<SerializedValue> StructuredSerialize(Interpreter& interpreter, const Value& value) {
  Writer writer(interpreter);
  if (!writer.Write(value)) {
    return std::nullopt;
  }
  SerializedValue out;
  out.bytes = writer.Take();
  return out;
}

Value StructuredDeserialize(Interpreter& interpreter, const SerializedValue& serialized) {
  if (serialized.bytes.empty()) {
    return Value::Undefined();
  }
  Reader reader(interpreter, serialized.bytes);
  return reader.Read().value_or(Value::Undefined());
}

}  // namespace microbrowser::js
