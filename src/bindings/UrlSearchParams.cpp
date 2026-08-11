// `URLSearchParams`, over the one urlencoded implementation in util.
//
// Its own translation unit because it is its own thing: a collection with no
// node in it, no document behind it, and nothing to do with the tree. It is
// here rather than in `src/js` because it is a web API rather than a language
// one -- a page gets it from the browser, and a script engine that shipped it
// would be shipping a browser feature.
//
// The pairs live in a JavaScript array on the instance rather than in a C++
// vector beside it. That is the same rule the wrapper cache and the interface
// table follow, and it is not a style preference: the collector cannot see a
// `js::Value` in a C++ field, so state kept there is state that gets freed
// while script still refers to it. This module has had that bug once.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "util/UrlEncoded.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where an instance keeps its pairs: an array of two-element arrays, in
// insertion order, because order is observable through `toString`, `forEach`
// and iteration.
constexpr const char* kPairsSlot = "#pairs";

Value PairsOf(const Value& self) {
  if (!self.IsObject()) {
    return Value::Undefined();
  }
  const Value* pairs = self.object->GetOwn(kPairsSlot);
  return pairs == nullptr ? Value::Undefined() : *pairs;
}

std::string PairPart(const Value& pair, std::size_t index) {
  if (!pair.IsObject() || pair.object->ElementCount() <= index) {
    return {};
  }
  return js::ToString(pair.object->GetElement(index));
}

// Rewrites the pair list, which every mutating method ends with. A fresh array
// rather than an in-place edit because `SetElements` is the only way to shrink
// one, and a half-updated list is observable from a getter that runs during it.
void SetPairs(const Value& self, js::Interpreter& interpreter, std::vector<Value> pairs) {
  if (!self.IsObject()) {
    return;
  }
  self.object->SetHidden(kPairsSlot, interpreter.NewArrayValue(std::move(pairs)));
}

std::vector<Value> ReadPairs(const Value& self) {
  std::vector<Value> out;
  const Value pairs = PairsOf(self);
  if (!pairs.IsObject()) {
    return out;
  }
  out.reserve(pairs.object->ElementCount());
  for (std::size_t i = 0; i < pairs.object->ElementCount(); ++i) {
    out.push_back(pairs.object->GetElement(i));
  }
  return out;
}

Value MakePair(js::Interpreter& interpreter, std::string name, std::string value) {
  return interpreter.NewArrayValue({Value::String(std::move(name)), Value::String(std::move(value))});
}

std::string SerializePairs(const Value& self) {
  std::vector<util::QueryPair> pairs;
  for (const Value& pair : ReadPairs(self)) {
    pairs.emplace_back(PairPart(pair, 0), PairPart(pair, 1));
  }
  return util::SerializeUrlEncoded(pairs);
}

}  // namespace

js::Value DomBindings::MakeUrlSearchParams(const std::string& search) {
  const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn("URLSearchParams")
                                                  : nullptr;
  const Value made = interpreter_->NewObjectValue();
  if (!made.IsObject()) {
    return Value::Undefined();
  }
  if (prototype != nullptr && prototype->IsObject()) {
    made.object->SetPrototype(prototype->object);
  }
  ResetUrlSearchParams(made, search);
  return made;
}

// Refills the list from a query string. Used when a setter on the owning URL moves the query out
// from under a params object a page is still holding: the two are one query, so the object has to
// change rather than be replaced -- a page that kept a reference would otherwise read a list that
// stopped tracking its URL.
void DomBindings::ResetUrlSearchParams(const js::Value& params, const std::string& search) {
  if (!params.IsObject()) {
    return;
  }
  std::string_view text = search;
  if (!text.empty() && text.front() == '?') {
    text.remove_prefix(1);
  }
  std::vector<Value> pairs;
  for (const util::QueryPair& pair : util::ParseUrlEncoded(std::string(text))) {
    pairs.push_back(MakePair(*interpreter_, pair.first, pair.second));
  }
  params.object->SetHidden(kPairsSlot, interpreter_->NewArrayValue(std::move(pairs)));
}

void DomBindings::InstallUrlSearchParams() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("URLSearchParams", prototype);

  // Every mutation ends here: if this list is the `searchParams` of a URL, that URL's query is
  // rewritten from it. The two objects are one query, and a page that appended a parameter and then
  // read `url.href` would otherwise get the URL from before its own write.
  const auto WriteBack = [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner != nullptr) {
      owner->WriteBackUrlSearchParams(call.self, SerializePairs(call.self));
    }
  };

  const auto method = [this, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->Set(name, native);
    }
  };

  method("get", [](NativeCall& call) {
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) == wanted) {
        return Value::String(PairPart(pair, 1));
      }
    }
    // Null rather than undefined for an absent name, which is what
    // `params.get('x') === null` tests for.
    return Value::Null();
  });
  method("getAll", [](NativeCall& call) {
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    std::vector<Value> found;
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) == wanted) {
        found.push_back(Value::String(PairPart(pair, 1)));
      }
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("has", [](NativeCall& call) {
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) == wanted) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  method("append", [WriteBack](NativeCall& call) {
    std::vector<Value> pairs = ReadPairs(call.self);
    pairs.push_back(MakePair(call.interpreter, js::ToString(Argument(call.arguments, 0)),
                             js::ToString(Argument(call.arguments, 1))));
    SetPairs(call.self, call.interpreter, std::move(pairs));
    WriteBack(call);
    return Value::Undefined();
  });
  method("set", [WriteBack](NativeCall& call) {
    // Replaces the *first* match in place and drops the rest, rather than
    // appending at the end. The distinction is observable through `toString`,
    // and a page that builds a URL by setting a parameter it already has
    // expects the order it wrote.
    const std::string name = js::ToString(Argument(call.arguments, 0));
    const std::string value = js::ToString(Argument(call.arguments, 1));
    std::vector<Value> kept;
    bool replaced = false;
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) != name) {
        kept.push_back(pair);
        continue;
      }
      if (!replaced) {
        kept.push_back(MakePair(call.interpreter, name, value));
        replaced = true;
      }
    }
    if (!replaced) {
      kept.push_back(MakePair(call.interpreter, name, value));
    }
    SetPairs(call.self, call.interpreter, std::move(kept));
    WriteBack(call);
    return Value::Undefined();
  });
  method("delete", [WriteBack](NativeCall& call) {
    const std::string name = js::ToString(Argument(call.arguments, 0));
    std::vector<Value> kept;
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) != name) {
        kept.push_back(pair);
      }
    }
    SetPairs(call.self, call.interpreter, std::move(kept));
    WriteBack(call);
    return Value::Undefined();
  });
  method("sort", [WriteBack](NativeCall& call) {
    // By name, stably, which is what the specification says: two values under
    // one name keep the order they were appended in.
    std::vector<Value> pairs = ReadPairs(call.self);
    std::stable_sort(pairs.begin(), pairs.end(), [](const Value& a, const Value& b) {
      return PairPart(a, 0) < PairPart(b, 0);
    });
    SetPairs(call.self, call.interpreter, std::move(pairs));
    WriteBack(call);
    return Value::Undefined();
  });
  method("forEach", [](NativeCall& call) {
    // `(value, name, params)`, in that order. Reversed arguments is the classic
    // way to get this wrong, and reddit's challenge writes
    // `.forEach((e, n) => …{name: n, value: e})` -- which produces a form with
    // its names and values swapped if the order is not this one.
    const Value callback = Argument(call.arguments, 0);
    if (!callback.IsObject() || !callback.object->IsCallable()) {
      return call.Throw("TypeError", "forEach needs a function");
    }
    const Value self_argument = Argument(call.arguments, 1);
    for (const Value& pair : ReadPairs(call.self)) {
      (void)call.interpreter.CallFunction(
          callback, self_argument,
          {Value::String(PairPart(pair, 1)), Value::String(PairPart(pair, 0)), call.self});
    }
    return Value::Undefined();
  });
  method("toString", [](NativeCall& call) {
    std::vector<util::QueryPair> pairs;
    for (const Value& pair : ReadPairs(call.self)) {
      pairs.emplace_back(PairPart(pair, 0), PairPart(pair, 1));
    }
    return Value::String(util::SerializeUrlEncoded(pairs));
  });

  // `keys()`, `values()` and `entries()` return arrays, which are iterable --
  // so `for (const k of params.keys())` and `[...params.entries()]` both work.
  // What they are not is the live, single-pass iterator the specification
  // defines, and the difference shows only in a program that mutates the
  // parameters while iterating them.
  const auto view = [&method](const char* name, int part) {
    method(name, [part](NativeCall& call) {
      std::vector<Value> out;
      for (const Value& pair : ReadPairs(call.self)) {
        if (part < 0) {
          out.push_back(call.interpreter.NewArrayValue(
              {Value::String(PairPart(pair, 0)), Value::String(PairPart(pair, 1))}));
        } else {
          out.push_back(Value::String(PairPart(pair, static_cast<std::size_t>(part))));
        }
      }
      return call.interpreter.NewArrayValue(std::move(out));
    });
  };
  view("keys", 0);
  view("values", 1);
  view("entries", -1);

  // `for (const [name, value] of params)`. The symbol comes from the
  // interpreter rather than from the global object, because a page can
  // reassign `Symbol.iterator` and the protocol must not follow it.
  const Value iterate = interpreter_->NewNativeValue("[Symbol.iterator]", [](NativeCall& call) {
    std::vector<Value> out;
    for (const Value& pair : ReadPairs(call.self)) {
      out.push_back(call.interpreter.NewArrayValue(
          {Value::String(PairPart(pair, 0)), Value::String(PairPart(pair, 1))}));
    }
    const Value entries = call.interpreter.NewArrayValue(std::move(out));
    if (!entries.IsObject()) {
      return Value::Undefined();
    }
    const js::Value* protocol =
        entries.object->Get(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
    if (protocol == nullptr) {
      return Value::Undefined();
    }
    const js::Result made = call.interpreter.CallFunction(*protocol, entries, {});
    return made.completion == js::Completion::Throw ? Value::Undefined() : made.value;
  });
  if (iterate.IsObject()) {
    prototype.object->Set(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), iterate);
  }

  const Value size = interpreter_->NewNativeValue("size", [](NativeCall& call) {
    return Value::Number(static_cast<double>(ReadPairs(call.self).size()));
  });
  if (size.IsObject()) {
    prototype.object->DefineAccessor("size", size.object, nullptr);
  }

  // The constructor. Returning the object is what makes it work under `new`:
  // the receiver a construct call builds is discarded in favour of what a
  // native returns.
  const Value constructor =
      interpreter_->NewNativeValue("URLSearchParams", [prototype](NativeCall& call) {
        const Value made = call.interpreter.NewObjectValue();
        if (!made.IsObject()) {
          return Value::Undefined();
        }
        if (prototype.IsObject()) {
          made.object->SetPrototype(prototype.object);
        }
        std::vector<Value> pairs;
        const Value init = Argument(call.arguments, 0);
        if (init.IsObject() && init.object->GetOwn(kPairsSlot) != nullptr) {
          // Copied, not shared: `new URLSearchParams(other)` is a snapshot,
          // and aliasing the array would make a write to one show up in both.
          pairs = ReadPairs(init);
          for (Value& pair : pairs) {
            pair = MakePair(call.interpreter, PairPart(pair, 0), PairPart(pair, 1));
          }
        } else if (init.IsObject() && init.object->ElementCount() > 0) {
          // A sequence of two-element sequences, which is what
          // `Object.entries(x)` produces and what a page passes most often
          // after a string.
          for (std::size_t i = 0; i < init.object->ElementCount(); ++i) {
            const Value entry = init.object->GetElement(i);
            pairs.push_back(MakePair(call.interpreter, PairPart(entry, 0), PairPart(entry, 1)));
          }
        } else if (init.IsObject()) {
          for (const std::string& key : init.object->EnumerableKeys()) {
            const Value* value = init.object->Get(key);
            pairs.push_back(MakePair(call.interpreter, key,
                                     value == nullptr ? std::string() : js::ToString(*value)));
          }
        } else if (init.type != js::ValueType::Undefined && init.type != js::ValueType::Null) {
          // A single leading "?" is removed, so `new URLSearchParams(location.search)` and
          // `new URLSearchParams(location.search.slice(1))` are the same list.
          std::string text = js::ToString(init);
          if (!text.empty() && text.front() == '?') {
            text.erase(0, 1);
          }
          for (const util::QueryPair& pair : util::ParseUrlEncoded(text)) {
            pairs.push_back(MakePair(call.interpreter, pair.first, pair.second));
          }
        }
        made.object->SetHidden(kPairsSlot, call.interpreter.NewArrayValue(std::move(pairs)));
        return made;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->SetHidden("constructor", constructor);
    interpreter_->Global()->Set("URLSearchParams", constructor);
    interpreter_->GlobalScope()->Declare("URLSearchParams", constructor, false);
  }
}

}  // namespace microbrowser::bindings
