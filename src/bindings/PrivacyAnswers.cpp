// What a page is told when it asks about the machine.
//
// ADR 0029 §§1, 5 and 6, session 37. The values are in `bindings/Fingerprint.h`; this is where they are
// handed to a page, plus the three permission-gated APIs and the two `matchMedia` features that are
// deliberate exceptions to the constant rule.
//
// **The absences are as much of the feature as the values are**, and they are the reason this file is
// mostly short functions rather than one big installer: `navigator.deviceMemory`,
// `navigator.connection`, `navigator.getBattery`, `navigator.geolocation`, `navigator.mediaDevices` and
// `navigator.doNotTrack` are all things a page can find. Under ADR 0012's rule a page that finds nothing
// takes whatever path it has for a browser without them; a page that finds a plausible-looking zero
// takes the path that assumes it works. `tests/PrivacyAnswerTests.cpp` names every one, so putting any
// of them back is a decision somebody makes on purpose rather than a line that slips in.

#include <cstdint>
#include <span>
#include <utility>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Fingerprint.h"
#include "bindings/Geometry.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/Random.h"
#include "util/UserAgent.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

}  // namespace

void DomBindings::InstallPrivacyAnswers(const js::Value& navigator) {
  if (interpreter_ == nullptr || !navigator.IsObject()) {
    return;
  }
  // The constants, straight from the table. `userAgent` is already set by InstallWindow from the same
  // `util::kUserAgent` the `User-Agent` header uses -- one constant, because a page may sniff both and
  // two that were meant to agree eventually do not.
  navigator.object->Set("appVersion", Value::String(std::string(util::kUserAgent)));
  navigator.object->Set("appName", Value::String("Netscape"));
  navigator.object->Set("appCodeName", Value::String("Mozilla"));
  navigator.object->Set("product", Value::String("Gecko"));
  navigator.object->Set("platform", Value::String(std::string(kPlatform)));
  navigator.object->Set("vendor", Value::String(std::string(kVendor)));
  navigator.object->Set("language", Value::String(std::string(kLanguage)));
  navigator.object->Set("hardwareConcurrency",
                        Value::Number(static_cast<double>(kHardwareConcurrency)));
  // `cookieEnabled` is true because cookies work (ADR 0005 partitions them); saying false would make a
  // page that tests it take a no-cookie path it probably does not maintain.
  navigator.object->Set("cookieEnabled", Value::Bool(true));
  // A one-entry list. Not empty: `navigator.languages[0]` is the idiom, and an empty array makes it
  // `undefined` -- which a page then passes to `Intl` or to a URL and gets nonsense from.
  const Value languages = interpreter_->NewArrayValue({Value::String(std::string(kLanguage))});
  if (languages.IsObject()) {
    navigator.object->Set("languages", languages);
  }
  // **Empty, not absent.** `navigator.plugins.length` is one of the oldest fingerprinting reads there
  // is, and a page that finds no `plugins` at all often assumes an ancient browser -- so the answer is
  // an empty list, which is both true and constant.
  for (const char* name : {"plugins", "mimeTypes"}) {
    const Value empty = interpreter_->NewArrayValue({});
    if (empty.IsObject()) {
      empty.object->Set("length", Value::Number(0.0));
      navigator.object->Set(name, empty);
    }
  }

  InstallPermissions(navigator);
  InstallClipboard(navigator);
  InstallUserActivation(navigator);
}

void DomBindings::InstallPermissions(const js::Value& navigator) {
  const Value permissions = interpreter_->NewObjectValue();
  if (!permissions.IsObject() || !navigator.IsObject()) {
    return;
  }
  const Value query = interpreter_->NewNativeValue("query", [](NativeCall& call) -> Value {
    const Value descriptor = Argument(call.arguments, 0);
    std::string name;
    if (descriptor.IsObject()) {
      if (const Value* value = descriptor.object->Get("name")) {
        name = js::ToString(*value);
      }
    }
    if (!IsKnownPermission(name)) {
      // The specification's answer, and a more useful one than "denied": a page querying a capability
      // this browser does not have at all has a bug, where one told "denied" would keep asking.
      const Value rejected = call.interpreter.NewPromiseValue();
      call.interpreter.SettleAsyncResult(
          rejected.object,
          call.interpreter.MakeError("TypeError",
                                     "the permission name is not one this browser knows"),
          true);
      return rejected;
    }
    const Value status = call.interpreter.NewObjectValue();
    if (status.IsObject()) {
      status.object->Set("name", Value::String(name));
      // **Honestly**, which is what makes this API worth having rather than decorative: a page told
      // `denied` can tell its user, where one told `prompt` waits for a prompt that never comes.
      status.object->Set("state", Value::String(std::string(PermissionStateFor(name))));
      // An event target, because `status.onchange` is how a page watches for a grant -- and this one
      // never fires, which is correct: the state cannot change, because there is no prompt.
      status.object->Set("onchange", Value::Null());
    }
    const Value promise = call.interpreter.NewPromiseValue();
    call.interpreter.SettleAsyncResult(promise.object, status, false);
    return promise;
  });
  if (query.IsObject()) {
    permissions.object->Set("query", query);
    navigator.object->Set("permissions", permissions);
  }
}

void DomBindings::InstallUserActivation(const js::Value& navigator) {
  if (interpreter_ == nullptr || !navigator.IsObject()) {
    return;
  }
  // ADR 0017 §3 / HTML §6.4.7. youtube's `playVideo()` reads
  // `navigator.userActivation.isActive` before calling `<video>.play()`; when
  // the name was missing the check read false and playback never started.
  //
  // The document holds one sticky bit (dom::Document::HasUserActivation); both
  // accessors read it until a transient model arrives. See Node.h.
  const Value user_activation = interpreter_->NewObjectValue();
  if (!user_activation.IsObject()) {
    return;
  }
  const auto flag_getter = [this](const char* name) {
    const Value getter = interpreter_->NewNativeValue(name, [](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      return Value::Bool(owner != nullptr && owner->HasUserActivation());
    });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      return getter;
    }
    return Value::Undefined();
  };
  const Value is_active = flag_getter("isActive");
  const Value has_been_active = flag_getter("hasBeenActive");
  if (is_active.IsObject()) {
    user_activation.object->DefineAccessor("isActive", is_active.object, nullptr);
  }
  if (has_been_active.IsObject()) {
    user_activation.object->DefineAccessor("hasBeenActive", has_been_active.object, nullptr);
  }
  navigator.object->Set("userActivation", user_activation);
}

void DomBindings::InstallClipboard(const js::Value& navigator) {
  const Value clipboard = interpreter_->NewObjectValue();
  if (!clipboard.IsObject() || !navigator.IsObject()) {
    return;
  }
  // `writeText`, allowed on a gesture. A copy button is common, the user pressed one, and ADR 0017's
  // trusted-event path is the gate -- so this checks the document's user activation rather than a
  // permission. Without one it rejects, which is what every browser does.
  const Value write_text = interpreter_->NewNativeValue("writeText", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const Value promise = call.interpreter.NewPromiseValue();
    if (owner == nullptr || !owner->HasUserActivation()) {
      call.interpreter.SettleAsyncResult(
          promise.object,
          MakeDomException(call.interpreter, "NotAllowedError",
                           "writing the clipboard needs a user gesture"),
          true);
      return promise;
    }
    owner->SetClipboardText(js::ToString(Argument(call.arguments, 0)));
    call.interpreter.SettleAsyncResult(promise.object, Value::Undefined(), false);
    return promise;
  });
  if (write_text.IsObject()) {
    write_text.object->Set(kOwnerSlot, OwnerValue(this));
    clipboard.object->Set("writeText", write_text);
  }
  // **`readText` is refused**, and it is present-but-rejecting rather than absent because the
  // specification defines exactly this: a `NotAllowedError`. The clipboard holds whatever the user last
  // selected *anywhere on the machine*, which may be a password from a manager -- and a page reading it
  // is reading data the user gave to something else.
  const Value read_text = interpreter_->NewNativeValue("readText", [](NativeCall& call) -> Value {
    const Value promise = call.interpreter.NewPromiseValue();
    call.interpreter.SettleAsyncResult(
        promise.object,
        MakeDomException(call.interpreter, "NotAllowedError",
                         "reading the clipboard is not permitted"),
        true);
    return promise;
  });
  if (read_text.IsObject()) {
    clipboard.object->Set("readText", read_text);
  }
  navigator.object->Set("clipboard", clipboard);
}

void DomBindings::InstallNotification() {
  if (interpreter_ == nullptr) {
    return;
  }
  // The one thing on this table that looks like a stub, and it is defensible precisely because the
  // *specification* defines a denied state with exactly this behaviour: the constructor exists, it
  // shows nothing, and `permission` is `"denied"`. The page is not being misled -- it is being told no
  // in the vocabulary the API has for no.
  const Value constructor =
      interpreter_->NewNativeValue("Notification", [](NativeCall& call) -> Value {
        const Value notification = call.interpreter.NewObjectValue();
        if (notification.IsObject()) {
          notification.object->Set("title", Value::String(js::ToString(Argument(call.arguments, 0))));
          // `close` exists and does nothing, because a page that constructs one usually closes it and
          // a missing method would be a TypeError in an error path.
          const Value close = call.interpreter.NewNativeValue(
              "close", [](NativeCall&) { return Value::Undefined(); });
          if (close.IsObject()) {
            notification.object->Set("close", close);
          }
        }
        return notification;
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set("permission", Value::String(std::string(kPermissionDenied)));
  const Value request = interpreter_->NewNativeValue(
      "requestPermission", [](NativeCall& call) -> Value {
        // Resolves `"denied"` rather than rejecting: the specification says it resolves with the
        // resulting state, and a rejection would look to a page like the call failed rather than like
        // the answer was no.
        const Value promise = call.interpreter.NewPromiseValue();
        call.interpreter.SettleAsyncResult(promise.object,
                                           Value::String(std::string(kPermissionDenied)), false);
        return promise;
      });
  if (request.IsObject()) {
    constructor.object->Set("requestPermission", request);
  }
  interpreter_->Global()->Set("Notification", constructor);
  interpreter_->GlobalScope()->Declare("Notification", constructor, false);
}

void DomBindings::InstallScreenAndPixelRatio() {
  if (interpreter_ == nullptr) {
    return;
  }
  // `devicePixelRatio`, quantised to {1, 2, 3}. An accessor rather than a value because the ratio can
  // change -- a window dragged between displays -- and a page that read it once and cached it would be
  // one this browser had told the truth to at the wrong moment.
  const Value ratio = interpreter_->NewNativeValue("devicePixelRatio", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr || owner->geometry_ == nullptr) {
      return Value::Number(1.0);
    }
    return Value::Number(QuantizeDevicePixelRatio(owner->geometry_->QueryDevicePixelRatio()));
  });
  if (ratio.IsObject()) {
    ratio.object->Set(kOwnerSlot, OwnerValue(this));
    interpreter_->Global()->DefineAccessor("devicePixelRatio", ratio.object, nullptr);
  }

  // `screen`, which reports **the viewport rather than the display**.
  //
  // That is ADR 0029 §6's row and it is the sharpest single line on the table: a display's resolution
  // is a strong, stable identifier that a page can read with no interaction at all, and it is not a
  // number any page needs -- what a page actually wants when it reads `screen.width` is "how much room
  // do I have", which is the viewport. So the answer is the viewport, quantised, and `availWidth`
  // agrees with it because there is no window furniture to subtract.
  const Value screen = interpreter_->NewObjectValue();
  if (!screen.IsObject()) {
    return;
  }
  struct Field {
    const char* name;
    bool vertical;
  };
  static constexpr Field kFields[] = {{"width", false},     {"height", true},
                                      {"availWidth", false}, {"availHeight", true}};
  for (const Field& field : kFields) {
    const bool vertical = field.vertical;
    const Value getter = interpreter_->NewNativeValue(field.name, [vertical](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->geometry_ == nullptr) {
        return Value::Number(0.0);
      }
      const GeometryRect viewport = owner->geometry_->QueryViewport();
      return Value::Number(static_cast<double>(QuantizeViewportExtent(
          static_cast<int>(vertical ? viewport.height : viewport.width))));
    });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      screen.object->DefineAccessor(field.name, getter.object, nullptr);
    }
  }
  // Constants, because they *are* constant here: this browser composites 32-bit RGBA and nothing else,
  // so reporting anything but 24/24 would be reporting a lie about a fixed property.
  screen.object->Set("colorDepth", Value::Number(24.0));
  screen.object->Set("pixelDepth", Value::Number(24.0));
  interpreter_->Global()->Set("screen", screen);
  interpreter_->GlobalScope()->Declare("screen", screen, false);
}

void DomBindings::InstallCrypto() {
  if (interpreter_ == nullptr) {
    return;
  }
  const Value crypto = interpreter_->NewObjectValue();
  if (!crypto.IsObject()) {
    return;
  }
  // **Real randomness, from the system.** The one entry on ADR 0029 §6's table that is not reduced,
  // and the reason is that it carries no information *about* the machine: a page that gets 32 random
  // bytes learns nothing, and a page that gets predictable ones has its session tokens guessed.
  // Weakening this would be trading a privacy property for a security hole.
  const Value get_random = interpreter_->NewNativeValue(
      "getRandomValues", [](NativeCall& call) -> Value {
        const Value array = Argument(call.arguments, 0);
        if (!array.IsObject()) {
          return call.Throw("TypeError", "getRandomValues expects an integer typed array");
        }
        const js::BufferView* view = array.object->View();
        if (view == nullptr || view->bytes == nullptr) {
          return call.Throw("TypeError", "getRandomValues expects an integer typed array");
        }
        const std::size_t length = view->length * js::ElementSize(view->kind);
        // The specification's bound, and it is the specification's for a reason: 65536 bytes is far
        // more than any key or token, and an unbounded call is a page asking for the entropy pool.
        if (length > 65536) {
          return ThrowDom(call, "QuotaExceededError", "at most 65536 bytes at a time");
        }
        if (view->offset > view->bytes->size() || length > view->bytes->size() - view->offset) {
          return call.Throw("TypeError", "the view is out of bounds for its buffer");
        }
        if (!util::FillRandomBytes(
                std::span<std::uint8_t>(view->bytes->data() + view->offset, length))) {
          // **Throws rather than filling with anything.** There is no pseudo-random fallback in
          // `util::FillRandomBytes` and there must not be one here: a page whose key material came from
          // a weak generator has no way to find out, where one that caught an exception does.
          return ThrowDom(call, "OperationError", "no source of randomness is available");
        }
        return array;  // the same array, which is what the specification returns
      });
  if (get_random.IsObject()) {
    crypto.object->Set("getRandomValues", get_random);
  }
  // `randomUUID`, which is `getRandomValues` plus the version and variant bits. Worth having because a
  // page that needs one otherwise writes it from `Math.random`, which is not random.
  const Value random_uuid = interpreter_->NewNativeValue("randomUUID", [](NativeCall&) -> Value {
    std::uint8_t bytes[16] = {};
    if (!util::FillRandomBytes(std::span<std::uint8_t>(bytes, sizeof(bytes)))) {
      return Value::Undefined();
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0Fu) | 0x40u);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3Fu) | 0x80u);  // variant 1
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) {
        out.push_back('-');
      }
      out.push_back(kHex[bytes[i] >> 4]);
      out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return Value::String(out);
  });
  if (random_uuid.IsObject()) {
    crypto.object->Set("randomUUID", random_uuid);
  }
  InstallSubtleCrypto(crypto);
  interpreter_->Global()->Set("crypto", crypto);
  interpreter_->GlobalScope()->Declare("crypto", crypto, false);
}

}  // namespace microbrowser::bindings
