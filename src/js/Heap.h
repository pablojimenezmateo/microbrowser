#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "js/Ast.h"
#include "js/Value.h"

namespace microbrowser::js {

class Environment;
class Interpreter;

// A native function's implementation.
struct NativeCall {
  Interpreter& interpreter;
  Value self;
  const std::vector<Value>& arguments;
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
  enum class Kind : std::uint8_t { Plain, Array, Function, Native, Error };

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
  const Value* GetOwn(std::string_view key) const;
  const Property* GetOwnProperty(std::string_view key) const;
  // Walks the prototype chain, so an accessor inherited from a class body is
  // found on an instance that does not have one of its own.
  const Property* GetProperty(std::string_view key) const;
  void DefineAccessor(std::string key, Object* getter, Object* setter);
  // Walks the prototype chain. The chain is bounded while walking rather than
  // on assignment: a cycle can be built with __proto__ or with Object.create,
  // and an unbounded walk is a hang reachable from a page.
  const Value* Get(std::string_view key) const;
  void Set(std::string key, Value value);
  bool Delete(std::string_view key);
  bool HasOwn(std::string_view key) const { return GetOwn(key) != nullptr; }

  // Insertion order, which is what `for...in` and Object.keys use for string
  // keys that are not array indices.
  const std::vector<std::string>& Keys() const { return key_order_; }

  std::vector<Value>& Elements() { return elements_; }
  const std::vector<Value>& Elements() const { return elements_; }

  // Function state. Empty for anything that is not callable.
  const Node* Parameters() const { return parameters_; }
  const Node* Body() const { return body_; }
  Environment* Closure() const { return closure_; }
  bool IsArrow() const { return arrow_; }
  const NativeFunction& Native() const { return native_; }
  void MakeFunction(const Node* parameters, const Node* body, Environment* closure, bool arrow);
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
  using InstanceField = std::pair<std::string, const Node*>;
  const std::vector<InstanceField>& InstanceFields() const { return instance_fields_; }
  void AddInstanceField(std::string name, const Node* initializer) {
    instance_fields_.emplace_back(std::move(name), initializer);
  }

  // `this` captured at creation, for an arrow function.
  const Value& BoundThis() const { return bound_this_; }
  void SetBoundThis(Value value) { bound_this_ = std::move(value); }

 private:
  friend class Heap;

  Kind kind_ = Kind::Plain;
  Object* prototype_ = nullptr;
  std::unordered_map<std::string, Property> properties_;
  std::vector<std::string> key_order_;
  std::vector<Value> elements_;

  const Node* parameters_ = nullptr;
  const Node* body_ = nullptr;
  Environment* closure_ = nullptr;
  bool arrow_ = false;
  Value bound_this_;
  NativeFunction native_;
  Object* home_object_ = nullptr;
  Object* super_constructor_ = nullptr;
  std::vector<InstanceField> instance_fields_;

  bool marked_ = false;
};

// One scope.
//
// Collected like an object, and for the same reason: a closure keeps its
// defining scope alive, and that scope can hold the closure, so the cycle is
// the normal case rather than the exception.
class Environment {
 public:
  explicit Environment(Environment* parent) : parent_(parent) {}

  Environment* Parent() const { return parent_; }

  // Null when the name is not bound anywhere up the chain, which is a
  // ReferenceError rather than undefined -- the distinction the language draws
  // between "declared and unset" and "never declared".
  Value* Lookup(std::string_view name);
  bool Declare(std::string name, Value value, bool is_const);
  // False when the binding is const, which the caller turns into a TypeError.
  bool Assign(std::string_view name, const Value& value);
  bool HasOwn(std::string_view name) const { return bindings_.count(std::string(name)) != 0; }

 private:
  friend class Heap;

  struct Binding {
    Value value;
    bool is_const = false;
  };

  Environment* parent_ = nullptr;
  std::unordered_map<std::string, Binding> bindings_;
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
  void Mark(Object* object);
  void Mark(Environment* environment);
  void MarkValue(const Value& value);

  std::vector<std::unique_ptr<Object>> objects_;
  std::vector<std::unique_ptr<Environment>> environments_;
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
