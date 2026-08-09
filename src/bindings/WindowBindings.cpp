#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>

#include "util/UserAgent.h"

// `window`, `location` and `navigator`: what a page reads about its
// environment rather than about its document.
//
// Split from DomBindings.cpp because that file reached the module's line cap,
// and the cap is written to mean a missing translation unit rather than a
// bigger file. These are the right ones to move: they are globals about the
// browsing context, and nothing in the tree walking refers to them.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

}  // namespace

void DomBindings::InstallWindow() {
  // `window` is the global object, and that is not a convenience alias -- a
  // page writes `window.foo = 1` and then reads `foo`, and the two have to be
  // the same binding or half of what a script sets goes missing.
  js::Object* global = interpreter_->Global();
  const Value window = Value::Obj(global);
  global->Set("window", window);
  global->Set("self", window);
  // Top-level browsing context: `top`/`parent`/`frames` are this window, and
  // `frameElement` is null. youtube's player reads `window.top.location` (and
  // `window === window.top`) on every load; without these the access throws
  // and reporting / bevasr / deid probes abort into catch paths.
  // Nested contexts are ADR 0027 — until they exist, every Window is top-level.
  global->Set("top", window);
  global->Set("parent", window);
  global->Set("frames", window);
  global->Set("frameElement", Value::Null());
  if (js::Value* document = interpreter_->GlobalScope()->Lookup("document")) {
    // So that `window.document` and `document` are the same object, which a
    // page checks more often than it looks.
    global->Set("document", *document);
  }
  interpreter_->GlobalScope()->Declare("window", window, false);

  // `location` is a real `Location` instance — not a plain object with own
  // parts. youtube (and every polyfill that does `Location.prototype`) needs
  // `location instanceof Location` and descriptors on the prototype; a missing
  // `Location` global is `typeof Location === "undefined"`.
  EnsureInterfaces();
  const Value location_prototype = MakeInterface("Location", Value::Undefined());
  const Value location = interpreter_->NewObjectValue();
  if (location.IsObject() && location_prototype.IsObject()) {
    location.object->SetPrototype(location_prototype.object);

    const auto href_of = [](const NativeCall& call) -> std::string {
      if (!call.self.IsObject()) {
        return {};
      }
      if (const Value* href = call.self.object->GetOwn("#href")) {
        return js::ToString(*href);
      }
      DomBindings* owner = OwnerOf(call);
      return owner == nullptr ? std::string() : owner->url_;
    };

    // Parts live on the prototype as accessors over `#href`, so
    // `WriteLocationFields` only has to refresh one slot and cannot drift from
    // `SplitHref` the way duplicated own properties could.
    const auto install_part = [this, &location_prototype, href_of](const char* name, auto pick) {
      const Value get = interpreter_->NewNativeValue(name, [href_of, pick](NativeCall& call) {
        return Value::String(pick(SplitHref(href_of(call))));
      });
      if (get.IsObject()) {
        get.object->Set(kOwnerSlot, PointerValue(this));
        location_prototype.object->DefineAccessor(name, get.object, nullptr);
      }
    };
    install_part("protocol", [](const HrefParts& p) { return p.protocol; });
    install_part("host", [](const HrefParts& p) { return p.host; });
    install_part("hostname", [](const HrefParts& p) { return p.hostname; });
    install_part("port", [](const HrefParts& p) { return p.port; });
    install_part("pathname", [](const HrefParts& p) { return p.pathname; });
    install_part("search", [](const HrefParts& p) { return p.search; });
    install_part("hash", [](const HrefParts& p) { return p.hash; });
    install_part("origin", [](const HrefParts& p) { return p.origin; });

    const Value to_string = interpreter_->NewNativeValue("toString", [href_of](NativeCall& call) {
      return Value::String(href_of(call));
    });
    if (to_string.IsObject()) {
      to_string.object->Set(kOwnerSlot, PointerValue(this));
      location_prototype.object->Set("toString", to_string);
    }

    // `assign` / `replace` / `reload` / writable `href` — ADR 0026 §3. Deferred
    // through HistorySource so the navigation runs after the turn ends.
    // youtube's consent Accept POSTs to consent.youtube.com/save, then
    // `location.reload()` so the watch page comes back with SOCS set; without
    // reload the cookie lands and the dialog stays forever.
    const auto install_nav = [this, &location_prototype](const char* name, bool replace) {
      const Value method = interpreter_->NewNativeValue(name, [replace](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->history_ == nullptr) {
          return Value::Undefined();
        }
        const std::string url = js::ToString(Argument(call.arguments, 0));
        owner->history_->RequestNavigation(url, replace);
        return Value::Undefined();
      });
      if (method.IsObject()) {
        method.object->Set(kOwnerSlot, PointerValue(this));
        location_prototype.object->Set(name, method);
      }
    };
    install_nav("assign", false);
    install_nav("replace", true);

    const Value reload = interpreter_->NewNativeValue("reload", [](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr || owner->history_ == nullptr) {
        return Value::Undefined();
      }
      if (!owner->url_.empty()) {
        owner->history_->RequestNavigation(owner->url_, true);
      }
      return Value::Undefined();
    });
    if (reload.IsObject()) {
      reload.object->Set(kOwnerSlot, PointerValue(this));
      location_prototype.object->Set("reload", reload);
    }

    const Value href_get = interpreter_->NewNativeValue("href", [href_of](NativeCall& call) {
      return Value::String(href_of(call));
    });
    const Value href_set = interpreter_->NewNativeValue("href", [](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr) {
        return Value::Undefined();
      }
      const std::string url = js::ToString(Argument(call.arguments, 0));
      if (owner->history_ != nullptr) {
        owner->history_->RequestNavigation(url, false);
      } else if (call.self.IsObject()) {
        // No history source (unit tests): update the slot in place so reads
        // still agree with what script wrote.
        call.self.object->Set("#href", Value::String(url));
      }
      return Value::Undefined();
    });
    if (href_get.IsObject() && href_set.IsObject()) {
      href_get.object->Set(kOwnerSlot, PointerValue(this));
      href_set.object->Set(kOwnerSlot, PointerValue(this));
      location_prototype.object->DefineAccessor("href", href_get.object, href_set.object);
    }

    WriteLocationFields(location);
    global->Set("location", location);
    interpreter_->GlobalScope()->Declare("location", location, false);
    // `document.location` is the same object as `window.location`, which is
    // what it is in a browser and what a page checks by identity. reddit's
    // interstitial reads `document.location.search`.
    if (js::Value* document = interpreter_->GlobalScope()->Lookup("document")) {
      if (document->IsObject()) {
        document->object->Set("location", location);
      }
    }
  }

  InstallUrlSearchParams();
  InstallComputedStyle();

  // `navigator`, with one property and a deliberate one.
  //
  // The user agent is a fingerprinting surface before it is anything else, and
  // the string here says what this browser is and nothing about the machine it
  // is on -- no platform, no version of anything installed, no build date. A
  // page that varies its markup by user agent gets one answer from every copy
  // of this browser, which is the point.
  //
  // It is the same constant net sends as the `User-Agent` header, and it is
  // shared rather than repeated because a page may sniff both: two constants
  // would eventually disagree, and a page that renders one way and scripts
  // another is a bug nobody would look for here.
  const Value navigator = interpreter_->NewObjectValue();
  if (navigator.IsObject()) {
    navigator.object->Set("userAgent", Value::String(std::string(util::kUserAgent)));
    // The rest of ADR 0029 §6's table, plus the permission-gated APIs. In its own translation unit
    // because the *answers* are a policy with reasons attached and this file is about the window.
    InstallPrivacyAnswers(navigator);
    global->Set("navigator", navigator);
    interpreter_->GlobalScope()->Declare("navigator", navigator, false);
  }
  InstallNotification();
  InstallCrypto();
  InstallTextEncoding();
  InstallScreenAndPixelRatio();
  const Value post_message = interpreter_->NewNativeValue("postMessage", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Undefined();
    }
    owner->DeliverWindowMessage(Argument(call.arguments, 0));
    return Value::Undefined();
  });
  if (post_message.IsObject()) {
    post_message.object->Set(kOwnerSlot, PointerValue(this));
    global->Set("postMessage", post_message);
    interpreter_->GlobalScope()->Declare("postMessage", post_message, false);
  }
}

void DomBindings::WriteLocationFields(const js::Value& location) {
  if (!location.IsObject()) {
    return;
  }
  // One slot: prototype accessors re-split it. Writing the parts as own
  // properties again would shadow the accessors and drift on the next navigate.
  location.object->Set("#href", Value::String(url_));
  if (js::Value* document = interpreter_->GlobalScope()->Lookup("document")) {
    if (document->IsObject()) {
      document->object->Set("URL", Value::String(url_));
    }
  }
}

void DomBindings::InstallUrlConstructor() {
  // `new URL(href[, base])`, and the properties `location` already reports.
  //
  // It exists because `URL.createObjectURL` has to hang off something (ADR 0028 §3, session 28) and a
  // `URL` object that was not constructible would be the stub ADR 0012 forbids. The parse is *not*
  // here: it goes through `NetworkSource::ResolveUrl` to the one parser in `src/url`, and only the
  // splitting of the canonical result happens in this module -- the same division of labour, and for
  // the same reason, as `SplitHref` (BindingSupport.h / HrefParts.cpp).
  if (interpreter_ == nullptr || network_ == nullptr) {
    // No network source means no resolver, and a `URL` that could not parse would answer about
    // nothing. Absent instead.
    return;
  }
  if (const Value* existing = interpreter_->Global()->Get("URL");
      existing != nullptr && existing->IsObject()) {
    return;  // Already installed: this is called from two places and must be idempotent.
  }
  const Value prototype = interpreter_->NewObjectValue();
  const Value constructor = interpreter_->NewNativeValue("URL", [this, prototype](NativeCall& call) {
    const std::string relative = js::ToString(Argument(call.arguments, 0));
    const Value base_argument = Argument(call.arguments, 1);
    // An absent base means the document's own address, which is what a relative URL is relative to.
    const std::string base =
        base_argument.IsUndefined() ? url_ : js::ToString(base_argument);
    const std::string resolved = network_->ResolveUrl(relative, base);
    if (resolved.empty()) {
      // The specification throws `TypeError` for a URL that does not parse, and pages depend on it:
      // `try { new URL(s) } catch { /* not a URL */ }` is the idiomatic validity test.
      return call.Throw("TypeError", "Failed to construct URL: invalid URL");
    }
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (prototype.IsObject()) {
      object.object->SetPrototype(prototype.object);
    }
    const HrefParts address = SplitHref(resolved);
    object.object->Set("href", Value::String(resolved));
    object.object->Set("protocol", Value::String(address.protocol));
    object.object->Set("host", Value::String(address.host));
    object.object->Set("hostname", Value::String(address.hostname));
    object.object->Set("port", Value::String(address.port));
    object.object->Set("pathname", Value::String(address.pathname));
    object.object->Set("search", Value::String(address.search));
    object.object->Set("hash", Value::String(address.hash));
    object.object->Set("origin", Value::String(address.origin));
    return object;
  });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, PointerValue(this));
  if (prototype.IsObject()) {
    // `toString` and `toJSON` both answer with `href`, which is what makes a URL usable everywhere a
    // string is -- `fetch(new URL(...))` is the common case and it goes through ToString.
    const Value to_string = interpreter_->NewNativeValue("toString", [](NativeCall& inner) {
      const Value* href = inner.self.IsObject() ? inner.self.object->GetOwn("href") : nullptr;
      return href == nullptr ? Value::String("") : *href;
    });
    if (to_string.IsObject()) {
      prototype.object->Set("toString", to_string);
      prototype.object->Set("toJSON", to_string);
    }
    constructor.object->Set("prototype", prototype);
  }
  interpreter_->Global()->Set("URL", constructor);
  interpreter_->GlobalScope()->Declare("URL", constructor, false);
}

void DomBindings::SetDocumentUrl(std::string url) {
  url_ = std::move(url);
  if (interpreter_ == nullptr) {
    return;
  }
  // The *existing* location object, rewritten in place. A page holds a reference
  // to it -- `document.location === window.location` is something pages check --
  // so a same-document navigation that replaced the object would make every such
  // reference stale. ADR 0026 §2: the address a page reads has to be the address
  // the URL bar shows, and there is one of each.
  const Value* location = interpreter_->GlobalScope()->Lookup("location");
  if (location != nullptr) {
    WriteLocationFields(*location);
  }
}

}  // namespace microbrowser::bindings
