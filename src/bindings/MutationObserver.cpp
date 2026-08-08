// `MutationObserver` — what a framework watches the tree with.
//
// The shape is the one the specification chose and it is worth saying why,
// because the obvious design is different. An observer does *not* call back
// per mutation. Mutations accumulate into a queue, and the queue is delivered
// once, as a microtask, after whatever ran finishes. A page that appends a
// thousand rows in a loop gets one callback holding a thousand records rather
// than a thousand callbacks -- which is the difference between a framework
// that works and one that spends its time in observers.
//
// That also makes it free against the zero-idle-CPU invariant, and for the
// same reason the promise queue was: a microtask exists only because something
// already ran, so the drain rides a wakeup that was already happening. No
// timer, no poll, and a page that mutates nothing schedules nothing.
//
// State lives in JavaScript objects hung off the interfaces object rather than
// in C++ members, because a `js::Value` in a C++ field is invisible to the
// collector -- a callback it cannot see is one it frees while an observer
// still points at it. This module has had that bug once already.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// The list of live observers, on the interfaces object.
constexpr const char* kObserversSlot = "#observers";
// On an observer: the callback, what it watches, and what it has queued.
constexpr const char* kCallbackSlot = "#callback";
constexpr const char* kTargetsSlot = "#targets";
constexpr const char* kRecordsSlot = "#records";
// Set while a delivery is already scheduled, so a hundred mutations in one
// turn queue one microtask rather than a hundred.
constexpr const char* kScheduledSlot = "#scheduled";

bool OptionIsTrue(const Value& options, const char* key) {
  if (!options.IsObject()) {
    return false;
  }
  const Value* found = options.object->Get(key);
  return found != nullptr && js::ToBoolean(*found);
}

// Whether a mutation at `node` is one that `target` was asked to watch:
// `target` itself always, and anything under it when `subtree` was set.
bool Observes(const dom::Node* node, const dom::Node* target, bool subtree) {
  if (node == target) {
    return true;
  }
  if (!subtree) {
    return false;
  }
  for (const dom::Node* at = node; at != nullptr; at = at->Parent()) {
    if (at == target) {
      return true;
    }
  }
  return false;
}

}  // namespace

js::Value DomBindings::ObserverList() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kObserversSlot)) {
    return *existing;
  }
  const Value list = interpreter_->NewArrayValue({});
  if (list.IsObject()) {
    interfaces_.object->Set(kObserversSlot, list);
  }
  return list;
}

void DomBindings::ScheduleObserverDelivery(const js::Value& observer) {
  if (!observer.IsObject()) {
    return;
  }
  const Value* scheduled = observer.object->GetOwn(kScheduledSlot);
  if (scheduled != nullptr && js::ToBoolean(*scheduled)) {
    return;  // already owed a delivery; the records simply join the queue
  }
  observer.object->Set(kScheduledSlot, Value::Bool(true));

  // A native that takes the queue and hands it to the callback. It carries the
  // observer as a *property* rather than in a capture, for the reason at the
  // top of this file.
  const Value job = interpreter_->NewNativeValue("deliverMutations", [](NativeCall& call) {
    const Value* watcher = call.callee == nullptr ? nullptr : call.callee->GetOwn("#observer");
    if (watcher == nullptr || !watcher->IsObject()) {
      return Value::Undefined();
    }
    watcher->object->Set(kScheduledSlot, Value::Bool(false));
    const Value* records = watcher->object->GetOwn(kRecordsSlot);
    const Value* callback = watcher->object->GetOwn(kCallbackSlot);
    if (records == nullptr || !records->IsObject() || callback == nullptr ||
        !callback->IsObject() || !callback->object->IsCallable()) {
      return Value::Undefined();
    }
    if (records->object->ElementCount() == 0) {
      // Everything was taken by takeRecords() before the queue drained. The
      // callback is not called with nothing, which is the rule and is what
      // stops a framework from doing a pass over an empty list.
      return Value::Undefined();
    }
    std::vector<Value> taken;
    for (std::size_t i = 0; i < records->object->ElementCount(); ++i) {
      taken.push_back(records->object->GetElement(i));
    }
    records->object->SetElements({}, {});
    // (records, observer), which is the signature every page writes against.
    // TD-0018: Polymer's ASAP observer runs here after kevlar spends the budget.
    call.interpreter.BeginTask();
    const js::Result ran = call.interpreter.CallFunction(
        *callback, Value::Undefined(),
        {call.interpreter.NewArrayValue(std::move(taken)), *watcher});
    if (ran.completion == js::Completion::Throw) {
      call.interpreter.ReportUncaught(ran.value, "MutationObserver callback");
    }
    return Value::Undefined();
  });
  if (!job.IsObject()) {
    return;
  }
  job.object->Set("#observer", observer);
  js::Interpreter::Microtask task;
  task.callee = job;
  interpreter_->EnqueueMicrotask(std::move(task));
}

void DomBindings::RecordMutation(dom::Node& node, const char* type, const std::string& name,
                                 const js::Value& old_value,
                                 const std::vector<dom::Node*>& added,
                                 const std::vector<dom::Node*>& removed) {
  const Value observers = ObserverList();
  if (!observers.IsObject() || observers.object->ElementCount() == 0) {
    return;  // nothing is watching, and this is the common case by a long way
  }
  for (std::size_t i = 0; i < observers.object->ElementCount(); ++i) {
    const Value observer = observers.object->GetElement(i);
    if (!observer.IsObject()) {
      continue;
    }
    const Value* targets = observer.object->GetOwn(kTargetsSlot);
    if (targets == nullptr || !targets->IsObject()) {
      continue;
    }
    for (std::size_t t = 0; t < targets->object->ElementCount(); ++t) {
      const Value registration = targets->object->GetElement(t);
      if (!registration.IsObject()) {
        continue;
      }
      const Value* watched = registration.object->GetOwn("node");
      dom::Node* target = watched == nullptr ? nullptr : NodeOf(*watched);
      if (target == nullptr ||
          !Observes(&node, target, OptionIsTrue(registration, "subtree"))) {
        continue;
      }
      // The kind has to have been asked for. An observer watching attributes
      // must not be handed childList records.
      if (!OptionIsTrue(registration, type)) {
        continue;
      }
      if (std::string(type) == "attributes") {
        // `attributeFilter`, when given, narrows to named attributes.
        const Value* filter = registration.object->GetOwn("attributeFilter");
        if (filter != nullptr && filter->IsObject()) {
          bool named = false;
          for (std::size_t f = 0; f < filter->object->ElementCount(); ++f) {
            named = named || js::ToString(filter->object->GetElement(f)) == name;
          }
          if (!named) {
            continue;
          }
        }
      }

      const Value record = interpreter_->NewObjectValue();
      if (!record.IsObject()) {
        continue;
      }
      record.object->Set("type", Value::String(std::string(type)));
      record.object->Set("target", WrapperFor(&node));
      record.object->Set("attributeName",
                         name.empty() ? Value::Null() : Value::String(name));
      // The old value only when it was asked for, because keeping it otherwise
      // is a copy of every attribute a page writes.
      const bool wants_old = OptionIsTrue(registration, "attributeOldValue") ||
                             OptionIsTrue(registration, "characterDataOldValue");
      record.object->Set("oldValue", wants_old ? old_value : Value::Null());
      std::vector<Value> added_values;
      for (dom::Node* each : added) {
        added_values.push_back(WrapperFor(each));
      }
      std::vector<Value> removed_values;
      for (dom::Node* each : removed) {
        removed_values.push_back(WrapperFor(each));
      }
      record.object->Set("addedNodes", interpreter_->NewArrayValue(std::move(added_values)));
      record.object->Set("removedNodes", interpreter_->NewArrayValue(std::move(removed_values)));

      const Value* records = observer.object->GetOwn(kRecordsSlot);
      if (records != nullptr && records->IsObject()) {
        records->object->PushElement(record);
      }
      ScheduleObserverDelivery(observer);
      break;  // one record per observer per mutation, however many
              // registrations of it match
    }
  }
}

void DomBindings::InstallMutationObserver() {
  DomBindings* self = this;
  const Value constructor =
      interpreter_->NewNativeValue("MutationObserver", [self](NativeCall& call) {
        const Value callback = Argument(call.arguments, 0);
        if (!callback.IsObject() || !callback.object->IsCallable()) {
          return call.Throw("TypeError", "MutationObserver needs a callback");
        }
        const Value observer = call.interpreter.NewObjectValue();
        if (!observer.IsObject()) {
          return Value::Undefined();
        }
        observer.object->Set(kCallbackSlot, callback);
        observer.object->Set(kTargetsSlot, call.interpreter.NewArrayValue({}));
        observer.object->Set(kRecordsSlot, call.interpreter.NewArrayValue({}));
        observer.object->Set(kScheduledSlot, Value::Bool(false));

        const auto method = [&](const char* name, js::NativeFunction function) {
          const Value native = call.interpreter.NewNativeValue(name, std::move(function));
          if (native.IsObject()) {
            native.object->Set(kOwnerSlot, PointerValue(self));
            observer.object->Set(name, native);
          }
        };

        method("observe", [](NativeCall& inner) {
          if (!inner.self.IsObject()) {
            return Value::Undefined();
          }
          const Value target = Argument(inner.arguments, 0);
          if (NodeOf(target) == nullptr) {
            return inner.Throw("TypeError", "observe needs a node");
          }
          const Value options = Argument(inner.arguments, 1);
          const Value registration = inner.interpreter.NewObjectValue();
          if (!registration.IsObject()) {
            return Value::Undefined();
          }
          registration.object->Set("node", target);
          // Copied out rather than kept as a reference to the page's object,
          // which it is free to mutate afterwards -- the options are read at
          // `observe` time and a later edit must not change what is watched.
          for (const char* key : {"childList", "attributes", "characterData", "subtree",
                                  "attributeOldValue", "characterDataOldValue"}) {
            registration.object->Set(key, Value::Bool(OptionIsTrue(options, key)));
          }
          if (options.IsObject()) {
            if (const Value* filter = options.object->Get("attributeFilter")) {
              registration.object->Set("attributeFilter", *filter);
              // Naming a filter implies watching attributes, which is what the
              // specification says and what a page that only sets the filter
              // expects.
              registration.object->Set("attributes", Value::Bool(true));
            }
          }
          const Value* targets = inner.self.object->GetOwn(kTargetsSlot);
          if (targets != nullptr && targets->IsObject()) {
            targets->object->PushElement(registration);
          }
          return Value::Undefined();
        });

        method("disconnect", [](NativeCall& inner) {
          if (!inner.self.IsObject()) {
            return Value::Undefined();
          }
          // Both halves: it watches nothing, and anything already queued is
          // dropped. A disconnected observer that still fired once more would
          // be the worst of both.
          const Value* targets = inner.self.object->GetOwn(kTargetsSlot);
          if (targets != nullptr && targets->IsObject()) {
            targets->object->SetElements({}, {});
          }
          const Value* records = inner.self.object->GetOwn(kRecordsSlot);
          if (records != nullptr && records->IsObject()) {
            records->object->SetElements({}, {});
          }
          return Value::Undefined();
        });

        method("takeRecords", [](NativeCall& inner) {
          if (!inner.self.IsObject()) {
            return inner.interpreter.NewArrayValue({});
          }
          const Value* records = inner.self.object->GetOwn(kRecordsSlot);
          if (records == nullptr || !records->IsObject()) {
            return inner.interpreter.NewArrayValue({});
          }
          std::vector<Value> taken;
          for (std::size_t i = 0; i < records->object->ElementCount(); ++i) {
            taken.push_back(records->object->GetElement(i));
          }
          records->object->SetElements({}, {});
          return inner.interpreter.NewArrayValue(std::move(taken));
        });

        const Value list = self->ObserverList();
        if (list.IsObject()) {
          list.object->PushElement(observer);
        }
        return observer;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    interpreter_->Global()->Set("MutationObserver", constructor);
    interpreter_->GlobalScope()->Declare("MutationObserver", constructor, false);
  }
}

}  // namespace microbrowser::bindings
