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
  };

  explicit Object(Kind kind) : kind_(kind) {}

  Kind GetKind() const { return kind_; }
  bool IsCallable() const { return kind_ == Kind::Function || kind_ == Kind::Native; }

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
  bool IsFrozen() const { return frozen_; }
  void Freeze() { frozen_ = true; }

  // Insertion order, which is what `for...in` and Object.keys use for string
  // keys that are not array indices. Symbol-keyed properties are deliberately
  // absent: nothing that enumerates an object is supposed to see them, which
  // is what makes a symbol a safe place to hang a protocol hook.
  const std::vector<std::string>& Keys() const { return key_order_; }

  std::size_t ElementCount() const { return elements_.size(); }
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

  const Node* parameters_ = nullptr;
  const Node* body_ = nullptr;
  // Owned by the interpreter's list of compiled programs, which outlives every
  // function made from one -- the same arrangement, and the same reason, as the
  // AST pointers above it.
  const CompiledFunction* code_ = nullptr;
  Environment* closure_ = nullptr;
  bool arrow_ = false;
  Value bound_this_;
  NativeFunction native_;
  Object* home_object_ = nullptr;
  Object* super_constructor_ = nullptr;
  std::vector<InstanceField> instance_fields_;

  bool marked_ = false;
  bool frozen_ = false;
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
  bool Declare(std::string name, Value value, bool is_const);
  // False when the binding is const, which the caller turns into a TypeError.
  bool Assign(std::string_view name, const Value& value);
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
  // Fills a reserved slot and registers its name, so a name lookup from
  // outside compiled code finds it from this point on and not before.
  void DeclareSlot(std::uint32_t index, std::string name, Value value, bool is_const);

 private:
  friend class Heap;

  struct Binding {
    Value value;
    bool is_const = false;
    // Whether the declaration has run. Reserved-but-unset is a third state,
    // distinct from both "absent" and "undefined".
    bool live = false;
  };

  Environment* parent_ = nullptr;
  std::vector<Binding> slots_;
  std::unordered_map<std::string, std::uint32_t, NameHash, NameEqual> index_;
  bool marked_ = false;
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
  void AttachRegExp(const Object* object, std::shared_ptr<const RegExp> pattern);
  const RegExp* FindRegExp(const Object* object) const;

  // A Map or Set's key-to-position index. Kept here for the same reason and on
  // the same terms as the compiled pattern above: it is native state belonging
  // to one object, and the sweep that frees the object is what drops it.
  MapIndex* AttachMapIndex(const Object* object);
  MapIndex* FindMapIndex(const Object* object) const;

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
  // When the bytecode VM arrives with a scannable value stack, this becomes a
  // trigger to collect rather than a reason to fail.
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
  std::unordered_map<const Object*, std::shared_ptr<const RegExp>> regexps_;
  // Same, for Map and Set.
  std::unordered_map<const Object*, std::shared_ptr<MapIndex>> map_indexes_;
  // One inner table per WeakMap. Keyed by the object rather than held on it,
  // for the reason above.
  std::unordered_map<const Object*, std::unordered_map<const Object*, Value>> weak_tables_;
  std::size_t since_collection_ = 0;
  // Around 150 MB here, which is far more than any page legitimately needs and
  // far less than the machine has. Measured rather than guessed: the fuzzer's
  // recursive allocator reached 600 MB at four times this.
  std::size_t limit_ = 500'000;
  // Worklist, kept as a member so a deep object graph is traced iteratively.
  // Recursing here would put the collector's stack depth under the control of
  // whoever wrote the page.
  std::vector<Object*> object_worklist_;
  std::vector<Environment*> environment_worklist_;
};

}  // namespace microbrowser::js
