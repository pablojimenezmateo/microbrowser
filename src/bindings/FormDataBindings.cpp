// `FormData`, and `formData()` on `Request` and `Response`.
//
// It is a separate translation unit from `FormBindings.cpp` on purpose: that file is about a
// `<form>` element in a tree, and this is an ordered list of name/value pairs with no node in it.
// The only thing they share is a name.
//
// **What is here and what is not.** `formData()` reads an
// `application/x-www-form-urlencoded` body through the one urlencoded implementation in `util` --
// the same one `URLSearchParams` and the engine's form data set use, so a query string
// round-tripped through all three cannot change. A `multipart/form-data` *response* is still
// **rejected with a TypeError**, which is the standard's own answer for a body this cannot be
// read as: multipart parts carry filenames and content types and become `File` objects, and a
// `formData()` that returned the raw bytes under a made-up name would be worse than the
// rejection (ADR 0012). Encoding the other direction -- `fetch` of a FormData that holds a
// `File` -- is here, because the entry list is already this object's state.

#include <string>
#include <string_view>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/FetchSupport.h"
#include "util/MimeType.h"
#include "util/Sha2.h"
#include "util/StringUtil.h"
#include "util/UrlEncoded.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// The same shape `URLSearchParams` uses: an array of two-element arrays, in insertion order,
// because order is observable through `forEach` and iteration. In the JavaScript heap rather than
// a C++ vector, because the collector cannot see a `js::Value` in a C++ field.
constexpr const char* kEntriesSlot = "#formEntries";
constexpr const char* kIteratorTargetSlot = "#iterTarget";
constexpr const char* kIteratorIndexSlot = "#iterIndex";

std::vector<Value> ReadEntries(const Value& self) {
  std::vector<Value> out;
  if (!self.IsObject()) {
    return out;
  }
  const Value* entries = self.object->GetOwn(kEntriesSlot);
  if (entries == nullptr || !entries->IsObject()) {
    return out;
  }
  out.reserve(entries->object->ElementCount());
  for (std::size_t i = 0; i < entries->object->ElementCount(); ++i) {
    out.push_back(entries->object->GetElement(i));
  }
  return out;
}

void SetEntries(const Value& self, js::Interpreter& interpreter, std::vector<Value> entries) {
  if (self.IsObject()) {
    self.object->SetHidden(kEntriesSlot, interpreter.NewArrayValue(std::move(entries)));
  }
}

std::string EntryPart(const Value& entry, std::size_t index) {
  if (!entry.IsObject() || entry.object->ElementCount() <= index) {
    return {};
  }
  return js::ToString(entry.object->GetElement(index));
}

Value MakeEntry(js::Interpreter& interpreter, std::string name, std::string value) {
  return interpreter.NewArrayValue(
      {Value::String(std::move(name)), Value::String(std::move(value))});
}

Value MakeBlobEntry(js::Interpreter& interpreter, std::string name, const Value& blob,
                    std::string filename) {
  return interpreter.NewArrayValue(
      {Value::String(std::move(name)), blob, Value::String(std::move(filename))});
}

Value EntryValueOf(const Value& entry) {
  if (!entry.IsObject() || entry.object->ElementCount() < 2) {
    return Value::Undefined();
  }
  return entry.object->GetElement(1);
}

std::string EntryFilename(const Value& entry) {
  if (!entry.IsObject() || entry.object->ElementCount() < 3) {
    return {};
  }
  return js::ToString(entry.object->GetElement(2));
}

bool MakeAppendedEntry(NativeCall& call, Value& out) {
  std::string name;
  if (!CoerceToUsvString(call, Argument(call.arguments, 0), name)) {
    return false;
  }
  const Value value = Argument(call.arguments, 1);
  if (IsBlobValue(value)) {
    std::string filename;
    if (call.arguments.size() >= 3 && !call.arguments[2].IsUndefined()) {
      if (!CoerceToUsvString(call, call.arguments[2], filename)) {
        return false;
      }
    } else if (const Value* given = value.object->GetOwn(kBlobNameSlot);
               given != nullptr && given->IsString()) {
      filename = given->AsString();
    } else {
      filename = "blob";
    }
    out = MakeBlobEntry(call.interpreter, std::move(name), value, std::move(filename));
    return true;
  }
  std::string string_value;
  if (!CoerceToUsvString(call, value, string_value)) {
    return false;
  }
  out = MakeEntry(call.interpreter, std::move(name), std::move(string_value));
  return true;
}

std::string NormalizeCrlf(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out.append("\r\n");
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    } else if (text[i] == '\n') {
      out.append("\r\n");
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::string PercentEncodeQuotesAndNewlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  constexpr char kHex[] = "0123456789ABCDEF";
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\r' || c == '\n') {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string EncodeDispositionName(std::string_view name) {
  return PercentEncodeQuotesAndNewlines(NormalizeCrlf(name));
}

std::string EncodeDispositionFilename(std::string_view filename) {
  return PercentEncodeQuotesAndNewlines(filename);
}

std::string HexPrefix(std::string_view bytes, std::size_t count) {
  std::string out;
  out.reserve(count * 2);
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t i = 0; i < count && i < bytes.size(); ++i) {
    const auto b = static_cast<unsigned char>(bytes[i]);
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15]);
  }
  return out;
}

std::string BoundaryToken(std::string_view material) {
  std::string seed(material);
  for (;;) {
    const std::string digest = util::Sha2(util::HashAlgorithm::Sha256, seed);
    std::string token = "----MicrobrowserFormBoundary";
    token += HexPrefix(digest, 8);
    if (material.find(token) == std::string_view::npos) {
      return token;
    }
    seed.push_back('x');
  }
}

std::string BlobTypeOf(const Value& blob) {
  const Value* type = blob.object->GetOwn(kBlobTypeSlot);
  return type == nullptr || !type->IsString() ? std::string() : type->AsString();
}

std::string BlobBodyOf(const Value& blob) {
  const Value* body = blob.object->GetOwn(kBlobBodySlot);
  return body == nullptr || !body->IsString() ? std::string() : body->AsString();
}

bool SerializeFormData(const Value& form_data, std::string& out, std::string& content_type) {
  const std::vector<Value> entries = ReadEntries(form_data);
  std::string material;
  for (const Value& entry : entries) {
    material += EntryPart(entry, 0);
    const Value value = EntryValueOf(entry);
    if (IsBlobValue(value)) {
      material += EntryFilename(entry);
      material += BlobBodyOf(value);
    } else {
      material += js::ToString(value);
    }
  }
  const std::string token = BoundaryToken(material);
  out.clear();
  for (const Value& entry : entries) {
    out.append("--");
    out.append(token);
    out.append("\r\nContent-Disposition: form-data; name=\"");
    out.append(EncodeDispositionName(EntryPart(entry, 0)));
    out.push_back('"');
    const Value value = EntryValueOf(entry);
    if (IsBlobValue(value)) {
      out.append("; filename=\"");
      out.append(EncodeDispositionFilename(EntryFilename(entry)));
      out.append("\"\r\nContent-Type: ");
      const std::string type = BlobTypeOf(value);
      out.append(type.empty() ? "application/octet-stream" : type);
      out.append("\r\n\r\n");
      out.append(BlobBodyOf(value));
    } else {
      out.append("\r\n\r\n");
      out.append(NormalizeCrlf(js::ToString(value)));
    }
    out.append("\r\n");
  }
  out.append("--");
  out.append(token);
  out.append("--\r\n");
  content_type = "multipart/form-data; boundary=" + token;
  return true;
}

// A `Content-Type` down to its essential MIME type, lowercased: `application/x-www-form-urlencoded;
// charset=windows-1252` is the same type as the bare one. The charset parameter is *ignored* rather
// than honoured, which is the Fetch Standard's rule for form data and not an omission -- a form
// body is UTF-8 whatever the header claims.
std::string EssenceOf(std::string_view content_type) {
  const std::size_t semicolon = content_type.find(';');
  std::string_view essence =
      semicolon == std::string_view::npos ? content_type : content_type.substr(0, semicolon);
  std::string out;
  for (const char c : essence) {
    if (c != ' ' && c != '\t') {
      out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
  }
  return out;
}

}  // namespace

js::Value DomBindings::MakeFormData(const std::vector<util::QueryPair>& pairs) {
  const Value* prototype =
      interfaces_.IsObject() ? interfaces_.object->GetOwn("FormData") : nullptr;
  const Value made = interpreter_->NewObjectValue();
  if (!made.IsObject()) {
    return Value::Undefined();
  }
  if (prototype != nullptr && prototype->IsObject()) {
    made.object->SetPrototype(prototype->object);
  }
  std::vector<Value> entries;
  entries.reserve(pairs.size());
  for (const util::QueryPair& pair : pairs) {
    entries.push_back(MakeEntry(*interpreter_, pair.first, pair.second));
  }
  SetEntries(made, *interpreter_, std::move(entries));
  made.object->SetHidden(kFormDataMarkerSlot, Value::Bool(true));
  return made;
}

void DomBindings::InstallFormData() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("FormData", prototype);

  const auto method = [this, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      prototype.object->Set(name, native);
    }
  };

  method("append", [](NativeCall& call) {
    Value entry;
    if (!MakeAppendedEntry(call, entry)) {
      return call.ThrownValue();
    }
    std::vector<Value> entries = ReadEntries(call.self);
    entries.push_back(std::move(entry));
    SetEntries(call.self, call.interpreter, std::move(entries));
    return Value::Undefined();
  });
  method("set", [](NativeCall& call) {
    // Replaces the *first* match in place and drops the rest, rather than appending at the end.
    // The distinction is observable through iteration, and a page that sets a field it already has
    // expects the order it wrote.
    Value replacement;
    if (!MakeAppendedEntry(call, replacement)) {
      return call.ThrownValue();
    }
    const std::string name = EntryPart(replacement, 0);
    std::vector<Value> kept;
    bool replaced = false;
    for (const Value& entry : ReadEntries(call.self)) {
      if (EntryPart(entry, 0) != name) {
        kept.push_back(entry);
        continue;
      }
      if (!replaced) {
        kept.push_back(replacement);
        replaced = true;
      }
    }
    if (!replaced) {
      kept.push_back(std::move(replacement));
    }
    SetEntries(call.self, call.interpreter, std::move(kept));
    return Value::Undefined();
  });
  method("delete", [](NativeCall& call) {
    std::string name;
    if (!CoerceToUsvString(call, Argument(call.arguments, 0), name)) {
      return call.ThrownValue();
    }
    std::vector<Value> kept;
    for (const Value& entry : ReadEntries(call.self)) {
      if (EntryPart(entry, 0) != name) {
        kept.push_back(entry);
      }
    }
    SetEntries(call.self, call.interpreter, std::move(kept));
    return Value::Undefined();
  });
  method("get", [](NativeCall& call) -> Value {
    std::string name;
    if (!CoerceToUsvString(call, Argument(call.arguments, 0), name)) {
      return call.ThrownValue();
    }
    for (const Value& entry : ReadEntries(call.self)) {
      if (EntryPart(entry, 0) == name) {
        return EntryValueOf(entry);
      }
    }
    return Value::Null();  // null rather than undefined, which is what a page tests for
  });
  method("getAll", [](NativeCall& call) -> Value {
    std::string name;
    if (!CoerceToUsvString(call, Argument(call.arguments, 0), name)) {
      return call.ThrownValue();
    }
    std::vector<Value> found;
    for (const Value& entry : ReadEntries(call.self)) {
      if (EntryPart(entry, 0) == name) {
        found.push_back(EntryValueOf(entry));
      }
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("has", [](NativeCall& call) -> Value {
    std::string name;
    if (!CoerceToUsvString(call, Argument(call.arguments, 0), name)) {
      return call.ThrownValue();
    }
    for (const Value& entry : ReadEntries(call.self)) {
      if (EntryPart(entry, 0) == name) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  method("forEach", [](NativeCall& call) -> Value {
    // `(value, name, formData)`, in that order -- the same reversal trap `URLSearchParams.forEach`
    // documents. By index against the current list, so a callback that deletes what it was handed
    // sees what moved into its place.
    const Value callback = Argument(call.arguments, 0);
    if (!callback.IsObject() || !callback.object->IsCallable()) {
      return call.Throw("TypeError", "forEach needs a function");
    }
    const Value self_argument = Argument(call.arguments, 1);
    for (std::size_t i = 0;; ++i) {
      const std::vector<Value> entries = ReadEntries(call.self);
      if (i >= entries.size()) {
        break;
      }
      (void)call.interpreter.CallFunction(callback, self_argument,
                                          {EntryValueOf(entries[i]),
                                           Value::String(EntryPart(entries[i], 0)), call.self});
    }
    return Value::Undefined();
  });

  // The live iterator, for the reason `URLSearchParams` has one: an array snapshot is observably
  // wrong when a page mutates the list while walking it.
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
        const auto at =
            index == nullptr ? std::size_t{0} : static_cast<std::size_t>(js::ToNumber(*index));
        const std::vector<Value> entries =
            target == nullptr ? std::vector<Value>() : ReadEntries(*target);
        if (at >= entries.size()) {
          result.object->Set("done", Value::Bool(true));
          result.object->Set("value", Value::Undefined());
          return result;
        }
        step.self.object->SetHidden(kIteratorIndexSlot, Value::Number(static_cast<double>(at + 1)));
        result.object->Set("done", Value::Bool(false));
        if (part < 0) {
          result.object->Set("value",
                             step.interpreter.NewArrayValue(
                                 {Value::String(EntryPart(entries[at], 0)),
                                  EntryValueOf(entries[at])}));
        } else if (part == 0) {
          result.object->Set("value", Value::String(EntryPart(entries[at], 0)));
        } else {
          result.object->Set("value", EntryValueOf(entries[at]));
        }
        return result;
      });
      if (next.IsObject()) {
        iterator.object->Set("next", next);
      }
      const Value self_iterator =
          call.interpreter.NewNativeValue("[Symbol.iterator]",
                                          [](NativeCall& inner) { return inner.self; });
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
  if (const Value* entries = prototype.object->Get("entries"); entries != nullptr) {
    prototype.object->Set(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), *entries);
  }

  const Value constructor =
      interpreter_->NewNativeValue("FormData", [prototype](NativeCall& call) {
        const Value made = call.interpreter.NewObjectValue();
        if (!made.IsObject()) {
          return Value::Undefined();
        }
        made.object->SetPrototype(prototype.object);
        made.object->SetHidden(kFormDataMarkerSlot, Value::Bool(true));
        // `new FormData(form)` is deliberately not the form's data set: constructing one from an
        // element means running the "constructing the entry list" algorithm, which is the engine's
        // (it owns form ownership and submitter semantics), and a *partial* answer here -- the
        // descendants with a `name`, say -- would be a form body that differs from the one a submit
        // produces. Empty until that crosses the seam.
        SetEntries(made, call.interpreter, {});
        return made;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->SetHidden("constructor", constructor);
    interpreter_->Global()->Set("FormData", constructor);
    interpreter_->GlobalScope()->Declare("FormData", constructor, false);
  }
}

void DomBindings::InstallBodyFormData(const js::Value& prototype, const char* body_slot,
                                      const char* used_slot) {
  if (!prototype.IsObject()) {
    return;
  }
  const std::string slot(body_slot);
  const std::string used(used_slot);
  const Value form_data =
      interpreter_->NewNativeValue("formData", [slot, used](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || !call.self.IsObject()) {
          return SettledPromise(call.interpreter,
                                call.interpreter.MakeError("TypeError", "not a body"), true);
        }
        if (const Value* already = call.self.object->GetOwn(used.c_str());
            already != nullptr && js::ToBoolean(*already)) {
          return SettledPromise(call.interpreter,
                                call.interpreter.MakeError("TypeError", "body already read"), true);
        }
        // The type decides whether these bytes are form data at all, and a body that is not is a
        // rejection rather than an empty list -- a page that got an empty `FormData` for a JSON
        // body would carry on as if the fields were missing.
        std::string content_type;
        if (const Value* headers = call.self.object->Get("headers");
            headers != nullptr && headers->IsObject()) {
          if (const Value* getter = headers->object->Get("get"); getter != nullptr) {
            const js::Result found = call.interpreter.CallFunction(
                *getter, *headers, {Value::String("content-type")});
            if (!found.IsAbrupt() && found.value.IsString()) {
              content_type = found.value.AsString();
            }
          }
        }
        const std::string essence = EssenceOf(content_type);
        if (essence != "application/x-www-form-urlencoded") {
          return SettledPromise(
              call.interpreter,
              call.interpreter.MakeError(
                  "TypeError", "cannot read a " +
                                   (essence.empty() ? std::string("typeless") : essence) +
                                   " body as form data"),
              true);
        }
        call.self.object->SetHidden(used.c_str(), Value::Bool(true));
        const Value* body = call.self.object->GetOwn(slot.c_str());
        const std::string bytes = body == nullptr ? std::string() : js::ToString(*body);
        return SettledPromise(call.interpreter,
                              owner->MakeFormData(util::ParseUrlEncoded(bytes)), false);
      });
  if (form_data.IsObject()) {
    form_data.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("formData", form_data);
  }

  const Value blob = interpreter_->NewNativeValue("blob", [slot, used](NativeCall& call) -> Value {
    if (!call.self.IsObject()) {
      return SettledPromise(call.interpreter,
                            call.interpreter.MakeError("TypeError", "not a body"), true);
    }
    if (const Value* already = call.self.object->GetOwn(used.c_str());
        already != nullptr && js::ToBoolean(*already)) {
      return SettledPromise(call.interpreter,
                            call.interpreter.MakeError("TypeError", "body already read"), true);
    }
    std::string content_type;
    if (const Value* headers = call.self.object->Get("headers");
        headers != nullptr && headers->IsObject()) {
      if (const Value* getter = headers->object->Get("get"); getter != nullptr) {
        const js::Result found =
            call.interpreter.CallFunction(*getter, *headers, {Value::String("content-type")});
        if (!found.IsAbrupt() && found.value.IsString()) {
          content_type = found.value.AsString();
        }
      }
    }
    call.self.object->SetHidden(used.c_str(), Value::Bool(true));
    const Value* body = call.self.object->GetOwn(slot.c_str());
    const std::string bytes = body == nullptr ? std::string() : js::ToString(*body);
    return SettledPromise(call.interpreter,
                          MakeBlobValue(call.interpreter, bytes, util::BlobMimeType(content_type)),
                          false);
  });
  if (blob.IsObject()) {
    blob.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("blob", blob);
  }
}

bool EncodeFormDataBody(const js::Value& form_data, std::string& out, std::string& content_type) {
  return SerializeFormData(form_data, out, content_type);
}

}  // namespace microbrowser::bindings
