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
#include "util/StringUtil.h"
#include "util/UrlEncoded.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where an instance keeps its pairs: an array of two-element arrays, in
// insertion order, because order is observable through `toString`, `forEach`
// and iteration.
constexpr const char* kPairsSlot = "#pairs";

// A live iterator's two pieces of state: which list it walks and how far it has got. On the
// iterator rather than in C++ fields, for the reason the pairs are: the collector cannot see either.
constexpr const char* kIteratorTargetSlot = "#iterTarget";
constexpr const char* kIteratorIndexSlot = "#iterIndex";

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

// Web IDL USVString: a lone surrogate has no encoding, and a name or a value here becomes bytes in
// a query string. `js::ToString` rather than `CoerceToUsvString` because these arguments are
// already past their conversion -- this is the scrub, not the coercion.
std::string UsvOf(NativeCall& call, const Value& value) {
  std::string out;
  if (!CoerceToUsvString(call, value, out)) {
    return {};
  }
  return out;
}

std::string SerializePairs(const Value& self) {
  std::vector<util::QueryPair> pairs;
  for (const Value& pair : ReadPairs(self)) {
    pairs.emplace_back(PairPart(pair, 0), PairPart(pair, 1));
  }
  return util::SerializeUrlEncoded(pairs);
}

}  // namespace

js::Value DomBindings::MakeUrlSearchParams(const std::string& query) {
  const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn("URLSearchParams")
                                                  : nullptr;
  const Value made = interpreter_->NewObjectValue();
  if (!made.IsObject()) {
    return Value::Undefined();
  }
  if (prototype != nullptr && prototype->IsObject()) {
    made.object->SetPrototype(prototype->object);
  }
  ResetUrlSearchParams(made, query);
  return made;
}

// Refills the list from a query string. Used when a setter on the owning URL moves the query out
// from under a params object a page is still holding: the two are one query, so the object has to
// change rather than be replaced -- a page that kept a reference would otherwise read a list that
// stopped tracking its URL.
void DomBindings::ResetUrlSearchParams(const js::Value& params, const std::string& query) {
  if (!params.IsObject()) {
    return;
  }
  std::vector<Value> pairs;
  for (const util::QueryPair& pair : util::ParseUrlEncoded(query)) {
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
    const std::string wanted = UsvOf(call, Argument(call.arguments, 0));
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
    const std::string wanted = UsvOf(call, Argument(call.arguments, 0));
    std::vector<Value> found;
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) == wanted) {
        found.push_back(Value::String(PairPart(pair, 1)));
      }
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("has", [](NativeCall& call) {
    // Two arguments means "this name with *this* value", and an explicit `undefined` is *not* one:
    // `has(name, undefined)` is the one-argument query. That is the standard's own rule, and the
    // reason for it is that `has(name, map.get(k))` written by a program whose lookup missed must
    // not silently become a stricter question than the one it meant to ask.
    const std::string wanted = UsvOf(call, Argument(call.arguments, 0));
    const bool by_value = !Argument(call.arguments, 1).IsUndefined();
    const std::string value = by_value ? UsvOf(call, Argument(call.arguments, 1)) : std::string();
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) == wanted && (!by_value || PairPart(pair, 1) == value)) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  method("append", [WriteBack](NativeCall& call) {
    std::vector<Value> pairs = ReadPairs(call.self);
    pairs.push_back(MakePair(call.interpreter, UsvOf(call, Argument(call.arguments, 0)),
                             UsvOf(call, Argument(call.arguments, 1))));
    SetPairs(call.self, call.interpreter, std::move(pairs));
    WriteBack(call);
    return Value::Undefined();
  });
  method("set", [WriteBack](NativeCall& call) {
    // Replaces the *first* match in place and drops the rest, rather than
    // appending at the end. The distinction is observable through `toString`,
    // and a page that builds a URL by setting a parameter it already has
    // expects the order it wrote.
    const std::string name = UsvOf(call, Argument(call.arguments, 0));
    const std::string value = UsvOf(call, Argument(call.arguments, 1));
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
    // Two arguments removes only the pairs with that name *and* that value. See `has` above for
    // why an explicit `undefined` is not one.
    const std::string name = UsvOf(call, Argument(call.arguments, 0));
    const bool by_value = !Argument(call.arguments, 1).IsUndefined();
    const std::string value = by_value ? UsvOf(call, Argument(call.arguments, 1)) : std::string();
    std::vector<Value> kept;
    for (const Value& pair : ReadPairs(call.self)) {
      if (PairPart(pair, 0) != name || (by_value && PairPart(pair, 1) != value)) {
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
      // By UTF-16 code unit, which is what a page's own `sort` would do and is *not* what
      // comparing the UTF-8 bytes does: a rainbow (U+1F308) sorts before U+FB03 in one order and
      // after it in the other.
      return util::CompareUtf16(PairPart(a, 0), PairPart(b, 0)) < 0;
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
    // By index against the *current* list rather than over a snapshot: a callback that deletes the
    // parameter it was handed is defined to see the one that moved into its place next.
    for (std::size_t i = 0;; ++i) {
      const std::vector<Value> pairs = ReadPairs(call.self);
      if (i >= pairs.size()) {
        break;
      }
      (void)call.interpreter.CallFunction(
          callback, self_argument,
          {Value::String(PairPart(pairs[i], 1)), Value::String(PairPart(pairs[i], 0)), call.self});
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

  // `entries()`, `keys()`, `values()` and `for (… of params)` all produce the **live** iterator the
  // specification defines: an index into the pair list, re-read on every `next()`. An array
  // snapshot is the obvious implementation and it is observably wrong -- a page that deletes the
  // parameter it is looking at is defined to see the one that moved into its place, and with a
  // snapshot it sees the deleted one and then runs off the end.
  const auto view = [this, &method](const char* name, int part) {
    method(name, [this, part](NativeCall& call) -> Value {
      const Value iterator = call.interpreter.NewObjectValue();
      if (!iterator.IsObject()) {
        return Value::Undefined();
      }
      iterator.object->SetHidden(kIteratorTargetSlot, call.self);
      iterator.object->SetHidden(kIteratorIndexSlot, Value::Number(0));
      const Value next = interpreter_->NewNativeValue("next", [part](NativeCall& step) -> Value {
        const Value result = step.interpreter.NewObjectValue();
        if (!result.IsObject() || !step.self.IsObject()) {
          return Value::Undefined();
        }
        const Value* target = step.self.object->GetOwn(kIteratorTargetSlot);
        const Value* index = step.self.object->GetOwn(kIteratorIndexSlot);
        const auto at = index == nullptr ? std::size_t{0}
                                         : static_cast<std::size_t>(js::ToNumber(*index));
        const std::vector<Value> pairs =
            target == nullptr ? std::vector<Value>() : ReadPairs(*target);
        if (at >= pairs.size()) {
          result.object->Set("done", Value::Bool(true));
          result.object->Set("value", Value::Undefined());
          return result;
        }
        step.self.object->SetHidden(kIteratorIndexSlot,
                                    Value::Number(static_cast<double>(at + 1)));
        result.object->Set("done", Value::Bool(false));
        if (part < 0) {
          result.object->Set("value", step.interpreter.NewArrayValue(
                                          {Value::String(PairPart(pairs[at], 0)),
                                           Value::String(PairPart(pairs[at], 1))}));
        } else {
          result.object->Set(
              "value", Value::String(PairPart(pairs[at], static_cast<std::size_t>(part))));
        }
        return result;
      });
      if (next.IsObject()) {
        iterator.object->Set("next", next);
      }
      // Iterable in its own right, so `[...params.entries()]` works: the iterator returns itself.
      const Value self_iterator =
          call.interpreter.NewNativeValue("[Symbol.iterator]", [](NativeCall& inner) {
            return inner.self;
          });
      if (self_iterator.IsObject()) {
        iterator.object->Set(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()),
                             self_iterator);
      }
      return iterator;
    });
  };
  view("keys", 0);
  view("values", 1);
  view("entries", -1);

  // `for (const [name, value] of params)` is `entries`, and literally the same function object --
  // which is what the specification says, so a page that replaces one has replaced both.
  if (const Value* entries = prototype.object->Get("entries"); entries != nullptr) {
    prototype.object->Set(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), *entries);
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
        // The Web IDL union `(sequence<sequence<USVString>> or record<USVString, USVString> or
        // USVString)`, resolved the way the standard resolves it: an object with a callable
        // `@@iterator` is a sequence, any other object is a record, and anything else is a string.
        // Asking `ElementCount()` instead -- which is what this did -- means a `FormData` or a
        // `Map`, both of which are iterable and neither of which has indices, silently became an
        // empty list.
        std::vector<Value> pairs;
        const Value init = Argument(call.arguments, 0);
        bool iterable = false;
        if (init.IsObject()) {
          const Value* iterator_method =
              init.object->Get(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
          iterable = iterator_method != nullptr && iterator_method->IsObject() &&
                     iterator_method->object->IsCallable();
        }
        if (iterable) {
          std::vector<Value> entries;
          if (!IterateValue(call, init, entries)) {
            return call.ThrownValue();
          }
          for (const Value& entry : entries) {
            std::vector<Value> parts;
            if (!IterateValue(call, entry, parts)) {
              return call.ThrownValue();
            }
            if (parts.size() != 2) {
              // The standard says exactly two, and says it as a TypeError rather than by padding:
              // a one-element inner sequence is a program that meant something else.
              return call.Throw("TypeError",
                                "each element of a URLSearchParams sequence must have two members");
            }
            pairs.push_back(MakePair(call.interpreter, UsvOf(call, parts[0]),
                                     UsvOf(call, parts[1])));
          }
        } else if (init.IsObject()) {
          // A `record` is a *map*, so two keys that scrub to the same USVString are one entry: the
          // later value wins and the earlier position is kept. `{"\uD835x": "1", "xx": "2",
          // "\uD83Dx": "3"}` is two parameters, not three, and the first is `\uFFFDx=3`.
          for (const std::string& key : init.object->EnumerableKeys()) {
            const Value* value = init.object->Get(key);
            const std::string name = util::ScrubLoneSurrogates(key);
            const std::string text = value == nullptr ? std::string() : UsvOf(call, *value);
            const auto found = std::find_if(pairs.begin(), pairs.end(), [&](const Value& pair) {
              return PairPart(pair, 0) == name;
            });
            if (found == pairs.end()) {
              pairs.push_back(MakePair(call.interpreter, name, text));
            } else {
              *found = MakePair(call.interpreter, name, text);
            }
          }
        } else if (init.type != js::ValueType::Undefined && init.type != js::ValueType::Null) {
          // A single leading "?" is removed, so `new URLSearchParams(location.search)` and
          // `new URLSearchParams(location.search.slice(1))` are the same list.
          std::string text = UsvOf(call, init);
          if (!text.empty() && text.front() == '?') {
            text.erase(0, 1);
          }
          for (const util::QueryPair& pair : util::ParseUrlEncoded(text)) {
            pairs.push_back(MakePair(call.interpreter, pair.first, pair.second));
          }
        }
        made.object->SetHidden(kPairsSlot, call.interpreter.NewArrayValue(std::move(pairs)));
        // Not clonable, for the reason a `URL` is not: see `MarkHostObject`.
        made.object->MarkHostObject();
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
