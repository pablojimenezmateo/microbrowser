#include "js/Heap.h"

#include <algorithm>
#include <utility>

namespace microbrowser::js {

namespace {

// How far a prototype chain may be walked. A cycle is buildable from a page
// (`a.__proto__ = b; b.__proto__ = a`), so the walk is bounded rather than
// trusted -- an unbounded one is a hang, not a wrong answer.
constexpr int kMaxPrototypeDepth = 1000;

}  // namespace

const Value* Object::GetOwn(std::string_view key) const {
  const auto found = properties_.find(std::string(key));
  return found == properties_.end() ? nullptr : &found->second;
}

const Value* Object::Get(std::string_view key) const {
  const Object* current = this;
  for (int depth = 0; current != nullptr && depth < kMaxPrototypeDepth; ++depth) {
    if (const Value* value = current->GetOwn(key)) {
      return value;
    }
    current = current->prototype_;
  }
  return nullptr;
}

void Object::Set(std::string key, Value value) {
  const auto found = properties_.find(key);
  if (found != properties_.end()) {
    found->second = std::move(value);
    return;
  }
  key_order_.push_back(key);
  properties_.emplace(std::move(key), std::move(value));
}

bool Object::Delete(std::string_view key) {
  const auto found = properties_.find(std::string(key));
  if (found == properties_.end()) {
    return false;
  }
  properties_.erase(found);
  key_order_.erase(std::remove(key_order_.begin(), key_order_.end(), std::string(key)),
                   key_order_.end());
  return true;
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
  if (value.type == ValueType::Object && value.object != nullptr) {
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
      MarkValue(object->bound_this_);
      for (const auto& property : object->properties_) {
        MarkValue(property.second);
      }
      for (const Value& element : object->elements_) {
        MarkValue(element);
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
