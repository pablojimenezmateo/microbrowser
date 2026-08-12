#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "js/Ast.h"
#include "js/Value.h"

namespace microbrowser::js {

class Environment;
class Interpreter;
class RegExp;
class BigInt;
struct MapIndex;
// One compiled function body, from Bytecode.h. Forward-declared rather than
// included: a function object points at its code, and nothing about the heap
// depends on what an instruction looks like.
struct CompiledFunction;

std::optional<std::size_t> ParseArrayIndex(std::string_view key);

class PropertyKey;
// The key a computed access denotes. A symbol stays a symbol; everything else
// becomes its string form, which is what makes `o[1]` and `o['1']` the same
// property.
PropertyKey KeyFrom(const Value& value);
// The other direction: the value a key is handed back to script as. A proxy
// trap receives the key it was asked about, and a symbol key has to arrive as
// the symbol rather than as text.
Value KeyValue(const PropertyKey& key);

struct ArrayElement {
  Value value;
  bool present = false;
};

// What a typed array's elements are, and how wide.
//
// One entry per view type the language has, minus the two BigInt ones -- there
// is no BigInt here, and a Int64Array whose elements silently lost precision
// would be worse than one that does not exist.
enum class ElementKind : std::uint8_t {
  Int8,
  Uint8,
  Uint8Clamped,
  Int16,
  Uint16,
  Int32,
  Uint32,
  Float32,
  Float64,
};

// How many bytes one element of each kind takes.
inline constexpr std::size_t ElementSize(ElementKind kind) {
  switch (kind) {
    case ElementKind::Int8:
    case ElementKind::Uint8:
    case ElementKind::Uint8Clamped:
      return 1;
    case ElementKind::Int16:
    case ElementKind::Uint16:
      return 2;
    case ElementKind::Int32:
    case ElementKind::Uint32:
    case ElementKind::Float32:
      return 4;
    case ElementKind::Float64:
      return 8;
  }
  return 1;
}

// One element read out of, or written into, a run of bytes.
//
// Shared with DataView, which does its own byte-order shuffle and then hands
// the result here -- so the encoding of each type is written once rather than
// once per view kind.
double ReadElementBytes(const std::vector<std::uint8_t>& bytes, std::size_t at,
                        ElementKind kind);
void WriteElementBytes(std::vector<std::uint8_t>& bytes, std::size_t at, ElementKind kind,
                       double value);

// A window onto an ArrayBuffer's bytes.
//
// One record for both halves of the feature: an ArrayBuffer carries one
// covering all of its own bytes, and every typed array and DataView over it
// carries one naming a slice. That is what makes `ta.buffer` and
// `new Uint8Array(buf, 4, 2)` the same mechanism rather than two.
//
// `bytes` is shared, so several views over one buffer see each other's writes
// -- which is the entire point of the buffer being separate from the view.
// Null means the buffer was detached, and every read through it is undefined
// and every write a no-op rather than a segfault.
struct BufferView {
  std::shared_ptr<std::vector<std::uint8_t>> bytes;
  // The ArrayBuffer object this looks at, for `.buffer`. Null on the buffer
  // itself, which would otherwise point at itself and make the collector's
  // walk a cycle it has to notice rather than one it cannot build.
  Object* buffer = nullptr;
  std::size_t offset = 0;
  // In *elements* for a typed array and in bytes for a DataView, which is the
  // same asymmetry the language has between `length` and `byteLength`.
  std::size_t length = 0;
  ElementKind kind = ElementKind::Uint8;
};

// What a property is filed under.
//
// The language has exactly two kinds of key and they never compare equal to
// each other, so this is a sum type rather than a string with an escape
// convention. Deriving a symbol's key from its description would let a string
// of the same text collide with it, and not colliding is the entire reason
// symbols exist -- `Symbol.iterator` has to be a key no page can reach by
// writing one out.
//
// Constructible from a string implicitly, because the overwhelming majority of
// keys are strings and every call site that names one would otherwise say so
// twice.
class PropertyKey {
 public:
  PropertyKey() = default;
  PropertyKey(std::string text) : text_(std::move(text)) {}       // NOLINT: implicit by design
  PropertyKey(std::string_view text) : text_(text) {}             // NOLINT
  PropertyKey(const char* text) : text_(text) {}                  // NOLINT
  // The cell is the identity: two symbols with the same description are
  // different keys, which is the whole point of them.
  static PropertyKey Symbol(const Object* cell) {
    PropertyKey key;
    key.symbol_ = cell;
    return key;
  }

  bool IsSymbol() const { return symbol_ != nullptr; }
  const std::string& Text() const { return text_; }
  const Object* Cell() const { return symbol_; }

  bool operator==(const PropertyKey& other) const {
    return symbol_ == other.symbol_ && (symbol_ != nullptr || text_ == other.text_);
  }

  struct Hash {
    std::size_t operator()(const PropertyKey& key) const {
      return key.IsSymbol() ? std::hash<const void*>{}(key.Cell())
                            : std::hash<std::string>{}(key.Text());
    }
  };

 private:
  const Object* symbol_ = nullptr;
  std::string text_;
};

// A native function's implementation.
struct NativeCall {
  NativeCall(Interpreter& owner, Object* function, Value receiver,
             const std::vector<Value>& args)
      : interpreter(owner), callee(function), self(std::move(receiver)), arguments(args) {}

  Interpreter& interpreter;
  // The function object being called, as distinct from `self`, which is the
  // receiver it was read from. A native that needs per-instance state -- the
  // function `bind` returns is the one that does -- keeps it in properties on
  // this and reads it back here. Captures in the std::function are invisible
  // to the collector; properties are marked, so this is the difference between
  // state that survives a collection and a use-after-free.
  Object* callee = nullptr;
  Value self;
  const std::vector<Value>& arguments;
  Value Throw(std::string_view kind, std::string message);
  Value ThrowValue(Value value);
  bool HasThrown() const { return threw; }
  const Value& ThrownValue() const { return thrown; }

 private:
  bool threw = false;
  Value thrown;
};
using NativeFunction = std::function<Value(NativeCall&)>;

// A JavaScript object.
//
// One class for every kind of object rather than a hierarchy, because the
// language does not have a hierarchy: an array is an object with a length, a
// function is an object that can be called, and any of them can gain any
// property at any time. Modelling that with inheritance means every property
// lookup starts with a downcast.
class Object {
 public:
  enum class Kind : std::uint8_t {
    Plain,
    Array,
    Function,
    Native,
    Error,
    RegExp,
    Symbol,
    // A proxy: every property operation on it goes to a handler first. A kind
    // rather than a flag because the check happens on the hot path of every
    // property access, and comparing one byte already being read is as cheap
    // as that check can be.
    Proxy,
    // The three that carry a BufferView. Kinds rather than a flag for the same
    // reason Proxy is one: an element access has to tell a typed array from an
    // array before it does anything, and the kind byte is already being read.
    ArrayBuffer,
    TypedArray,
    DataView,
    // `document.all`: the HTML [[IsHTMLDDA]] exotic. One kind rather than a
    // flag because ToBoolean / typeof / == ask on every use, and the kind byte
    // is already being read. See HTML "DOM interface" obsolete section.
    HTMLAllCollection,
  };

  explicit Object(Kind kind) : kind_(kind) {}

  Kind GetKind() const { return kind_; }
  // The HTML willful violation: ToBoolean is false, typeof is "undefined", and
  // `== null` / `== undefined` are true -- while `!== undefined` stays true.
  // Polymer resin's `!Z && Z !== document.all` needs that last distinction;
  // without a real `document.all`, `undefined !== document.all` is false and
  // undefined sinks become the innocuous string `"zClosurez"`.
  bool IsHTMLDDA() const { return kind_ == Kind::HTMLAllCollection; }
  // A bigint cell's digits. Null for every other object -- and read on every
  // bigint operation, which is why it is a pointer here rather than a lookup
  // in the table beside the heap that owns it.
  const BigInt* BigIntDigits() const { return bigint_; }
  void SetBigIntDigits(const BigInt* digits) { bigint_ = digits; }
  // The kind of whatever is behind this, looking through any number of
  // proxies. What `Array.isArray` asks, and the reason it asks it: a proxy
  // over an array *is* an array to the language, and a feature test that said
  // otherwise would make every wrapper visible.
  Kind TargetKind() const;
  bool IsCallable() const {
    if (kind_ == Kind::Function || kind_ == Kind::Native) {
      return true;
    }
    // A proxy is callable exactly when what it wraps is -- `new Proxy(fn, {})`
    // has to be a function to everything that looks, which is how every
    // framework's function wrapper stays invisible. Out of line and bounded,
    // because the walk is a chain a page can nest and this is the one branch
    // that is not the common case.
    return kind_ == Kind::Proxy && ProxyTargetIsCallable();
  }

  // Whether the *language* may copy this object. False for a host object -- a `URL`, a `Node`, a
  // `URLSearchParams` -- which is a handle onto something outside the heap: structured cloning one
  // would produce a plain object wearing its properties, and a page would carry it around until
  // something called a method on it. The specification's answer is a DataCloneError, and this flag
  // is how the module that *knows* an object is a host object tells the one that does the copying.
  bool IsSerializable() const { return serializable_; }
  void MarkHostObject() { serializable_ = false; }

  Object* Prototype() const { return prototype_; }
  void SetPrototype(Object* prototype) { prototype_ = prototype; }

  // A property slot: either a value or a pair of accessors.
  //
  // One record rather than two maps, because `Object.keys` and `for...in` have
  // to see both kinds in insertion order, and two maps means merging two
  // orders at every enumeration.
  struct Property {
    Value value;
    // Null unless this is an accessor. A getter-only property is legal and
    // assigning to it is a silent no-op outside strict mode, so the two
    // pointers are independent rather than a single flag.
    Object* getter = nullptr;
    Object* setter = nullptr;
    // The three attributes, defaulting the way an ordinary assignment leaves
    // them rather than the way `defineProperty` does -- those defaults are the
    // opposite of each other and the difference is load-bearing.
    //
    // `enumerable` is the one that earns its place: every transpiled module
    // writes `Object.defineProperty(exports, '__esModule', {value: true})`,
    // which is *non*-enumerable, and an engine that reported it from
    // `Object.keys` would put it in every loop over every module's exports.
    bool enumerable = true;
    bool writable = true;
    bool configurable = true;

    bool IsAccessor() const { return getter != nullptr || setter != nullptr; }
  };

  // Own properties only. Null when absent, which is distinct from a property
  // whose value is undefined -- `'x' in o` and `o.x === undefined` are
  // different questions.
  const Value* GetOwn(const PropertyKey& key) const;
  const Property* GetOwnProperty(const PropertyKey& key) const;
  // Walks the prototype chain, so an accessor inherited from a class body is
  // found on an instance that does not have one of its own.
  const Property* GetProperty(const PropertyKey& key) const;
  void DefineAccessor(PropertyKey key, Object* getter, Object* setter);
  // Walks the prototype chain. The chain is bounded while walking rather than
  // on assignment: a cycle can be built with __proto__ or with Object.create,
  // and an unbounded walk is a hang reachable from a page.
  const Value* Get(const PropertyKey& key) const;
  void Set(PropertyKey key, Value value);
  bool Delete(const PropertyKey& key);
  bool HasOwn(const PropertyKey& key) const;

  // Frozen, by Object.freeze. Writes, deletes and new properties become
  // silent no-ops -- which is what they are outside strict mode, and this
  // engine is outside it.
  //
  // Checked in Set, Delete and the element mutators rather than only in the
  // interpreter, so that a builtin cannot write through it either: `arr.push`
  // on a frozen array has to fail, and it does not go through SetProperty.
  // SetElements is the deliberate exception -- it is how a collection's own
  // storage is rebuilt, and freezing a Map is not what a page means by it.
  // How far an object has been closed to change.
  //
  // One ordered field rather than three flags, because the three levels the
  // language defines are nested: sealing is preventing extensions plus
  // refusing deletes, and freezing is sealing plus refusing writes. Ordered so
  // that `>= Sealed` is "cannot delete" and `== Frozen` is "cannot write",
  // which is the whole of what the object model has to enforce.
  enum class Integrity : std::uint8_t { Extensible, NonExtensible, Sealed, Frozen };
  bool IsFrozen() const { return integrity_ == Integrity::Frozen; }
  bool IsSealed() const { return integrity_ >= Integrity::Sealed; }
  bool IsExtensible() const { return integrity_ == Integrity::Extensible; }
  // Never loosens: an object that has been frozen cannot be made extensible
  // again, which is what a page freezing its own state is relying on.
  void Restrict(Integrity level) {
    integrity_ = level > integrity_ ? level : integrity_;
  }
  void Freeze() { Restrict(Integrity::Frozen); }

  // Insertion order, which is what `for...in` and Object.keys use for string
  // keys that are not array indices. Symbol-keyed properties are deliberately
  // absent: nothing that enumerates an object is supposed to see them, which
  // is what makes a symbol a safe place to hang a protocol hook.
  // Every own string key, in enumeration order -- integer-like first and
  // ascending, then the rest in insertion order. Maintained on insert rather
  // than sorted on read, because every enumeration would otherwise pay for it.
  //
  // *Every* one, including the non-enumerable: this is what
  // `Object.getOwnPropertyNames` reports. Anything that walks properties on a
  // page's behalf -- `Object.keys`, `for...in`, spread, `JSON.stringify` --
  // wants EnumerableKeys instead.
  const std::vector<std::string>& Keys() const { return key_order_; }
  // The subset a page can see by enumerating. Built on demand: filtering costs
  // a walk and most objects have nothing hidden, so the common case is a copy
  // of a vector that was going to be walked anyway.
  std::vector<std::string> EnumerableKeys() const {
    std::vector<std::string> out;
    out.reserve(key_order_.size());
    for (const std::string& key : key_order_) {
      const Property* property = GetOwnProperty(key);
      if (property == nullptr || property->enumerable) {
        out.push_back(key);
      }
    }
    return out;
  }
  // Sets a property that enumeration does not see.
  //
  // Two callers, and they are the same case twice: a built-in's own state --
  // a Map's entries, a proxy's target, a Date's instant -- which is stored as
  // a `#`-prefixed property because there is nowhere better, and must not turn
  // up in a `for...in` over the object it belongs to. And `constructor` on a
  // prototype, which the language makes non-enumerable for the same reason.
  void SetHidden(PropertyKey key, Value value) {
    Set(key, std::move(value));
    // After the Set rather than instead of it: Set may have refused -- frozen,
    // or non-extensible -- and HideProperty then finds nothing, which is the
    // right answer either way.
    HideProperty(key);
  }
  // Marks one own property non-enumerable, when it is there.
  void HideProperty(const PropertyKey& key) {
    const auto found = properties_.find(key);
    if (found != properties_.end()) {
      found->second.enumerable = false;
    }
  }
  // Marks every own property non-enumerable.
  //
  // What makes a built-in prototype's methods invisible to `for...in` and to
  // `Object.keys`, which is what they are in the language: `for (const k in
  // [])` must not report `map`. Applied in one sweep after installation rather
  // than at each of two hundred install sites, because a site that forgot
  // would be invisible until a page enumerated the one object it touched.
  void HideProperties() {
    for (auto& entry : properties_) {
      entry.second.enumerable = false;
    }
  }
  // Sets a property with attributes, which an ordinary assignment cannot: an
  // assignment leaves a new property enumerable, writable and configurable,
  // and `Object.defineProperty` leaves it none of those unless told otherwise.
  void Define(PropertyKey key, Property property);
  // The symbol-keyed own properties, which `key_order_` deliberately does not
  // hold: a symbol has no text to file it under, and `Object.keys` must not
  // report one. Built on demand rather than kept, because the one caller --
  // Object.getOwnPropertySymbols -- is not on any hot path.
  std::vector<PropertyKey> SymbolKeys() const {
    std::vector<PropertyKey> keys;
    for (const auto& entry : properties_) {
      if (entry.first.IsSymbol()) {
        keys.push_back(entry.first);
      }
    }
    return keys;
  }

  // How many elements this has, wherever they live.
  //
  // A typed array's are bytes in a buffer rather than Values in a vector, and
  // routing that through here is what lets every generic Array.prototype
  // method -- map, filter, reduce, indexOf -- work on one unchanged. Those
  // methods are specified over array-likes and this is what makes one.
  std::size_t ElementCount() const {
    return view_ != nullptr ? view_->length : elements_.size();
  }
  // The window onto a buffer, for a typed array, a DataView or an ArrayBuffer.
  // Null for everything else, which is what every element access tests.
  const BufferView* View() const { return view_.get(); }
  void MakeView(BufferView view);
  bool HasElement(std::size_t index) const;
  Value GetElement(std::size_t index) const;
  void SetElements(std::vector<Value> elements, std::vector<bool> present);
  void ResizeElements(std::size_t size);
  void SetElement(std::size_t index, Value value);
  void PushElement(Value value);
  Value PopElement();

  // Function state. Empty for anything that is not callable.
  const Node* Parameters() const { return parameters_; }
  const Node* Body() const { return body_; }
  Environment* Closure() const { return closure_; }
  bool IsArrow() const { return arrow_; }
  const NativeFunction& Native() const { return native_; }
  void MakeFunction(const Node* parameters, const Node* body, Environment* closure, bool arrow);
  // The compiled body, when there is one. A function has either this or the
  // two AST pointers above and never both: they are two spellings of the same
  // thing, and which one a function got is what CallFunction dispatches on.
  //
  // Here rather than in a table beside the heap -- which is where compiled
  // regular expressions and the weak tables live -- because this is read on
  // every call. A hash lookup per call is the one place that cost would show.
  const CompiledFunction* Code() const { return code_; }
  void MakeCompiled(const CompiledFunction* code, Environment* closure, bool arrow);
  void MakeNative(NativeFunction native) {
    kind_ = Kind::Native;
    native_ = std::move(native);
  }
  // What `super.x` resolves against: the object the method was *defined* on,
  // not the one it was called through. Using the receiver instead makes a
  // three-level hierarchy recurse into itself.
  Object* HomeObject() const { return home_object_; }
  void SetHomeObject(Object* home) { home_object_ = home; }
  // The class this one extends, for `super(...)`.
  Object* SuperConstructor() const { return super_constructor_; }
  void SetSuperConstructor(Object* parent) { super_constructor_ = parent; }

  // Instance field initializers, in declaration order. The node is null for a
  // field with no initializer, which is undefined rather than absent.
  using InstanceField = std::pair<PropertyKey, const Node*>;
  const std::vector<InstanceField>& InstanceFields() const { return instance_fields_; }
  void AddInstanceField(PropertyKey name, const Node* initializer) {
    instance_fields_.emplace_back(std::move(name), initializer);
  }

  // `this` captured at creation, for an arrow function.
  const Value& BoundThis() const { return bound_this_; }
  void SetBoundThis(Value value) { bound_this_ = std::move(value); }

 private:
  friend class Heap;

  Kind kind_ = Kind::Plain;
  Object* prototype_ = nullptr;
  std::unordered_map<PropertyKey, Property, PropertyKey::Hash> properties_;
  std::vector<std::string> key_order_;
  std::vector<ArrayElement> elements_;
  // Set only for an ArrayBuffer, a typed array or a DataView. A pointer rather
  // than the record itself because every other object would otherwise carry
  // forty bytes for a kind of object a page makes a handful of -- the same
  // reasoning that keeps compiled patterns and map indexes beside the heap,
  // except that this one is read on every element access and a hash lookup
  // there would show.
  std::unique_ptr<BufferView> view_;
  // Borrowed from the heap's table, which owns it and drops it in the sweep
  // that frees this cell.
  const BigInt* bigint_ = nullptr;

  const Node* parameters_ = nullptr;
  const Node* body_ = nullptr;
  // Owned by the interpreter's list of compiled programs, which outlives every
  // function made from one -- the same arrangement, and the same reason, as the
  // AST pointers above it.
  const CompiledFunction* code_ = nullptr;
  Environment* closure_ = nullptr;
  bool arrow_ = false;
  bool serializable_ = true;
  Value bound_this_;
  NativeFunction native_;
  Object* home_object_ = nullptr;
  Object* super_constructor_ = nullptr;
  std::vector<InstanceField> instance_fields_;

  bool marked_ = false;
  // Files a new string key in enumeration order. See Keys().
  void RecordKey(const std::string& text);
  // Whether the object behind this proxy -- possibly through more proxies --
  // can be called. Bounded, like every chain walk here.
  bool ProxyTargetIsCallable() const;

  Integrity integrity_ = Integrity::Extensible;
};

// Heterogeneous lookup for a scope's bindings.
//
// Without it, `bindings_.find(std::string(name))` builds a std::string -- and
// for anything past the small-string limit, calls the allocator -- once per
// scope in the chain, on every read of every variable. That is a malloc in the
// inner loop of the language. Measured at 165ns per iteration of
// `t += i * 3 - 1` before, and the benchmark is what found it: calls were three
// times faster on the machine and loops were barely faster at all, which is the
// shape of a cost neither engine had anything to do with.
struct NameHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view text) const {
    return std::hash<std::string_view>{}(text);
  }
};
struct NameEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

// One binding, wherever it lives.
//
// Named here rather than inside Environment because a binding no longer always
// lives in one. A call whose scopes nothing can capture keeps them in a slice
// of the machine's locals stack instead, and that slice holds these -- so the
// two storage places hold the same record and a slot means the same thing in
// both. See CompiledFunction::frame_locals.
struct Binding {
  Value value;
  bool is_const = false;
  // Whether the declaration has run. Reserved-but-unset is a third state,
  // distinct from both "absent" and "undefined": reading a binding before its
  // own `let` is a ReferenceError, and reserving the slot up front must not
  // quietly turn that into undefined.
  bool live = false;
  // Whether writing to this const is ignored rather than a TypeError.
  //
  // One binding in the language is immutable *and* silent: the name a named
  // function expression sees itself by. The spec makes it with
  // CreateImmutableBinding(name, false) -- the `false` is "not strict", and a
  // set to a non-strict immutable binding is dropped. `const` uses the same
  // record with the flag the other way, which is why this is a field here
  // rather than a second kind of scope.
  //
  // It matters because the alternative is loud: old code that does
  // `var f = function f(){ ...; f = null }` would take a TypeError and lose
  // the rest of the script, which is the failure this whole area was fixed for.
  bool silent_const = false;
};

// One scope.
//
// Collected like an object, and for the same reason: a closure keeps its
// defining scope alive, and that scope can hold the closure, so the cycle is
// the normal case rather than the exception.
// Bindings live in a vector and the names index into it.
//
// One store, two ways in. Compiled code resolved its names to a (hops, slot)
// pair while compiling and indexes straight in; everything else -- the
// tree-walker, every builtin, a class body -- still asks by name and pays a
// hash to get the same slot. Values live in `slots_` only, so the two paths
// cannot disagree about what a binding holds.
//
// Measured before this existed: 15.6ns per name operation, plus 3.8ns per scope
// crossed, against a loop iteration of 158ns. Half the time in a loop was
// hashing names the compiler already knew the answer for.
class Environment {
 public:
  explicit Environment(Environment* parent) : parent_(parent) {}

  Environment* Parent() const { return parent_; }
  // `hops` scopes up, or null past the end of the chain. What a resolved name
  // walks before indexing.
  Environment* Ancestor(std::uint32_t hops);

  // Null when the name is not bound anywhere up the chain, which is a
  // ReferenceError rather than undefined -- the distinction the language draws
  // between "declared and unset" and "never declared".
  Value* Lookup(std::string_view name);
  bool Declare(std::string name, Value value, bool is_const, bool silent_const = false);
  // What happened to a write, which is four answers rather than two: the caller
  // has to tell "no such binding" from "const" to know whether to make a global
  // or to throw, and `Ignored` is the third thing a write can do -- see
  // Binding::silent_const.
  enum class AssignResult : std::uint8_t { Stored, Ignored, Constant, Unbound };
  AssignResult Assign(std::string_view name, const Value& value);
  bool HasOwn(std::string_view name) const;

  // --- The resolved path ---------------------------------------------------
  //
  // Slots are reserved when the scope is created and filled as their
  // declarations run, which is what makes an index a compile-time constant
  // even when control flow skips a declaration -- `switch (n) { case 1: let a;
  // case 2: let b }` entered at the second case must still put `b` where the
  // compiler said it would be.
  void Reserve(std::uint32_t count);
  // Null until the declaration has run. That null is a real answer: reading a
  // binding before its own `let` is a ReferenceError, and reserving the slot
  // must not turn it into undefined.
  Value* SlotValue(std::uint32_t index);
  bool SlotIsConst(std::uint32_t index) const;
  // The whole record, for the machine, which needs all three fields and would
  // otherwise range-check the same index three times. Null only when the slot
  // was never reserved -- an unset slot comes back with `live` false, which is
  // the state SlotValue reports as null.
  Binding* Slot(std::uint32_t index) {
    return index < slots_.size() ? &slots_[index] : nullptr;
  }
  // Fills a reserved slot and registers its name, so a name lookup from
  // outside compiled code finds it from this point on and not before.
  void DeclareSlot(std::uint32_t index, std::string name, Value value, bool is_const,
                   bool silent_const = false);
  // Copies every binding out of `other`, slots and names alike.
  //
  // One caller: the per-iteration environment a `for (let i = ...)` makes at
  // the end of each pass. The layout has to match exactly, because the
  // compiler resolved the body's names against it and the *next* iteration
  // runs the same instructions -- so this is a copy rather than a rebuild.
  void CopyBindingsFrom(const Environment& other);

 private:
  friend class Heap;

  Environment* parent_ = nullptr;
  std::vector<Binding> slots_;
  std::unordered_map<std::string, std::uint32_t, NameHash, NameEqual> index_;
  bool marked_ = false;
};

// One generator, as the machine sees it.
//
// A generator object is an ordinary object to a page and two facts to the
// interpreter: which filed frame is its, and whether that frame may be put
// back. Both live here rather than on the object; see AttachGenerator.
struct GeneratorState {
  // Which filed frame is this generator's, as an id into the interpreter's
  // suspension table. An id rather than a pointer for the reason the table is
  // keyed by one: a page can reach the generator, and a number is a thing it
  // can do nothing with.
  std::uint64_t suspension = 0;
  enum class Status : std::uint8_t {
    // Made by the call, its parameters bound, and not one line of its body
    // run. `throw` and `return` on one of these must not start the body.
    Start,
    // Waiting at a `yield`.
    Suspended,
    // Waiting at an `await`, which only an async generator can be. Distinct
    // from Suspended because a settled promise is what puts this one back: a
    // `next` arriving now has to queue rather than resume, or the frame goes
    // back twice.
    Awaiting,
    // Its frame is on the machine right now. `next` on one of these is a
    // generator resuming itself, which the spec makes a TypeError -- and here
    // it would be the same frame put back twice.
    Running,
    // Returned, threw, or was closed. Never resumable, and its frame is gone.
    Done,
  };
  Status status = Status::Start;
};

// Owns every object and environment, and collects them.
//
// Mark and sweep, not reference counting: closures make cycles the normal case
// and a refcounted cycle is a leak that grows with every page.
//
// Collection runs only when the interpreter says it is safe -- see
// Interpreter::MaybeCollect. A tree-walking interpreter keeps live values in
// C++ locals where a collector cannot see them, so the roots are the global
// object and the environment stack the interpreter tracks, and a collection in
// the middle of evaluating an expression would free a temporary that is still
// on the C++ stack. That constraint is a real cost of tree-walking and one of
// the concrete reasons a bytecode VM is worth building: its value stack is
// explicit, so it can be scanned.
class Heap {
 public:
  Heap() = default;
  ~Heap();

  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  Object* AllocateObject(Object::Kind kind);
  Environment* AllocateEnvironment(Environment* parent);

  // Frees everything not reachable from `roots`. Returns how many objects went.
  std::size_t Collect(const std::vector<Object*>& object_roots,
                      const std::vector<Environment*>& environment_roots);

  // A compiled pattern belonging to a RegExp object.
  //
  // Kept beside the object rather than in it. Every JavaScript object would
  // otherwise carry a pointer for a kind of object a page makes a handful of,
  // and the collector is already the thing that knows when an object dies --
  // so the entry is dropped by the same sweep that frees it, rather than by a
  // second lifetime story that could disagree.
  // A bigint's digits, beside the cell that is its identity. Here for the
  // reason the compiled patterns are: the collector is what knows when a cell
  // dies, and the sweep that frees it drops the digits.
  void AttachBigInt(const Object* cell, std::shared_ptr<const BigInt> digits);
  const BigInt* FindBigInt(const Object* cell) const;

  void AttachRegExp(const Object* object, std::shared_ptr<const RegExp> pattern);
  const RegExp* FindRegExp(const Object* object) const;

  // A Map or Set's key-to-position index. Kept here for the same reason and on
  // the same terms as the compiled pattern above: it is native state belonging
  // to one object, and the sweep that frees the object is what drops it.
  MapIndex* AttachMapIndex(const Object* object);
  MapIndex* FindMapIndex(const Object* object) const;

  // A generator's state.
  //
  // Beside the object rather than in it, on the same terms as the compiled
  // pattern above -- native state belonging to one object, dropped by the
  // sweep that frees it -- and for one more reason that only applies here. In
  // the object it would be a property, and a property is something the page
  // can write. A page that set its own `#suspension` would have `next` put
  // back a frame that had already been put back, which is a use-after-free
  // with a script behind it rather than a wrong answer.
  GeneratorState* AttachGenerator(const Object* object);
  GeneratorState* FindGenerator(const Object* object) const;

  // A WeakMap's entries.
  //
  // Here rather than in the object because they have to be *weak*, and only
  // the collector can say whether a key is still reachable. An entry survives
  // a collection exactly when its key does: the key holds the value alive and
  // not the other way round, which is the whole difference from a Map and the
  // reason a WeakMap does not leak the objects it is keyed on.
  void WeakSet(const Object* table, const Object* key, Value value);
  const Value* WeakGet(const Object* table, const Object* key) const;
  bool WeakDelete(const Object* table, const Object* key);
  void MakeWeakTable(const Object* table);

  // A ceiling on live cells.
  //
  // Needed because the collector cannot run during evaluation -- see the note
  // above -- so a script that recurses while allocating grows the heap with
  // nothing able to shrink it. Past the limit, allocation fails and the
  // interpreter turns that into a RangeError, which is the same answer a real
  // engine gives when it cannot grow.
  //
  // Soft capacity for live cells (objects + environments). Past the limit,
  // allocation fails and the interpreter turns that into a RangeError.
  //
  // 2M rather than 500k: youtube.com's Polymer upgrades hold about 500k live
  // cells at peak with the collector running (js.heap_live_peak), so the old
  // ceiling was a hard wall the moment a custom-element reaction did any real
  // work. The bytecode VM makes this a *live-set* budget rather than a "fail
  // before collect" budget -- see MaybeCollect and GatherVmRoots.
  void SetLimit(std::size_t cells) { limit_ = cells; }
  bool AtLimit() const { return objects_.size() + environments_.size() >= limit_; }

  std::size_t ObjectCount() const { return objects_.size(); }
  std::size_t EnvironmentCount() const { return environments_.size(); }
  // Allocations since the last collection, so a caller can decide when the
  // cost of tracing is worth paying.
  std::size_t AllocationsSinceCollection() const { return since_collection_; }

 private:
  void DrainWorklists();
  bool IsMarked(const Value& value) const;
  void Mark(Object* object);
  void Mark(Environment* environment);
  void MarkValue(const Value& value);

  std::vector<std::unique_ptr<Object>> objects_;
  std::vector<std::unique_ptr<Environment>> environments_;
  // Sparse: one entry per RegExp object alive, not one slot per object.
  std::unordered_map<const Object*, std::shared_ptr<const BigInt>> bigints_;
  std::unordered_map<const Object*, std::shared_ptr<const RegExp>> regexps_;
  // Same, for Map and Set.
  std::unordered_map<const Object*, std::shared_ptr<MapIndex>> map_indexes_;
  // Same, for a generator. Behind a pointer like the two above, and here that
  // is load-bearing rather than uniform: the interpreter holds a
  // GeneratorState* across running the generator's body, and the body can make
  // another generator. Held by value, that insertion would rehash the map and
  // the held pointer would be into freed memory.
  std::unordered_map<const Object*, std::unique_ptr<GeneratorState>> generators_;
  // One inner table per WeakMap. Keyed by the object rather than held on it,
  // for the reason above.
  std::unordered_map<const Object*, std::unordered_map<const Object*, Value>> weak_tables_;
  std::size_t since_collection_ = 0;
  // Around 150 MB here, which is far more than any page legitimately needs and
  // far less than the machine has. Measured rather than guessed: the fuzzer's
  // recursive allocator reached 600 MB at four times this.
  std::size_t limit_ = 2'000'000;
  // Worklist, kept as a member so a deep object graph is traced iteratively.
  // Recursing here would put the collector's stack depth under the control of
  // whoever wrote the page.
  std::vector<Object*> object_worklist_;
  std::vector<Environment*> environment_worklist_;
};

}  // namespace microbrowser::js
