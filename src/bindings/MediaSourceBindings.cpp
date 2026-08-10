// `MediaSource`, `SourceBuffer`, and the object URL registry, as a page sees them.
//
// ADR 0028 §3, session 28. Everything real is on the other side of `bindings::MediaController`: the
// append algorithm, the range set, the quota, the codec allowlist. What is here is the shape of the
// API and the three things about that shape which are load-bearing.
//
// **A source is named by an id, not by a pointer.** The same decision the socket and storage seams
// made: a `std::uint64_t` is a number a page cannot forge into a pointer, and the table it indexes
// lives with the document. A page holding a `SourceBuffer` whose `MediaSource` has closed holds a
// dead id, and every method on it throws `InvalidStateError` -- which is the specified behaviour and
// also what makes it safe.
//
// **`appendBuffer` is asynchronous to a page and synchronous underneath.** The algorithm runs
// before `appendBuffer` returns; `updatestart`/`update`/`updateend` are queued as a macrotask
// (`TimerQueue::QueueTask`), which is the MSE sequencing rule and what stops youtube's SABR
// `Ty1` → `wSl` → `appendBuffer` → nested `Ty1`/`vW` from emptying `d9` while the outer call still
// holds the segment (TD-0020 → `fmt.unplayable`). A prior sync delivery with a re-entrancy gate
// flattened same-buffer nesting for stack depth but could not protect a Ty1 that was *not* already
// inside `Deliver`. Microtask deferral starved the pump; a host task is what MessageChannel uses
// for the same "later turn" contract and what `RunDue` already batches. **`sourceopen` is the
// same shape** (TD-0040): sync FireOn from `video.src = blob:…` under a spent soft-nav budget
// created SourceBuffers and never reached the first append.
//
// **A refused append is not a failure to hide.** `QuotaExceededError` is the signal a player is
// waiting for -- it is how a player is told to evict -- so it is thrown with that name, and the
// `error` event fires. A page whose append quietly did nothing would keep appending forever.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Media.h"
#include "bindings/Timers.h"
#include "dom/Node.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// The id a `MediaSource` or a `SourceBuffer` wrapper carries. Hidden, so a page cannot enumerate it
// or set it -- an id a page could write would be an id it could use to reach another document's
// source.
constexpr const char* kSourceIdSlot = "#media-source-id";
constexpr const char* kBufferIdSlot = "#source-buffer-id";
constexpr const char* kMediaSourcesKey = "media-sources";
// Page-visible `SourceBuffer.updating`: true from `appendBuffer`/`abort` return until the
// macrotask that fires `updateend`. The media algorithm itself clears its own flag before return.
constexpr const char* kBufferUpdatingSlot = "#sb-updating";

// `Object::Get` answers with a pointer, which is null for a property that is not there. Every read
// here wants a value, and undefined is the right answer for absent -- so the dereference happens once,
// in one place, rather than six times with six chances to forget the null.
Value Read(const Value& object, const std::string& key) {
  if (!object.IsObject()) {
    return Value::Undefined();
  }
  const Value* found = object.object->Get(key);
  return found == nullptr ? Value::Undefined() : *found;
}


std::uint64_t IdIn(const Value& object, const char* slot) {
  if (!object.IsObject()) {
    return 0;
  }
  const Value* value = object.object->GetOwn(slot);
  if (value == nullptr || !value->IsNumber()) {
    return 0;
  }
  const double number = value->number;
  return number > 0.0 ? static_cast<std::uint64_t>(number) : 0;
}

// The bytes of an `ArrayBuffer` or a typed array, copied.
//
// **Copied here rather than referenced**, which is not laziness: the buffer belongs to the page, and
// a page can detach it, resize it, or write into it between this call and the moment the far side
// looks. What arrives is a snapshot of what the page meant to append.
bool BytesOf(const Value& value, std::string& out) {
  if (!value.IsObject()) {
    return false;
  }
  const js::BufferView* view = value.object->View();
  if (view == nullptr || view->bytes == nullptr) {
    // Null bytes is a *detached* buffer, which a page can produce between building the view and
    // appending it. Refused rather than treated as empty: an empty append is a no-op that fires the
    // event pair, and a page whose transferred buffer silently became a no-op would keep appending.
    return false;
  }
  // A typed array's `length` is in elements and a DataView's is in bytes, which is the language's own
  // asymmetry. The byte span is what an append wants either way, so the element size is applied here
  // -- an `Int16Array` of ten elements is twenty bytes of segment.
  const std::size_t element_size = js::ElementSize(view->kind);
  const std::size_t byte_length = view->length * element_size;
  if (view->offset > view->bytes->size() || byte_length > view->bytes->size() - view->offset) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(view->bytes->data() + view->offset), byte_length);
  return true;
}

}  // namespace

void DomBindings::InstallMediaSource() {
  EnsureInterfaces();
  if (media_ == nullptr || !interfaces_.IsObject()) {
    // ADR 0012's rule: absent rather than stubbed. A page that finds `MediaSource` and gets a
    // `SourceBuffer` whose `buffered` is always empty stalls forever with no fallback; a page that
    // finds nothing falls back to `<video src>`, which this browser does have.
    return;
  }

  // --- SourceBuffer's prototype ---------------------------------------------------------------
  const Value buffer_prototype = interpreter_->NewObjectValue();
  if (!buffer_prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("SourceBuffer", buffer_prototype);

  // `SourceBuffer` on the global, so `typeof SourceBuffer` and `x instanceof SourceBuffer`
  // resolve. Calling `new SourceBuffer()` is an Illegal constructor TypeError in every
  // browser; the name exists for `instanceof` and for feature detection, not construction.
  {
    const Value buffer_ctor = interpreter_->NewNativeValue(
        "SourceBuffer", [](NativeCall& call) -> Value {
          return call.Throw("TypeError", "Illegal constructor: SourceBuffer");
        });
    if (buffer_ctor.IsObject()) {
      buffer_ctor.object->Set("prototype", buffer_prototype);
      buffer_prototype.object->Set("constructor", buffer_ctor);
      interpreter_->Global()->Set("SourceBuffer", buffer_ctor);
      interpreter_->GlobalScope()->Declare("SourceBuffer", buffer_ctor, false);
    }
  }

  const auto buffer_method = [this, &buffer_prototype](const char* name) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#sb-method");
      if (owner == nullptr || owner->media_ == nullptr || which == nullptr) {
        return Value::Undefined();
      }
      const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
      // A dead id is the case a page reaches by keeping a `SourceBuffer` past its source's close.
      // Throwing is the specified answer and it is also the safe one: answering would mean answering
      // about whatever now holds that id.
      if (id == 0 || !owner->media_->IsLiveSourceBuffer(id)) {
        return ThrowDom(call, "InvalidStateError",
                                 "InvalidStateError: the SourceBuffer has been removed");
      }
      const std::string what = js::ToString(*which);
      if (what == "appendBuffer") {
        std::string bytes;
        if (!BytesOf(Argument(call.arguments, 0), bytes)) {
          return call.Throw("TypeError", "appendBuffer expects an ArrayBuffer or a view of one");
        }
        util::AddPerformanceCounter(util::PerfCounterId::MediaSourceAppendAttempts);
        const int result = owner->media_->AppendToSourceBuffer(id, bytes);
        if (result == 4) {
          return ThrowDom(call, "InvalidStateError",
                                   "InvalidStateError: an append is already in progress");
        }
        call.self.object->SetHidden(kBufferUpdatingSlot, Value::Bool(true));
        owner->ScheduleSourceBufferEvents(call.self, id);
        switch (result) {
          case 0:
            return Value::Undefined();
          case 1:
            return ThrowDom(call, "QuotaExceededError",
                            "the SourceBuffer is full; remove some data");
          default:
            return Value::Undefined();
        }
      }
      if (what == "remove") {
        const double start = js::ToNumber(Argument(call.arguments, 0));
        const double end = js::ToNumber(Argument(call.arguments, 1));
        owner->media_->RemoveFromSourceBuffer(id, start, end);
        owner->ScheduleSourceBufferEvents(call.self, id);
        return Value::Undefined();
      }
      if (what == "abort") {
        owner->media_->AbortSourceBuffer(id);
        call.self.object->SetHidden(kBufferUpdatingSlot, Value::Bool(true));
        owner->ScheduleSourceBufferEvents(call.self, id);
        return Value::Undefined();
      }
      if (what == "changeType") {
        const std::string type = js::ToString(Argument(call.arguments, 0));
        const MediaController::AddBufferError error =
            owner->media_->ChangeSourceBufferType(id, type);
        if (error == MediaController::AddBufferError::NotSupported) {
          return ThrowDom(call, "NotSupportedError",
                                   "NotSupportedError: unsupported MIME type or codec");
        }
        if (error == MediaController::AddBufferError::InvalidState) {
          return ThrowDom(call, "InvalidStateError",
                                   "InvalidStateError: SourceBuffer.changeType while updating");
        }
        return Value::Undefined();
      }
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      native.object->SetHidden("#sb-method", Value::String(name));
      buffer_prototype.object->Set(name, native);
    }
  };
  buffer_method("appendBuffer");
  buffer_method("remove");
  buffer_method("abort");
  buffer_method("changeType");

  // `buffered`, as a `TimeRanges`. Built fresh on every read, because that is what it is: a snapshot
  // of the ranges at the moment a player asked, and a cached one would tell it about the append
  // before last.
  const Value buffered = interpreter_->NewNativeValue("buffered", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
    if (owner == nullptr || owner->media_ == nullptr || id == 0) {
      return Value::Undefined();
    }
    return owner->MakeTimeRanges(owner->media_->SourceBufferBuffered(id));
  });
  if (buffered.IsObject()) {
    buffered.object->Set(kOwnerSlot, PointerValue(this));
    buffer_prototype.object->DefineAccessor("buffered", buffered.object, nullptr);
  }

  const Value updating = interpreter_->NewNativeValue("updating", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
    if (call.self.IsObject()) {
      if (const Value* pending = call.self.object->GetOwn(kBufferUpdatingSlot);
          pending != nullptr && pending->type == js::ValueType::Boolean && pending->boolean) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(owner != nullptr && owner->media_ != nullptr && id != 0 &&
                       owner->media_->SourceBufferUpdating(id));
  });
  if (updating.IsObject()) {
    updating.object->Set(kOwnerSlot, PointerValue(this));
    buffer_prototype.object->DefineAccessor("updating", updating.object, nullptr);
  }

  const Value offset_get =
      interpreter_->NewNativeValue("timestampOffset", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
        if (owner == nullptr || owner->media_ == nullptr || id == 0) {
          return Value::Number(0.0);
        }
        return Value::Number(owner->media_->TimestampOffset(id));
      });
  const Value offset_set =
      interpreter_->NewNativeValue("timestampOffset", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
        if (owner != nullptr && owner->media_ != nullptr && id != 0) {
          owner->media_->SetTimestampOffset(id, js::ToNumber(Argument(call.arguments, 0)));
        }
        return Value::Undefined();
      });
  if (offset_get.IsObject() && offset_set.IsObject()) {
    offset_get.object->Set(kOwnerSlot, PointerValue(this));
    offset_set.object->Set(kOwnerSlot, PointerValue(this));
    buffer_prototype.object->DefineAccessor("timestampOffset", offset_get.object, offset_set.object);
  }

  // The append window, as its two properties. Written separately by a page and applied together,
  // which is why the far side takes both: a window whose start had been applied and whose end had not
  // would clip a segment against a window nobody asked for.
  const auto window_pair = [this, &buffer_prototype](const char* name, bool is_start) {
    const Value get = interpreter_->NewNativeValue(name, [is_start](NativeCall& call) -> Value {
      const Value* stored =
          call.self.IsObject() ? call.self.object->GetOwn(is_start ? "#aw-start" : "#aw-end")
                               : nullptr;
      if (stored != nullptr) {
        return *stored;
      }
      return Value::Number(is_start ? 0.0 : std::numeric_limits<double>::infinity());
    });
    const Value set = interpreter_->NewNativeValue(name, [is_start](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      const std::uint64_t id = IdIn(call.self, kBufferIdSlot);
      if (owner == nullptr || owner->media_ == nullptr || id == 0 || !call.self.IsObject()) {
        return Value::Undefined();
      }
      const Value value = Value::Number(js::ToNumber(Argument(call.arguments, 0)));
      call.self.object->SetHidden(is_start ? "#aw-start" : "#aw-end", value);
      const Value* start = call.self.object->GetOwn("#aw-start");
      const Value* end = call.self.object->GetOwn("#aw-end");
      owner->media_->SetAppendWindow(
          id, start == nullptr ? 0.0 : start->number,
          end == nullptr ? std::numeric_limits<double>::infinity() : end->number);
      return Value::Undefined();
    });
    if (get.IsObject() && set.IsObject()) {
      get.object->Set(kOwnerSlot, PointerValue(this));
      set.object->Set(kOwnerSlot, PointerValue(this));
      buffer_prototype.object->DefineAccessor(name, get.object, set.object);
    }
  };
  window_pair("appendWindowStart", true);
  window_pair("appendWindowEnd", false);

  // --- MediaSource's prototype ----------------------------------------------------------------
  const Value source_prototype = interpreter_->NewObjectValue();
  if (!source_prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("MediaSource", source_prototype);

  const Value add_buffer = interpreter_->NewNativeValue(
      "addSourceBuffer", [buffer_prototype](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        const std::uint64_t source = IdIn(call.self, kSourceIdSlot);
        if (owner == nullptr || owner->media_ == nullptr || source == 0) {
          return Value::Undefined();
        }
        const std::string type = js::ToString(Argument(call.arguments, 0));
        MediaController::AddBufferError error = MediaController::AddBufferError::None;
        const std::uint64_t id = owner->media_->AddSourceBuffer(source, type, error);
        if (id == 0) {
          // Two different exceptions, because a page handles them differently: `NotSupportedError`
          // means try another codec, and `InvalidStateError` means the source is not open. A single
          // error name would make a page retry the thing that cannot work.
          if (error == MediaController::AddBufferError::NotSupported) {
            return ThrowDom(call, "NotSupportedError",
                                     "NotSupportedError: unsupported MIME type or codec");
          }
          return ThrowDom(call, "InvalidStateError",
                                   "InvalidStateError: the MediaSource is not open");
        }
        const Value wrapper = call.interpreter.NewObjectValue();
        if (!wrapper.IsObject()) {
          return Value::Undefined();
        }
        wrapper.object->SetPrototype(buffer_prototype.object);
        wrapper.object->SetHidden(kBufferIdSlot, Value::Number(static_cast<double>(id)));
        owner->InstallEventMethods(wrapper);
        // Kept on the source's own list, which is what `sourceBuffers` is -- and what keeps the
        // wrapper alive for the collector while the source is. A `js::Value` in a C++ field would be
        // invisible to it.
        if (const Value* list = call.self.object->GetOwn("sourceBuffers")) {
          if (list->IsObject()) {
            const double count = js::ToNumber(Read(*list, "length"));
            list->object->Set(std::to_string(static_cast<long long>(count)), wrapper);
            list->object->Set("length", Value::Number(count + 1.0));
          }
        }
        return wrapper;
      });
  if (add_buffer.IsObject()) {
    add_buffer.object->Set(kOwnerSlot, PointerValue(this));
    source_prototype.object->Set("addSourceBuffer", add_buffer);
  }

  const Value end_of_stream =
      interpreter_->NewNativeValue("endOfStream", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        const std::uint64_t source = IdIn(call.self, kSourceIdSlot);
        if (owner == nullptr || owner->media_ == nullptr || source == 0) {
          return Value::Undefined();
        }
        if (owner->media_->SourceReadyState(source) != 1) {
          return ThrowDom(call, "InvalidStateError",
                                   "InvalidStateError: the MediaSource is not open");
        }
        owner->media_->EndOfStream(source);
        owner->DeliverMediaSourceEvents(call.self, source);
        return Value::Undefined();
      });
  if (end_of_stream.IsObject()) {
    end_of_stream.object->Set(kOwnerSlot, PointerValue(this));
    source_prototype.object->Set("endOfStream", end_of_stream);
  }

  const Value ready_state = interpreter_->NewNativeValue("readyState", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t source = IdIn(call.self, kSourceIdSlot);
    if (owner == nullptr || owner->media_ == nullptr || source == 0) {
      return Value::String("closed");
    }
    // Strings, because that is what the API is -- unlike the media element's numeric `readyState`,
    // which a page compares with `>=`. Two APIs, two spellings, and matching each is the job.
    switch (owner->media_->SourceReadyState(source)) {
      case 1:
        return Value::String("open");
      case 2:
        return Value::String("ended");
      default:
        return Value::String("closed");
    }
  });
  if (ready_state.IsObject()) {
    ready_state.object->Set(kOwnerSlot, PointerValue(this));
    source_prototype.object->DefineAccessor("readyState", ready_state.object, nullptr);
  }

  const Value duration_get = interpreter_->NewNativeValue("duration", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t source = IdIn(call.self, kSourceIdSlot);
    if (owner == nullptr || owner->media_ == nullptr || source == 0) {
      return Value::Number(std::numeric_limits<double>::quiet_NaN());
    }
    return Value::Number(owner->media_->SourceDuration(source));
  });
  const Value duration_set = interpreter_->NewNativeValue("duration", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t source = IdIn(call.self, kSourceIdSlot);
    if (owner != nullptr && owner->media_ != nullptr && source != 0) {
      owner->media_->SetSourceDuration(source, js::ToNumber(Argument(call.arguments, 0)));
    }
    return Value::Undefined();
  });
  if (duration_get.IsObject() && duration_set.IsObject()) {
    duration_get.object->Set(kOwnerSlot, PointerValue(this));
    duration_set.object->Set(kOwnerSlot, PointerValue(this));
    source_prototype.object->DefineAccessor("duration", duration_get.object, duration_set.object);
  }

  // `MediaSource.isTypeSupported`, a static. A player calls it before it builds anything, and it must
  // agree with `addSourceBuffer` exactly -- which it does because both ask the same allowlist on the
  // far side. Two answers to "can we play this" would be the worst kind of bug: a player told yes and
  // then refused.
  const Value constructor =
      interpreter_->NewNativeValue("MediaSource", [source_prototype](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->media_ == nullptr) {
          return Value::Undefined();
        }
        const std::uint64_t id = owner->media_->CreateMediaSource();
        if (id == 0) {
          return Value::Undefined();
        }
        const Value source = call.interpreter.NewObjectValue();
        if (!source.IsObject()) {
          return Value::Undefined();
        }
        source.object->SetPrototype(source_prototype.object);
        source.object->SetHidden(kSourceIdSlot, Value::Number(static_cast<double>(id)));
        const Value list = call.interpreter.NewObjectValue();
        if (list.IsObject()) {
          list.object->Set("length", Value::Number(0.0));
          source.object->Set("sourceBuffers", list);
        }
        // An event target: `sourceopen` is how every player learns it may start appending, and a
        // MediaSource that could not be listened to would be one nothing could use.
        owner->InstallEventMethods(source);
        owner->RegisterMediaSourceWrapper(id, source);
        return source;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    // Without this, `MediaSource.prototype` is undefined while instances still carry the
    // methods via SetPrototype — feature detection that walks the constructor fails and
    // youtube's player takes a different path than the one that appends.
    constructor.object->Set("prototype", source_prototype);
    source_prototype.object->Set("constructor", constructor);
    const Value is_supported =
        interpreter_->NewNativeValue("isTypeSupported", [](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          if (owner == nullptr || owner->media_ == nullptr) {
            return Value::Bool(false);
          }
          MediaController::AddBufferError error = MediaController::AddBufferError::None;
          // Asked of the allowlist without creating anything: `AddSourceBuffer` with a zero source id
          // is the type check alone, which is how the two cannot disagree.
          owner->media_->AddSourceBuffer(0, js::ToString(Argument(call.arguments, 0)), error);
          return Value::Bool(error != MediaController::AddBufferError::NotSupported);
        });
    if (is_supported.IsObject()) {
      is_supported.object->Set(kOwnerSlot, PointerValue(this));
      constructor.object->Set("isTypeSupported", is_supported);
    }
    interpreter_->Global()->Set("MediaSource", constructor);
    interpreter_->GlobalScope()->Declare("MediaSource", constructor, false);
  }

  InstallObjectUrls();
}

void DomBindings::InstallObjectUrls() {
  if (interpreter_ == nullptr) {
    return;
  }
  InstallBlob();
  InstallUrlConstructor();
  const Value* url_slot = interpreter_->Global()->Get("URL");
  const Value url = url_slot == nullptr ? Value::Undefined() : *url_slot;
  if (!url.IsObject()) {
    return;
  }
  const Value create = interpreter_->NewNativeValue("createObjectURL", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const Value& argument = Argument(call.arguments, 0);
    if (owner != nullptr && owner->IsBlobValue(argument)) {
      if (owner->network_ == nullptr) {
        return call.Throw("TypeError", "createObjectURL expects a Blob");
      }
      const std::string blob_url =
          owner->network_->RegisterBlobUrl(owner->BlobBodyOf(argument), owner->BlobTypeOf(argument));
      if (blob_url.empty()) {
        return call.Throw("TypeError", "createObjectURL expects a Blob");
      }
      return Value::String(blob_url);
    }
    if (owner == nullptr || owner->media_ == nullptr) {
      return Value::Undefined();
    }
    const std::uint64_t id = IdIn(Argument(call.arguments, 0), kSourceIdSlot);
    if (id == 0) {
      return call.Throw("TypeError", "createObjectURL expects a MediaSource or Blob");
    }
    const std::string created = owner->media_->CreateObjectUrl(id);
    if (created.empty()) {
      return call.Throw("TypeError", "createObjectURL expects a MediaSource or Blob");
    }
    return Value::String(created);
  });
  if (create.IsObject()) {
    create.object->Set(kOwnerSlot, PointerValue(this));
    url.object->Set("createObjectURL", create);
  }
  const Value revoke = interpreter_->NewNativeValue("revokeObjectURL", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Undefined();
    }
    const std::string object_url = js::ToString(Argument(call.arguments, 0));
    if (owner->network_ != nullptr && util::StartsWithAsciiCaseInsensitive(object_url, "blob:null/")) {
      owner->network_->RevokeBlobUrl(object_url);
    } else if (owner->media_ != nullptr) {
      owner->media_->RevokeObjectUrl(object_url);
    }
    return Value::Undefined();
  });
  if (revoke.IsObject()) {
    revoke.object->Set(kOwnerSlot, PointerValue(this));
    url.object->Set("revokeObjectURL", revoke);
  }
}

js::Value DomBindings::MakeTimeRanges(const std::vector<double>& flat) {
  // `TimeRanges`: `length`, `start(i)`, `end(i)`. An index-based API rather than a list, which is
  // why the far side hands over a flat vector -- the shape a page reads is not a shape worth
  // building twice.
  const Value ranges = interpreter_->NewObjectValue();
  if (!ranges.IsObject()) {
    return Value::Undefined();
  }
  const Value stored = interpreter_->NewObjectValue();
  if (stored.IsObject()) {
    for (std::size_t i = 0; i < flat.size(); ++i) {
      stored.object->Set(std::to_string(i), Value::Number(flat[i]));
    }
    stored.object->Set("length", Value::Number(static_cast<double>(flat.size())));
    ranges.object->SetHidden("#flat", stored);
  }
  ranges.object->Set("length", Value::Number(static_cast<double>(flat.size() / 2)));
  const auto accessor = [this, &ranges](const char* name, std::size_t which) {
    const Value native = interpreter_->NewNativeValue(name, [which](NativeCall& call) -> Value {
      const Value* flat_value = call.self.IsObject() ? call.self.object->GetOwn("#flat") : nullptr;
      if (flat_value == nullptr || !flat_value->IsObject()) {
        return Value::Number(0.0);
      }
      const double index = js::ToNumber(Argument(call.arguments, 0));
      const double count = js::ToNumber(Read(*flat_value, "length")) / 2.0;
      if (!(index >= 0.0) || index >= count) {
        // The specification throws `IndexSizeError` for an out-of-range index, and a player that
        // walks past the end depends on being stopped rather than being handed a zero it will treat
        // as a real range starting at the beginning of the stream.
        return ThrowDom(call, "IndexSizeError", "index out of range");
      }
      const std::size_t at = static_cast<std::size_t>(index) * 2 + which;
      return Read(*flat_value, std::to_string(at));
    });
    if (native.IsObject()) {
      ranges.object->Set(name, native);
    }
  };
  accessor("start", 0);
  accessor("end", 1);
  return ranges;
}

namespace {

// One event, delivered to an `on<type>` handler and to `addEventListener` listeners alike.
//
// Both paths intentionally: youtube's player wires `onupdateend` and other code uses
// `addEventListener('updateend')`. `dispatchEvent` alone would cover both *if* the property
// handler is found through `Get`, but delivering `on*` first matches the previous behaviour a
// successful SABR pump was measured against. Duplicate appends from a double-fire are prevented
// by the per-buffer deliver gate (the second append queues events for the same loop).
void FireOn(js::Interpreter& interpreter, const Value& target, const std::string& type) {
  if (!target.IsObject()) {
    return;
  }
  // Each MSE callback is one host-facing event even when delivered inside
  // appendBuffer (frames still live). Without this, a spent kevlar budget
  // aborts the SABR pump and youtube maps the empty queue to fmt.unplayable.
  const js::Interpreter::MediaEventBudget media_budget(interpreter);
  const Value event = interpreter.NewObjectValue();
  if (event.IsObject()) {
    event.object->Set("type", Value::String(type));
    event.object->Set("target", target);
    event.object->Set("bubbles", Value::Bool(false));
    event.object->Set("cancelable", Value::Bool(false));
  }
  if (const Value* handler = target.object->Get("on" + type);
      handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
    const js::Result answer = interpreter.CallFunction(*handler, target, {event});
    if (answer.completion == js::Completion::Throw) {
      interpreter.ReportUncaught(answer.value, "mediasource handler");
    }
  }
  if (const Value* dispatch = target.object->GetOwn("dispatchEvent");
      dispatch != nullptr && dispatch->IsObject()) {
    const js::Result answer = interpreter.CallFunction(*dispatch, target, {event});
    if (answer.completion == js::Completion::Throw) {
      interpreter.ReportUncaught(answer.value, "mediasource dispatch");
    }
  }
}

}  // namespace

void DomBindings::RegisterMediaSourceWrapper(std::uint64_t id, const js::Value& wrapper) {
  EnsureInterfaces();
  if (!interfaces_.IsObject() || !wrapper.IsObject()) {
    return;
  }
  // Hung off the interfaces object, which is a real JS object the collector walks. A C++ map of
  // `js::Value` would be invisible to it, and a MediaSource collected while its element was still
  // attached is a video that stops for no reason a page can see.
  Value table = Read(interfaces_, kMediaSourcesKey);
  if (!table.IsObject()) {
    table = interpreter_->NewObjectValue();
    if (!table.IsObject()) {
      return;
    }
    interfaces_.object->Set(kMediaSourcesKey, table);
  }
  table.object->Set(std::to_string(id), wrapper);
}

js::Value DomBindings::MediaSourceWrapper(std::uint64_t id) const {
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  const Value* table = interfaces_.object->GetOwn(kMediaSourcesKey);
  if (table == nullptr || !table->IsObject()) {
    return Value::Undefined();
  }
  return Read(*table, std::to_string(id));
}

void DomBindings::ScheduleSourceBufferEvents(const js::Value& buffer, std::uint64_t id) {
  if (interpreter_ == nullptr || !buffer.IsObject()) {
    return;
  }
  // Coalesce: one pending task per buffer. A second append before the first
  // updateend task runs still leaves its events in the media queue; the task
  // drains them. Double-queueing would only reorder relative to other hosts.
  if (const Value* pending = buffer.object->GetOwn("#sb-events-queued");
      pending != nullptr && pending->type == js::ValueType::Boolean && pending->boolean) {
    return;
  }
  buffer.object->SetHidden("#sb-events-queued", Value::Bool(true));
  DomBindings* self = this;
  const Value deliver = interpreter_->NewNativeValue(
      "sourceBufferEvents", [self, buffer, id](NativeCall&) -> Value {
        if (buffer.IsObject()) {
          buffer.object->SetHidden("#sb-events-queued", Value::Bool(false));
        }
        self->DeliverSourceBufferEvents(buffer, id);
        return Value::Undefined();
      });
  if (!deliver.IsObject()) {
    buffer.object->SetHidden("#sb-events-queued", Value::Bool(false));
    DeliverSourceBufferEvents(buffer, id);
    return;
  }
  deliver.object->Set("#buffer", buffer);
  if (!TimerQueue::QueueTask(*interpreter_, deliver)) {
    buffer.object->SetHidden("#sb-events-queued", Value::Bool(false));
    DeliverSourceBufferEvents(buffer, id);
  }
}

void DomBindings::DeliverSourceBufferEvents(const js::Value& buffer, std::uint64_t id) {
  if (media_ == nullptr || interpreter_ == nullptr || !buffer.IsObject()) {
    return;
  }
  // One Take per invocation. A FireOn handler that appends again schedules a
  // *later* task via ScheduleSourceBufferEvents; draining those events here
  // would re-enter Ty1 under the outer call (TD-0020). The old for(;;) +
  // per-buffer gate flattened C++ stack growth but still fired nested
  // updateend before the outer handler finished its post-append work (`vW`).
  const std::vector<std::string> events = media_->TakeSourceBufferEvents(id);
  if (events.empty()) {
    media_->FlushMediaEventsForBuffer(id);
    return;
  }
  // Spec order: updating becomes false before update/updateend, so the pump may append again.
  buffer.object->SetHidden(kBufferUpdatingSlot, Value::Bool(false));
  for (const std::string& type : events) {
    FireOn(*interpreter_, buffer, type);
  }
  media_->FlushMediaEventsForBuffer(id);
}

void DomBindings::DeliverMediaSourceEvents(const js::Value& source, std::uint64_t id) {
  if (media_ == nullptr || interpreter_ == nullptr) {
    return;
  }
  for (const std::string& type : media_->TakeMediaSourceEvents(id)) {
    FireOn(*interpreter_, source, type);
  }
}

bool DomBindings::DeliverMediaSourceOpenedFor(const std::string& url) {
  if (media_ == nullptr) {
    return false;
  }
  return DeliverMediaSourceOpened(media_->SourceForObjectUrl(url));
}

void DomBindings::ScheduleMediaSourceOpened(std::uint64_t id) {
  if (interpreter_ == nullptr || media_ == nullptr || id == 0) {
    return;
  }
  const Value source = MediaSourceWrapper(id);
  if (!source.IsObject()) {
    return;
  }
  // Coalesce: one pending task per MediaSource. A second attach before the
  // first sourceopen runs still leaves events in the media queue.
  if (const Value* pending = source.object->GetOwn("#ms-open-queued");
      pending != nullptr && pending->type == js::ValueType::Boolean && pending->boolean) {
    return;
  }
  source.object->SetHidden("#ms-open-queued", Value::Bool(true));
  DomBindings* self = this;
  const Value deliver = interpreter_->NewNativeValue(
      "mediaSourceOpen", [self, source, id](NativeCall&) -> Value {
        if (source.IsObject()) {
          source.object->SetHidden("#ms-open-queued", Value::Bool(false));
        }
        (void)self->DeliverMediaSourceOpened(id);
        return Value::Undefined();
      });
  if (!deliver.IsObject()) {
    source.object->SetHidden("#ms-open-queued", Value::Bool(false));
    (void)DeliverMediaSourceOpened(id);
    return;
  }
  deliver.object->Set("#source", source);
  if (!TimerQueue::QueueTask(*interpreter_, deliver)) {
    source.object->SetHidden("#ms-open-queued", Value::Bool(false));
    (void)DeliverMediaSourceOpened(id);
  }
}

bool DomBindings::DeliverMediaSourceOpened(std::uint64_t id) {
  const Value source = MediaSourceWrapper(id);
  if (!source.IsObject()) {
    return false;
  }
  DeliverMediaSourceEvents(source, id);
  return true;
}

}  // namespace microbrowser::bindings
