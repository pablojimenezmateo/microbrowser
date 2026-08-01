#include <utility>

#include "js/Interpreter.h"

namespace microbrowser::js {

// Class evaluation.
//
// A class is a function with a populated prototype, and saying so in one place
// is what keeps `new Foo()` from needing to know which of the two forms it was
// given. What classes add on top of that is ordering: fields initialize after
// a super() call and before the constructor body, and getting that wrong shows
// up as a field that is undefined in the constructor of a derived class.
Result Interpreter::EvaluateClass(const Node& node, Environment& scope) {
  Object* superclass = nullptr;
  if (node.Child(0) != nullptr) {
    const Result parent = Evaluate(*node.Child(0), scope);
    if (parent.IsAbrupt()) {
      return parent;
    }
    if (!parent.value.IsObject() || !parent.value.object->IsCallable()) {
      return Throw("TypeError", "class can only extend a constructor or null");
    }
    superclass = parent.value.object;
  }

  Object* prototype = NewObject();
  if (prototype == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  if (superclass != nullptr) {
    // The instance chain. Without this, a method on the base class is
    // invisible to an instance of the derived one.
    const Value* parent_prototype = superclass->Get("prototype");
    prototype->SetPrototype(parent_prototype != nullptr && parent_prototype->IsObject()
                                ? parent_prototype->object
                                : object_prototype_);
  }

  // The scope the methods close over holds the class binding, so a method can
  // name its own class -- `static create(){ return new Counter() }` works even
  // though the outer binding is not created until this returns.
  Environment* class_scope = heap_.AllocateEnvironment(&scope);
  if (class_scope == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  const ScopeGuard guard(*this, class_scope);

  // Find the constructor first: it becomes the class object itself, so
  // everything else is attached to it.
  const Node* constructor_body = nullptr;
  const Node* constructor_parameters = nullptr;
  for (const NodePtr& member : node.children) {
    if (member == nullptr || member->kind != NodeKind::MethodDefinition) {
      continue;
    }
    const auto flags = static_cast<std::uint8_t>(member->number);
    if (member->string == "constructor" && (flags & kMethodStatic) == 0) {
      const Node* function = member->Child(0);
      if (function != nullptr && function->kind == NodeKind::FunctionExpression) {
        constructor_parameters = function->Child(0);
        constructor_body = function->Child(1);
      }
    }
  }

  Object* constructor = heap_.AllocateObject(Object::Kind::Function);
  if (constructor == nullptr) {
    return Throw("RangeError", "out of memory");
  }
  constructor->SetPrototype(superclass != nullptr ? superclass : function_prototype_);
  constructor->MakeFunction(constructor_parameters, constructor_body, class_scope, false);
  constructor->Set("name", Value::String(node.string));
  constructor->Set("prototype", Value::Obj(prototype));
  constructor->SetHomeObject(prototype);
  constructor->SetSuperConstructor(superclass);
  prototype->Set("constructor", Value::Obj(constructor));

  const Value class_value = Value::Obj(constructor);
  if (!node.string.empty()) {
    class_scope->Declare(node.string, class_value, true);
  }

  for (const NodePtr& member : node.children) {
    if (member == nullptr || member->kind != NodeKind::MethodDefinition) {
      continue;
    }
    const auto flags = static_cast<std::uint8_t>(member->number);
    const bool is_static = (flags & kMethodStatic) != 0;
    Object* target = is_static ? constructor : prototype;

    std::string name = member->string;
    if ((flags & kMethodComputed) != 0 && member->Child(0) != nullptr) {
      const Result computed = Evaluate(*member->Child(0), *class_scope);
      if (computed.IsAbrupt()) {
        return computed;
      }
      name = ToString(computed.value);
    }

    const Node* function_node = member->children.empty()
                                    ? nullptr
                                    : member->children.back().get();
    const bool is_method =
        function_node != nullptr && function_node->kind == NodeKind::FunctionExpression;

    if (!is_method) {
      // A field. Instance fields are recorded on the constructor and run per
      // instance; static ones are evaluated now, against the class.
      if (is_static) {
        Value value;
        if (function_node != nullptr) {
          const Result initializer = Evaluate(*function_node, *class_scope);
          if (initializer.IsAbrupt()) {
            return initializer;
          }
          value = initializer.value;
        }
        target->Set(std::move(name), value);
      } else {
        constructor->AddInstanceField(std::move(name), function_node);
      }
      continue;
    }

    if (name == "constructor" && !is_static) {
      continue;  // already the class object
    }

    Value method = NewFunction(*function_node, *class_scope, false);
    if (!method.IsObject()) {
      return Throw("RangeError", "out of memory");
    }
    method.object->Set("name", Value::String(name));
    // The home object is what `super.x` resolves against: the *defining*
    // object's prototype, not the receiver's. Without it, a method that calls
    // super in a three-level hierarchy recurses into itself.
    method.object->SetHomeObject(target);

    if ((flags & kMethodGetter) != 0) {
      target->DefineAccessor(std::move(name), method.object, nullptr);
    } else if ((flags & kMethodSetter) != 0) {
      target->DefineAccessor(std::move(name), nullptr, method.object);
    } else {
      target->Set(std::move(name), method);
    }
  }

  return Result::Normal(class_value);
}

Result Interpreter::InitializeFields(Object* instance, Object* constructor) {
  if (instance == nullptr || constructor == nullptr) {
    return Result::Normal();
  }
  for (const auto& field : constructor->InstanceFields()) {
    Value value;
    if (field.second != nullptr) {
      Environment* field_scope = heap_.AllocateEnvironment(constructor->Closure());
      if (field_scope == nullptr) {
        return Throw("RangeError", "out of memory");
      }
      const ScopeGuard guard(*this, field_scope);
      // A field initializer sees `this`, which is how `#count = 0` and
      // `total = this.a + this.b` both work.
      field_scope->Declare("this", Value::Obj(instance), true);
      const Result initializer = Evaluate(*field.second, *field_scope);
      if (initializer.IsAbrupt()) {
        return initializer;
      }
      value = initializer.value;
    }
    instance->Set(field.first, value);
  }
  return Result::Normal();
}

}  // namespace microbrowser::js
