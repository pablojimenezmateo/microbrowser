#include "js/Heap.h"

#include <algorithm>
#include <utility>

#include "js/Collections.h"
#include "js/RegExp.h"

namespace microbrowser::js {

namespace {

// How far a prototype chain may be walked. A cycle is buildable from a page
// (`a.__proto__ = b; b.__proto__ = a`), so the walk is bounded rather than
// trusted -- an unbounded one is a hang, not a wrong answer.
constexpr int kMaxPrototypeDepth = 1000;
constexpr std::size_t kMaxArrayIndex = 4294967294ull;

}  // namespace

std::optional<std::size_t> ParseArrayIndex(std::string_view key) {
  if (key.empty() || key.size() > 10) {
    return std::nullopt;
  }
  if (key.size() > 1 && key[0] == '0') {
    return std::nullopt;  // "01" is a property name, not an index.
  }
  std::size_t index = 0;
  for (const char c : key) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    index = index * 10 + static_cast<std::size_t>(c - '0');
  }
  if (index > kMaxArrayIndex) {
    return std::nullopt;
  }
  return index;
}

Value KeyValue(const PropertyKey& key) {
  return key.IsSymbol() ? Value::Sym(const_cast<Object*>(key.Cell()))
                        : Value::String(key.Text());
}

PropertyKey KeyFrom(const Value& value) {
  return value.IsSymbol() ? PropertyKey::Symbol(value.object) : PropertyKey(ToString(value));
}

const Object::Property* Object::GetOwnProperty(const PropertyKey& key) const {
  const auto found = properties_.find(key);
  return found == properties_.end() ? nullptr : &found->second;
}

const Value* Object::GetOwn(const PropertyKey& key) const {
  const Property* property = GetOwnProperty(key);
  // An accessor has no value to hand back. Callers that can invoke a getter go
  // through GetProperty; the ones that cannot are asking about data.
  return property == nullptr || property->IsAccessor() ? nullptr : &property->value;
}

const Object::Property* Object::GetProperty(const PropertyKey& key) const {
  const Object* current = this;
  for (int depth = 0; current != nullptr && depth < kMaxPrototypeDepth; ++depth) {
    if (const Property* property = current->GetOwnProperty(key)) {
      return property;
    }
    current = current->prototype_;
  }
  return nullptr;
}

const Value* Object::Get(const PropertyKey& key) const {
  const Property* property = GetProperty(key);
  return property == nullptr || property->IsAccessor() ? nullptr : &property->value;
}

void Object::Set(PropertyKey key, Value value) {
  if (IsFrozen()) {
    return;
  }
  const auto found = properties_.find(key);
  if (found == properties_.end() && !IsExtensible()) {
    // A *new* property on a non-extensible object. Writing an existing one is
    // still allowed, which is the whole difference between sealed and frozen.
    return;
  }
  if (found != properties_.end()) {
    if (!found->second.writable && !found->second.IsAccessor()) {
      return;  // a non-writable data property; the write is a silent no-op
    }
    found->second.value = std::move(value);
    found->second.getter = nullptr;
    found->second.setter = nullptr;
    return;
  }
  if (!key.IsSymbol()) {
    RecordKey(key.Text());
  }
  properties_.emplace(std::move(key), Property{std::move(value), nullptr, nullptr});
}

void Object::RecordKey(const std::string& text) {
  // Integer-like keys come first, in ascending numeric order, and everything
  // else follows in insertion order. That is the language's rule, not an
  // implementation detail: `Object.keys({b:1, a:2, 1:3, 0:4})` is
  // `['0','1','b','a']` everywhere, and a page that renders a keyed map in
  // enumeration order can tell.
  const std::optional<std::size_t> index = ParseArrayIndex(text);
  if (!index) {
    key_order_.push_back(text);
    return;
  }
  // Only the leading run is numeric, so the search stops at the first key that
  // is not -- which bounds it by how many integer keys there are rather than
  // by how many keys there are.
  std::size_t at = 0;
  while (at < key_order_.size()) {
    const std::optional<std::size_t> existing = ParseArrayIndex(key_order_[at]);
    if (!existing || *existing > *index) {
      break;
    }
    ++at;
  }
  key_order_.insert(key_order_.begin() + static_cast<std::ptrdiff_t>(at), text);
}

Object::Kind Object::TargetKind() const {
  const Object* current = this;
  for (int depth = 0; current != nullptr && depth < kMaxPrototypeDepth; ++depth) {
    if (current->kind_ != Kind::Proxy) {
      return current->kind_;
    }
    const Value* behind = current->GetOwn("#target");
    current = behind != nullptr && behind->IsObject() ? behind->object : nullptr;
  }
  return Kind::Proxy;
}

bool Object::ProxyTargetIsCallable() const {
  const Object* current = this;
  for (int depth = 0; current != nullptr && depth < kMaxPrototypeDepth; ++depth) {
    if (current->kind_ != Kind::Proxy) {
      return current->kind_ == Kind::Function || current->kind_ == Kind::Native;
    }
    const Value* behind = current->GetOwn("#target");
    current = behind != nullptr && behind->IsObject() ? behind->object : nullptr;
  }
  return false;
}

void Object::Define(PropertyKey key, Property property) {
  if (IsFrozen()) {
    return;
  }
  const auto found = properties_.find(key);
  if (found == properties_.end()) {
    if (!IsExtensible()) {
      return;
    }
    if (!key.IsSymbol()) {
      RecordKey(key.Text());
    }
    properties_.emplace(std::move(key), std::move(property));
    return;
  }
  if (!found->second.configurable && !found->second.writable) {
    return;
  }
  found->second = std::move(property);
}

void Object::DefineAccessor(PropertyKey key, Object* getter, Object* setter) {
  if (IsFrozen()) {
    return;
  }
  const auto found = properties_.find(key);
  if (found != properties_.end()) {
    // A second `get`/`set` for the same name fills in the other half rather
    // than replacing it, which is what `get x(){} set x(v){}` means.
    if (getter != nullptr) {
      found->second.getter = getter;
    }
    if (setter != nullptr) {
      found->second.setter = setter;
    }
    found->second.value = Value::Undefined();
    return;
  }
  if (!key.IsSymbol()) {
    RecordKey(key.Text());
  }
  properties_.emplace(std::move(key), Property{Value::Undefined(), getter, setter});
}

bool Object::Delete(const PropertyKey& key) {
  if (IsSealed()) {
    return false;
  }
  if (kind_ == Kind::Array && !key.IsSymbol()) {
    if (key.Text() == "length") {
      return false;
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      if (*index < elements_.size()) {
        elements_[*index].value = Value::Undefined();
        elements_[*index].present = false;
      }
      return true;
    }
  }
  const auto found = properties_.find(key);
  if (found == properties_.end()) {
    return true;
  }
  if (!found->second.configurable) {
    return false;
  }
  properties_.erase(found);
  if (!key.IsSymbol()) {
    key_order_.erase(std::remove(key_order_.begin(), key_order_.end(), key.Text()),
                     key_order_.end());
  }
  return true;
}

bool Object::HasOwn(const PropertyKey& key) const {
  if (kind_ == Kind::Array && !key.IsSymbol()) {
    if (key.Text() == "length") {
      return true;
    }
    if (const std::optional<std::size_t> index = ParseArrayIndex(key.Text())) {
      return HasElement(*index);
    }
  }
  return GetOwnProperty(key) != nullptr;
}

bool Object::HasElement(std::size_t index) const {
  return index < elements_.size() && elements_[index].present;
}

Value Object::GetElement(std::size_t index) const {
  return HasElement(index) ? elements_[index].value : Value::Undefined();
}

void Object::SetElements(std::vector<Value> elements, std::vector<bool> present) {
  elements_.clear();
  elements_.reserve(elements.size());
  for (std::size_t i = 0; i < elements.size(); ++i) {
    elements_.push_back(ArrayElement{
        std::move(elements[i]),
        present.empty() ? true : (i < present.size() && present[i]),
    });
  }
}

void Object::ResizeElements(std::size_t size) {
  elements_.resize(size);
}

void Object::SetElement(std::size_t index, Value value) {
  if (IsFrozen()) {
    return;
  }
  if (index >= elements_.size() && !IsExtensible()) {
    return;  // growing an array is an extension
  }
  if (index >= elements_.size()) {
    elements_.resize(index + 1);
  }
  elements_[index] = ArrayElement{std::move(value), true};
}

void Object::PushElement(Value value) {
  if (!IsExtensible()) {
    return;
  }
  elements_.push_back(ArrayElement{std::move(value), true});
}

Value Object::PopElement() {
  if (IsSealed() || elements_.empty()) {
    return Value::Undefined();
  }
  Value value = elements_.back().present ? elements_.back().value : Value::Undefined();
  elements_.pop_back();
  return value;
}

void Object::MakeFunction(const Node* parameters, const Node* body, Environment* closure,
                          bool arrow) {
  kind_ = Kind::Function;
  parameters_ = parameters;
  body_ = body;
  closure_ = closure;
  arrow_ = arrow;
}

void Object::MakeCompiled(const CompiledFunction* code, Environment* closure, bool arrow) {
  kind_ = Kind::Function;
  code_ = code;
  closure_ = closure;
  arrow_ = arrow;
}

Environment* Environment::Ancestor(std::uint32_t hops) {
  Environment* current = this;
  for (std::uint32_t i = 0; i < hops && current != nullptr; ++i) {
    current = current->parent_;
  }
  return current;
}

Value* Environment::Lookup(std::string_view name) {
  Environment* current = this;
  while (current != nullptr) {
    const auto found = current->index_.find(name);
    if (found != current->index_.end()) {
      return &current->slots_[found->second].value;
    }
    current = current->parent_;
  }
  return nullptr;
}

bool Environment::HasOwn(std::string_view name) const { return index_.count(name) != 0; }

bool Environment::Declare(std::string name, Value value, bool is_const) {
  const auto found = index_.find(name);
  if (found != index_.end()) {
    slots_[found->second] = Binding{std::move(value), is_const, true};
    return true;
  }
  slots_.push_back(Binding{std::move(value), is_const, true});
  index_.emplace(std::move(name), static_cast<std::uint32_t>(slots_.size() - 1));
  return true;
}

bool Environment::Assign(std::string_view name, const Value& value) {
  Environment* current = this;
  while (current != nullptr) {
    const auto found = current->index_.find(name);
    if (found != current->index_.end()) {
      Binding& binding = current->slots_[found->second];
      if (binding.is_const) {
        return false;
      }
      binding.value = value;
      return true;
    }
    current = current->parent_;
  }
  return false;
}

void Environment::Reserve(std::uint32_t count) {
  if (slots_.size() < count) {
    slots_.resize(count);
  }
}

Value* Environment::SlotValue(std::uint32_t index) {
  if (index >= slots_.size() || !slots_[index].live) {
    return nullptr;
  }
  return &slots_[index].value;
}

bool Environment::SlotIsConst(std::uint32_t index) const {
  return index < slots_.size() && slots_[index].is_const;
}

void Environment::DeclareSlot(std::uint32_t index, std::string name, Value value, bool is_const) {
  if (index >= slots_.size()) {
    slots_.resize(index + 1);
  }
  slots_[index] = Binding{std::move(value), is_const, true};
  // Registered only now. A name lookup from outside compiled code must not find
  // a binding whose `let` has not run yet -- it has to keep walking out to
  // whatever `a` meant before this block, which is what it would have found.
  index_.insert_or_assign(std::move(name), index);
}

void Environment::CopyBindingsFrom(const Environment& other) {
  slots_ = other.slots_;
  index_ = other.index_;
}

Heap::~Heap() = default;

void Heap::AttachRegExp(const Object* object, std::shared_ptr<const RegExp> pattern) {
  regexps_[object] = std::move(pattern);
}

const RegExp* Heap::FindRegExp(const Object* object) const {
  const auto found = regexps_.find(object);
  return found == regexps_.end() ? nullptr : found->second.get();
}

MapIndex* Heap::AttachMapIndex(const Object* object) {
  std::shared_ptr<MapIndex>& slot = map_indexes_[object];
  if (slot == nullptr) {
    slot = std::make_shared<MapIndex>();
  }
  return slot.get();
}

MapIndex* Heap::FindMapIndex(const Object* object) const {
  const auto found = map_indexes_.find(object);
  return found == map_indexes_.end() ? nullptr : found->second.get();
}

GeneratorState* Heap::AttachGenerator(const Object* object) {
  std::unique_ptr<GeneratorState>& slot = generators_[object];
  if (slot == nullptr) {
    slot = std::make_unique<GeneratorState>();
  }
  return slot.get();
}

GeneratorState* Heap::FindGenerator(const Object* object) const {
  const auto found = generators_.find(object);
  return found == generators_.end() ? nullptr : found->second.get();
}

void Heap::MakeWeakTable(const Object* table) { weak_tables_[table]; }

void Heap::WeakSet(const Object* table, const Object* key, Value value) {
  const auto found = weak_tables_.find(table);
  if (found == weak_tables_.end()) {
    return;  // not a WeakMap; the caller turns that into a TypeError
  }
  found->second[key] = std::move(value);
}

const Value* Heap::WeakGet(const Object* table, const Object* key) const {
  const auto table_entry = weak_tables_.find(table);
  if (table_entry == weak_tables_.end()) {
    return nullptr;
  }
  const auto found = table_entry->second.find(key);
  return found == table_entry->second.end() ? nullptr : &found->second;
}

bool Heap::WeakDelete(const Object* table, const Object* key) {
  const auto table_entry = weak_tables_.find(table);
  return table_entry != weak_tables_.end() && table_entry->second.erase(key) != 0;
}

Object* Heap::AllocateObject(Object::Kind kind) {
  if (AtLimit()) {
    return nullptr;
  }
  ++since_collection_;
  objects_.push_back(std::make_unique<Object>(kind));
  return objects_.back().get();
}

Environment* Heap::AllocateEnvironment(Environment* parent) {
  if (AtLimit()) {
    return nullptr;
  }
  ++since_collection_;
  environments_.push_back(std::make_unique<Environment>(parent));
  return environments_.back().get();
}

void Heap::MarkValue(const Value& value) {
  // A symbol's cell is collected like an object and lives in the same vector,
  // so it is marked the same way. Missing it here would free a symbol still
  // held in a variable.
  if ((value.type == ValueType::Object || value.type == ValueType::Symbol) &&
      value.object != nullptr) {
    Mark(value.object);
  }
}

void Heap::Mark(Object* object) {
  if (object == nullptr || object->marked_) {
    return;
  }
  object->marked_ = true;
  object_worklist_.push_back(object);
}

void Heap::Mark(Environment* environment) {
  if (environment == nullptr || environment->marked_) {
    return;
  }
  environment->marked_ = true;
  environment_worklist_.push_back(environment);
}

// Traces everything reachable from what is already marked.
//
// Iterative, not recursive: an object graph's depth is under the control of
// whoever wrote the page, and a recursive tracer would put the collector's
// stack depth there too.
//
// A method rather than a loop inside Collect because the ephemeron pass has to
// run it again: marking a WeakMap's value can reach a new object, which can
// make another table's key live.
void Heap::DrainWorklists() {
  while (!object_worklist_.empty() || !environment_worklist_.empty()) {
    while (!object_worklist_.empty()) {
      Object* object = object_worklist_.back();
      object_worklist_.pop_back();
      Mark(object->prototype_);
      Mark(object->closure_);
      Mark(object->home_object_);
      Mark(object->super_constructor_);
      MarkValue(object->bound_this_);
      for (const auto& entry : object->properties_) {
        // The key too: a symbol whose only reference is that it is a key here
        // is still reachable, and freeing it would leave the map holding a
        // pointer to a cell that could be reallocated as something else.
        Mark(const_cast<Object*>(entry.first.Cell()));
        MarkValue(entry.second.value);
        Mark(entry.second.getter);
        Mark(entry.second.setter);
      }
      for (const ArrayElement& element : object->elements_) {
        if (element.present) {
          MarkValue(element.value);
        }
      }
    }
    while (!environment_worklist_.empty()) {
      Environment* environment = environment_worklist_.back();
      environment_worklist_.pop_back();
      Mark(environment->parent_);
      for (const Binding& binding : environment->slots_) {
        // A reserved-but-unset slot holds a default-constructed value, which
        // marks as nothing. Tracing it anyway is cheaper than the branch.
        MarkValue(binding.value);
      }
    }
  }
}

// Whether a value is already reachable, which the ephemeron pass asks before
// marking so that it can tell when it has reached a fixpoint.
bool Heap::IsMarked(const Value& value) const {
  return (value.type != ValueType::Object && value.type != ValueType::Symbol) ||
         value.object == nullptr || value.object->marked_;
}

std::size_t Heap::Collect(const std::vector<Object*>& object_roots,
                          const std::vector<Environment*>& environment_roots) {
  for (const std::unique_ptr<Object>& object : objects_) {
    object->marked_ = false;
  }
  for (const std::unique_ptr<Environment>& environment : environments_) {
    environment->marked_ = false;
  }

  object_worklist_.clear();
  environment_worklist_.clear();
  for (Object* root : object_roots) {
    Mark(root);
  }
  for (Environment* root : environment_roots) {
    Mark(root);
  }

  DrainWorklists();

  // Ephemerons.
  //
  // A WeakMap's value is reachable only through its key, so marking cannot be
  // done in one pass: marking a value can make another table's key reachable,
  // which makes *that* entry's value reachable in turn. Iterate until nothing
  // new is marked -- bounded by the number of entries, since every round marks
  // at least one key or is the last.
  //
  // Doing this in the main mark loop instead would mark every value whether
  // its key was live or not, which is a WeakMap that leaks -- the one thing it
  // exists not to do.
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto& table : weak_tables_) {
      if (table.first->marked_) {
        for (const auto& entry : table.second) {
          if (entry.first->marked_ && !IsMarked(entry.second)) {
            MarkValue(entry.second);
            changed = true;
          }
        }
      }
    }
    // Marking a value can reach new objects, which can make another table's
    // key live.
    while (!object_worklist_.empty() || !environment_worklist_.empty()) {
      DrainWorklists();
      changed = true;
    }
  }

  const std::size_t before = objects_.size() + environments_.size();
  // Before the objects go, so the side table never holds a key that has been
  // freed -- a stale entry would be handed out as a compiled pattern the next
  // time an object happened to be allocated at the same address.
  if (!regexps_.empty() || !map_indexes_.empty() || !generators_.empty()) {
    for (const std::unique_ptr<Object>& object : objects_) {
      if (!object->marked_) {
        regexps_.erase(object.get());
        map_indexes_.erase(object.get());
        generators_.erase(object.get());
      }
    }
  }
  // The weak tables, separately, because a table has two things that can die
  // and only one of them is the table.
  //
  // A dead *entry* was the bug: dropping the table when its own object died
  // left every entry whose key had died still holding that key's address, and
  // the next collection's ephemeron pass reads `entry.first->marked_` on it.
  // That is a read of freed memory driven by a page, and it stayed invisible
  // for as long as collection only ran between top-level statements -- there
  // was rarely a *next* pass. The machine collects at every loop back edge, and
  // a `for` loop putting five thousand short-lived keys in a WeakMap found it
  // on the second one.
  for (auto table = weak_tables_.begin(); table != weak_tables_.end();) {
    if (!table->first->marked_) {
      table = weak_tables_.erase(table);
      continue;
    }
    std::unordered_map<const Object*, Value>& entries = table->second;
    for (auto entry = entries.begin(); entry != entries.end();) {
      entry = entry->first->marked_ ? std::next(entry) : entries.erase(entry);
    }
    ++table;
  }
  objects_.erase(std::remove_if(objects_.begin(), objects_.end(),
                                [](const std::unique_ptr<Object>& object) {
                                  return !object->marked_;
                                }),
                 objects_.end());
  environments_.erase(
      std::remove_if(environments_.begin(), environments_.end(),
                     [](const std::unique_ptr<Environment>& environment) {
                       return !environment->marked_;
                     }),
      environments_.end());

  since_collection_ = 0;
  return before - (objects_.size() + environments_.size());
}

}  // namespace microbrowser::js
