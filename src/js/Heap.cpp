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
  if (frozen_) {
    return;
  }
  const auto found = properties_.find(key);
  if (found != properties_.end()) {
    found->second.value = std::move(value);
    found->second.getter = nullptr;
    found->second.setter = nullptr;
    return;
  }
  if (!key.IsSymbol()) {
    key_order_.push_back(key.Text());
  }
  properties_.emplace(std::move(key), Property{std::move(value), nullptr, nullptr});
}

void Object::DefineAccessor(PropertyKey key, Object* getter, Object* setter) {
  if (frozen_) {
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
    key_order_.push_back(key.Text());
  }
  properties_.emplace(std::move(key), Property{Value::Undefined(), getter, setter});
}

bool Object::Delete(const PropertyKey& key) {
  if (frozen_) {
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
  if (frozen_) {
    return;
  }
  if (index >= elements_.size()) {
    elements_.resize(index + 1);
  }
  elements_[index] = ArrayElement{std::move(value), true};
}

void Object::PushElement(Value value) {
  if (frozen_) {
    return;
  }
  elements_.push_back(ArrayElement{std::move(value), true});
}

Value Object::PopElement() {
  if (frozen_ || elements_.empty()) {
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

Value* Environment::Lookup(std::string_view name) {
  Environment* current = this;
  while (current != nullptr) {
    const auto found = current->bindings_.find(std::string(name));
    if (found != current->bindings_.end()) {
      return &found->second.value;
    }
    current = current->parent_;
  }
  return nullptr;
}

bool Environment::Declare(std::string name, Value value, bool is_const) {
  bindings_[std::move(name)] = Binding{std::move(value), is_const};
  return true;
}

bool Environment::Assign(std::string_view name, const Value& value) {
  Environment* current = this;
  while (current != nullptr) {
    const auto found = current->bindings_.find(std::string(name));
    if (found != current->bindings_.end()) {
      if (found->second.is_const) {
        return false;
      }
      found->second.value = value;
      return true;
    }
    current = current->parent_;
  }
  return false;
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

  // Iterative, not recursive: an object graph's depth is under the control of
  // whoever wrote the page, and a recursive tracer would put the collector's
  // stack depth there too.
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
      for (const auto& binding : environment->bindings_) {
        MarkValue(binding.second.value);
      }
    }
  }

  const std::size_t before = objects_.size() + environments_.size();
  // Before the objects go, so the side table never holds a key that has been
  // freed -- a stale entry would be handed out as a compiled pattern the next
  // time an object happened to be allocated at the same address.
  if (!regexps_.empty() || !map_indexes_.empty()) {
    for (const std::unique_ptr<Object>& object : objects_) {
      if (!object->marked_) {
        regexps_.erase(object.get());
        map_indexes_.erase(object.get());
      }
    }
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
