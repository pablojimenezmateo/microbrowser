// `URL`, `location`'s components, and the URL decomposition attributes on `<a>` and `<area>` —
// all three over the one parser in `src/url`.
//
// **The point of this file is that there is only one of it.** What it replaces was a string cut
// (`SplitHref`, deleted with this): find the first colon, find the next two slashes, and call
// whatever follows the host. That has no idea what a special scheme is, cannot resolve `..`, does
// not know that `https://x:443/` and `https://x/` are one origin, and answered every one of
// `URL`, `location.pathname` and `a.host` differently from the parser every fetch, cookie scope
// and CSP decision already goes through. Two answers to "what does this URL mean" is a security
// bug rather than a duplication — see src/url/Url.h.
//
// **The state is the href, and the object is re-parsed on every access.** No `url::Url` is kept in
// a JavaScript object, because a C++ value in one is state the collector cannot see and this
// module has had that bug once. Re-parsing is sound rather than merely convenient: the standard's
// serializer round-trips, so the href a getter reads is the same URL a setter wrote — and it means
// there is no second representation to fall out of step with the string a page can read.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "dom/Node.h"
#include "url/Url.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where a `URL` object keeps its href, and where a `URLSearchParams` made by `url.searchParams`
// keeps the URL it belongs to. Hidden slots, so neither shows up in `Object.keys`.
constexpr const char* kHrefSlot = "#href";
constexpr const char* kSearchParamsSlot = "#searchParams";
constexpr const char* kOwningUrlSlot = "#url";

// The components a page can read, and the eleven names are the standard's. `Origin` has no setter,
// which is why it is at the end rather than interleaved.
enum class Part {
  Href,
  Protocol,
  Username,
  Password,
  Host,
  Hostname,
  Port,
  Pathname,
  Search,
  Hash,
  Origin,
};

std::string ReadPart(const url::Url& url, Part part) {
  switch (part) {
    case Part::Href: return url.Href();
    case Part::Protocol: return url.Protocol();
    case Part::Username: return url.Username();
    case Part::Password: return url.Password();
    case Part::Host: return url.HostPort();
    case Part::Hostname: return url.Hostname();
    case Part::Port: return url.PortString();
    case Part::Pathname: return url.Pathname();
    case Part::Search: return url.Search();
    case Part::Hash: return url.Hash();
    case Part::Origin: return url.OriginString();
  }
  return {};
}

// Applies one setter. False only for `href`, which is the only one the standard makes fail — every
// other setter is defined to leave the URL alone rather than to complain.
bool WritePart(url::Url& url, Part part, std::string_view value) {
  switch (part) {
    case Part::Href: return url.SetHref(value);
    case Part::Protocol: url.SetProtocol(value); break;
    case Part::Username: url.SetUsername(value); break;
    case Part::Password: url.SetPassword(value); break;
    case Part::Host: url.SetHost(value); break;
    case Part::Hostname: url.SetHostname(value); break;
    case Part::Port: url.SetPort(value); break;
    case Part::Pathname: url.SetPathname(value); break;
    case Part::Search: url.SetSearch(value); break;
    case Part::Hash: url.SetHash(value); break;
    case Part::Origin: break;  // readonly
  }
  return true;
}

const char* PartName(Part part) {
  switch (part) {
    case Part::Href: return "href";
    case Part::Protocol: return "protocol";
    case Part::Username: return "username";
    case Part::Password: return "password";
    case Part::Host: return "host";
    case Part::Hostname: return "hostname";
    case Part::Port: return "port";
    case Part::Pathname: return "pathname";
    case Part::Search: return "search";
    case Part::Hash: return "hash";
    case Part::Origin: return "origin";
  }
  return "";
}

constexpr Part kAllParts[] = {
    Part::Href,     Part::Protocol, Part::Username, Part::Password,
    Part::Host,     Part::Hostname, Part::Port,     Part::Pathname,
    Part::Search,   Part::Hash,     Part::Origin,
};

dom::Element* ElementOf(const js::Value& value) {
  dom::Node* node = NodeOf(value);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

std::string HrefOf(const Value& self) {
  if (!self.IsObject()) {
    return {};
  }
  const Value* href = self.object->GetOwn(kHrefSlot);
  return href == nullptr ? std::string() : js::ToString(*href);
}

// The first `<base>` element in tree order, which is what the HTML Standard names -- a second one
// is inert rather than an override.
const dom::Element* FindFirstBaseElement(const dom::Node& node) {
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    if (child->IsElement()) {
      const auto* element = static_cast<const dom::Element*>(child.get());
      if (element->LocalName() == "base") {
        return element;
      }
    }
    if (const dom::Element* found = FindFirstBaseElement(*child); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

// The document base URL, computed from the tree rather than remembered.
//
// It has to be live: `<base href>` is an attribute a script can rewrite between two calls, and the
// URL Standard's own test page does exactly that — it sets `#base`.href before resolving each of
// nine hundred hrefs. A base captured once at parse would answer about the first of them for all
// nine hundred.
std::string DomBindings::DocumentBaseUrl(const dom::Document* document) const {
  if (document != nullptr) {
    // The *first* base element with an href attribute, which is what the HTML Standard says; a
    // second one is inert rather than an override.
    if (const dom::Element* base = FindFirstBaseElement(*document); base != nullptr) {
      if (const std::string* href = base->GetAttribute("href"); href != nullptr) {
        const std::optional<url::Url> fallback = url::Url::Parse(url_);
        const std::optional<url::Url> parsed =
            fallback.has_value() ? url::Url::Parse(*href, *fallback) : url::Url::Parse(*href);
        if (parsed.has_value()) {
          return parsed->Serialize();
        }
      }
    }
  }
  return url_;
}

void DomBindings::InstallUrlConstructor() {
  // `new URL(href[, base])`. Unlike almost everything else here it needs no engine behind it: the
  // parser is a pure function of two strings, so a `URL` works in a document with no loader.
  if (interpreter_ == nullptr) {
    return;
  }
  if (const Value* existing = interpreter_->Global()->Get("URL");
      existing != nullptr && existing->IsObject()) {
    return;  // Already installed: this is called from two places and must be idempotent.
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }

  // Parses the two arguments the way the constructor and the two statics all need to. Nullopt is
  // "this is not a URL", which each caller reports differently: a throw, a null, or a false.
  const auto parse_arguments = [](NativeCall& call,
                                  bool& threw) -> std::optional<url::Url> {
    threw = false;
    // Must coerce through `toString`: `js::ToString(location)` is "[object Object]", which then
    // resolves as a path against the document base — `https://www.youtube.com/[object%20Object]` —
    // and that string is what youtube put in consent.youtube.com's `continue=` parameter.
    std::string relative;
    if (!CoerceToUsvString(call, Argument(call.arguments, 0), relative)) {
      threw = true;
      return std::nullopt;
    }
    const Value base_argument = Argument(call.arguments, 1);
    if (base_argument.IsUndefined()) {
      return url::Url::Parse(relative);
    }
    std::string base;
    if (!CoerceToUsvString(call, base_argument, base)) {
      threw = true;
      return std::nullopt;
    }
    const std::optional<url::Url> parsed_base = url::Url::Parse(base);
    if (!parsed_base.has_value()) {
      return std::nullopt;  // a base that is not a URL fails the whole call, per the standard
    }
    return url::Url::Parse(relative, *parsed_base);
  };

  const Value constructor =
      interpreter_->NewNativeValue("URL", [this, prototype, parse_arguments](NativeCall& call) {
        bool threw = false;
        const std::optional<url::Url> parsed = parse_arguments(call, threw);
        if (threw) {
          return call.ThrownValue();
        }
        if (!parsed.has_value()) {
          // The specification throws `TypeError` for a URL that does not parse, and pages depend on
          // it: `try { new URL(s) } catch { /* not a URL */ }` is the idiomatic validity test.
          return call.Throw("TypeError", "Failed to construct URL: invalid URL");
        }
        const Value object = call.interpreter.NewObjectValue();
        if (!object.IsObject()) {
          return Value::Undefined();
        }
        object.object->SetPrototype(prototype.object);
        object.object->SetHidden(kHrefSlot, Value::String(parsed->Serialize()));
        object.object->Set(kOwnerSlot, PointerValue(this));
        // Not clonable: a `URL` is a handle, and a structured clone of one would be a plain object
        // carrying its href with none of its behaviour. The standard's answer is a DataCloneError.
        object.object->MarkHostObject();
        return object;
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, PointerValue(this));

  // The components, as accessors on the prototype over the one href slot. On the prototype rather
  // than as own properties so that a setter cannot be shadowed by the value it wrote.
  for (const Part part : kAllParts) {
    const Value get = interpreter_->NewNativeValue(PartName(part), [part](NativeCall& call) {
      const std::optional<url::Url> url = url::Url::Parse(HrefOf(call.self));
      return Value::String(url.has_value() ? ReadPart(*url, part) : std::string());
    });
    if (!get.IsObject()) {
      continue;
    }
    if (part == Part::Origin) {
      prototype.object->DefineAccessor(PartName(part), get.object, nullptr);
      continue;
    }
    const Value set =
        interpreter_->NewNativeValue(PartName(part), [this, part](NativeCall& call) -> Value {
          std::string value;
          if (!CoerceToUsvString(call, Argument(call.arguments, 0), value)) {
            return call.ThrownValue();
          }
          std::optional<url::Url> url = url::Url::Parse(HrefOf(call.self));
          if (!url.has_value()) {
            return Value::Undefined();
          }
          if (!WritePart(*url, part, value)) {
            // Only `href` gets here, and only the standard makes it throw.
            return call.Throw("TypeError", "Failed to set the 'href' property on 'URL'");
          }
          if (call.self.IsObject()) {
            call.self.object->SetHidden(kHrefSlot, Value::String(url->Serialize()));
            RefreshUrlSearchParams(call.self);
          }
          return Value::Undefined();
        });
    if (set.IsObject()) {
      set.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->DefineAccessor(PartName(part), get.object, set.object);
    }
  }

  // `searchParams` is a *live* object rather than a snapshot: the standard says a URL and its
  // search params are two views of one query, and pages rely on `url.searchParams.set(...)`
  // changing `url.href`. The link is the pair of slots below — the URL holds its params object,
  // the params object holds the URL — and it is created lazily, because most URLs never grow one.
  const Value search_params_get =
      interpreter_->NewNativeValue("searchParams", [this](NativeCall& call) -> Value {
        if (!call.self.IsObject()) {
          return Value::Undefined();
        }
        if (const Value* existing = call.self.object->GetOwn(kSearchParamsSlot);
            existing != nullptr && existing->IsObject()) {
          return *existing;
        }
        const std::optional<url::Url> url = url::Url::Parse(HrefOf(call.self));
        const Value params = MakeUrlSearchParams(url.has_value() ? url->Query() : std::string());
        if (params.IsObject()) {
          params.object->SetHidden(kOwningUrlSlot, call.self);
          call.self.object->SetHidden(kSearchParamsSlot, params);
        }
        return params;
      });
  if (search_params_get.IsObject()) {
    search_params_get.object->Set(kOwnerSlot, PointerValue(this));
    prototype.object->DefineAccessor("searchParams", search_params_get.object, nullptr);
  }

  // `toString` and `toJSON` both answer with `href`, which is what makes a URL usable everywhere a
  // string is — `fetch(new URL(...))` is the common case and it goes through ToString.
  const Value to_string = interpreter_->NewNativeValue("toString", [](NativeCall& call) {
    return Value::String(HrefOf(call.self));
  });
  if (to_string.IsObject()) {
    prototype.object->Set("toString", to_string);
    prototype.object->Set("toJSON", to_string);
  }
  constructor.object->Set("prototype", prototype);

  // `URL.parse` and `URL.canParse`: the same question the constructor answers, without the throw.
  // They exist because the throwing form is what pages actually use to *test* a string, and a
  // try/catch per candidate URL is expensive enough that the standard grew alternatives.
  const Value can_parse = interpreter_->NewNativeValue(
      "canParse", [parse_arguments](NativeCall& call) -> Value {
        if (call.arguments.empty()) {
          return call.Throw("TypeError", "URL.canParse requires an argument");
        }
        bool threw = false;
        const std::optional<url::Url> parsed = parse_arguments(call, threw);
        if (threw) {
          return call.ThrownValue();
        }
        return Value::Bool(parsed.has_value());
      });
  if (can_parse.IsObject()) {
    constructor.object->Set("canParse", can_parse);
  }
  const Value parse_static = interpreter_->NewNativeValue(
      "parse", [this, prototype, parse_arguments](NativeCall& call) -> Value {
        if (call.arguments.empty()) {
          return call.Throw("TypeError", "URL.parse requires an argument");
        }
        bool threw = false;
        const std::optional<url::Url> parsed = parse_arguments(call, threw);
        if (threw) {
          return call.ThrownValue();
        }
        if (!parsed.has_value()) {
          return Value::Null();
        }
        const Value object = call.interpreter.NewObjectValue();
        if (!object.IsObject()) {
          return Value::Null();
        }
        object.object->SetPrototype(prototype.object);
        object.object->SetHidden(kHrefSlot, Value::String(parsed->Serialize()));
        object.object->Set(kOwnerSlot, PointerValue(this));
        // Not clonable: a `URL` is a handle, and a structured clone of one would be a plain object
        // carrying its href with none of its behaviour. The standard's answer is a DataCloneError.
        object.object->MarkHostObject();
        return object;
      });
  if (parse_static.IsObject()) {
    parse_static.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("parse", parse_static);
  }

  interpreter_->Global()->Set("URL", constructor);
  interpreter_->GlobalScope()->Declare("URL", constructor, false);
}

// Called after a mutating `URLSearchParams` method: writes the serialized list back into the URL
// that owns it. The other direction — a setter on the URL refreshing the params — is
// `RefreshUrlSearchParams`. Both exist because the two objects are one query, and a page that
// changed either and read the other would otherwise see the state from before its own write.
void DomBindings::WriteBackUrlSearchParams(const js::Value& params, const std::string& serialized) {
  if (!params.IsObject()) {
    return;
  }
  const Value* owning = params.object->GetOwn(kOwningUrlSlot);
  if (owning == nullptr || !owning->IsObject()) {
    return;
  }
  std::optional<url::Url> url = url::Url::Parse(HrefOf(*owning));
  if (!url.has_value()) {
    return;
  }
  // An empty list clears the query rather than leaving a bare `?`, which is what the standard's
  // "if query is the empty string, set url's query to null" step is for.
  url->SetQuery(serialized.empty() ? std::optional<std::string>() : std::optional(serialized));
  owning->object->SetHidden(kHrefSlot, Value::String(url->Serialize()));
}

void DomBindings::RefreshUrlSearchParams(const js::Value& url_object) {
  if (!url_object.IsObject()) {
    return;
  }
  const Value* params = url_object.object->GetOwn(kSearchParamsSlot);
  if (params == nullptr || !params->IsObject()) {
    return;
  }
  const std::optional<url::Url> url = url::Url::Parse(HrefOf(url_object));
  ResetUrlSearchParams(*params, url.has_value() ? url->Query() : std::string());
}

// `HTMLBaseElement.href`, which is *not* an ordinary reflected attribute and cannot be one.
//
// It reads back absolute — the content attribute resolved against the document's fallback base URL,
// which is the document's own address rather than the base element's own answer, because a base
// that resolved against itself would be circular. Without this, `base.href = x` set a plain
// property on the wrapper and every link on the page kept resolving against the document address:
// the URL Standard's own test page sets `<base>` before each of nine hundred link resolutions and
// got the same answer for all of them.
void DomBindings::InstallBaseElementHref() {
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value* prototype = interfaces_.object->GetOwn("HTMLBaseElement");
  if (prototype == nullptr || !prototype->IsObject()) {
    return;
  }
  const Value get = interpreter_->NewNativeValue("href", [this](NativeCall& call) {
    dom::Element* element = ElementOf(call.self);
    const std::string* href = element == nullptr ? nullptr : element->GetAttribute("href");
    if (href == nullptr) {
      return Value::String(url_);  // no attribute: the document's own address
    }
    const std::optional<url::Url> fallback = url::Url::Parse(url_);
    const std::optional<url::Url> parsed =
        fallback.has_value() ? url::Url::Parse(*href, *fallback) : url::Url::Parse(*href);
    // A base that does not parse answers with what was written, and resolves nothing.
    return Value::String(parsed.has_value() ? parsed->Serialize() : *href);
  });
  const Value set = interpreter_->NewNativeValue("href", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Element* element = ElementOf(call.self);
    if (owner == nullptr || element == nullptr) {
      return Value::Undefined();
    }
    std::string href;
    if (!CoerceToString(call, Argument(call.arguments, 0), href)) {
      return call.ThrownValue();
    }
    owner->SetElementAttribute(*element, "href", href);
    return Value::Undefined();
  });
  if (get.IsObject() && set.IsObject()) {
    get.object->Set(kOwnerSlot, PointerValue(this));
    set.object->Set(kOwnerSlot, PointerValue(this));
    prototype->object->DefineAccessor("href", get.object, set.object);
  }
}

void DomBindings::InstallHyperlinkElementUtils() {
  InstallBaseElementHref();
  // HTMLHyperlinkElementUtils, on both elements that have it. `<area>` is not an afterthought: the
  // standard defines the two together, and the URL Standard's own setter tests run every case
  // against both — a decomposition attribute that worked on `<a>` and not on `<area>` is a page
  // whose image map silently navigates somewhere else.
  //
  // youtube's searchbox resolves `location.href` through `document.createElement('a'); a.href =
  // url; a.pathname` (`n0n`). Without `pathname` that call threw and Enter never navigated
  // (TD-0026).
  if (!interfaces_.IsObject()) {
    return;
  }
  for (const char* interface_name : {"HTMLAnchorElement", "HTMLAreaElement"}) {
    const Value* prototype = interfaces_.object->GetOwn(interface_name);
    if (prototype == nullptr || !prototype->IsObject()) {
      continue;
    }

    // "Reinitialize url": the href content attribute, resolved against the document base URL.
    // Nullopt when there is no attribute or it does not parse, and the two are different — the
    // href getter answers "" for the first and the attribute's own text for the second.
    const auto url_of = [this](const Value& self) -> std::optional<url::Url> {
      dom::Element* element = ElementOf(self);
      if (element == nullptr) {
        return std::nullopt;
      }
      const std::string* href = element->GetAttribute("href");
      if (href == nullptr) {
        return std::nullopt;
      }
      // The attribute is a DOMString and may hold a lone surrogate; the URL parser's input is a
      // scalar value string, so the conversion happens here rather than at the setter -- the
      // attribute keeps what was written, and only the *parse* sees U+FFFD.
      const std::string text = util::ScrubLoneSurrogates(*href);
      const std::optional<url::Url> base = url::Url::Parse(DocumentBaseUrl(element->NodeDocument()));
      return base.has_value() ? url::Url::Parse(text, *base) : url::Url::Parse(text);
    };

    const Value href_get =
        interpreter_->NewNativeValue("href", [this, url_of](NativeCall& call) -> Value {
          const std::optional<url::Url> url = url_of(call.self);
          if (url.has_value()) {
            return Value::String(url->Serialize());
          }
          // A URL that did not parse answers with the attribute *as written*, which is what makes
          // the standard's own failure check work: `a.href === input` after `a.href = garbage`.
          dom::Element* element = ElementOf(call.self);
          const std::string* href =
              element == nullptr ? nullptr : element->GetAttribute("href");
          return Value::String(href == nullptr ? std::string() : *href);
        });
    const Value href_set = interpreter_->NewNativeValue("href", [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = ElementOf(call.self);
      if (owner == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      std::string href;
      if (!CoerceToString(call, Argument(call.arguments, 0), href)) {
        return call.ThrownValue();
      }
      owner->SetElementAttribute(*element, "href", href);
      return Value::Undefined();
    });
    if (href_get.IsObject() && href_set.IsObject()) {
      href_get.object->Set(kOwnerSlot, PointerValue(this));
      href_set.object->Set(kOwnerSlot, PointerValue(this));
      prototype->object->DefineAccessor("href", href_get.object, href_set.object);
    }

    for (const Part part : kAllParts) {
      if (part == Part::Href) {
        continue;  // installed above: its failure answer is not the empty string
      }
      const Value get =
          interpreter_->NewNativeValue(PartName(part), [part, url_of](NativeCall& call) {
            const std::optional<url::Url> url = url_of(call.self);
            if (!url.has_value()) {
              // The standard spells these out one by one, and `protocol` is the odd one: a
              // hyperlink with no URL still has a colon. It is what the URL Standard's own test
              // page checks to decide that parsing failed.
              return Value::String(part == Part::Protocol ? ":" : "");
            }
            return Value::String(ReadPart(*url, part));
          });
      if (!get.IsObject()) {
        continue;
      }
      get.object->Set(kOwnerSlot, PointerValue(this));
      if (part == Part::Origin) {
        prototype->object->DefineAccessor(PartName(part), get.object, nullptr);
        continue;
      }
      const Value set = interpreter_->NewNativeValue(
          PartName(part), [part, url_of](NativeCall& call) -> Value {
            DomBindings* owner = OwnerOf(call);
            dom::Element* element = ElementOf(call.self);
            if (owner == nullptr || element == nullptr) {
              return Value::Undefined();
            }
            std::string value;
            if (!CoerceToUsvString(call, Argument(call.arguments, 0), value)) {
              return call.ThrownValue();
            }
            std::optional<url::Url> url = url_of(call.self);
            if (!url.has_value()) {
              return Value::Undefined();  // no URL to decompose, so nothing to set
            }
            WritePart(*url, part, value);
            // "Update href": the whole serialized URL goes back into the content attribute, so the
            // element and its href agree afterwards.
            owner->SetElementAttribute(*element, "href", url->Serialize());
            return Value::Undefined();
          });
      if (set.IsObject()) {
        set.object->Set(kOwnerSlot, PointerValue(this));
        prototype->object->DefineAccessor(PartName(part), get.object, set.object);
      }
    }
  }
}

void DomBindings::InstallLocationParts(const js::Value& location_prototype) {
  // `location`'s components, over the same parser and the same eleven names. The href lives in one
  // slot that `WriteLocationFields` refreshes, so a navigation moves every component at once and
  // none of them can drift from the address bar.
  if (!location_prototype.IsObject()) {
    return;
  }
  const auto href_of = [](const NativeCall& call) -> std::string {
    if (!call.self.IsObject()) {
      return {};
    }
    if (const Value* href = call.self.object->GetOwn(kHrefSlot)) {
      return js::ToString(*href);
    }
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? std::string() : owner->url_;
  };
  for (const Part part : kAllParts) {
    if (part == Part::Href) {
      continue;  // `location.href` is installed with the navigation setters, in WindowBindings
    }
    const Value get =
        interpreter_->NewNativeValue(PartName(part), [part, href_of](NativeCall& call) {
          const std::optional<url::Url> url = url::Url::Parse(href_of(call));
          return Value::String(url.has_value() ? ReadPart(*url, part) : std::string());
        });
    if (get.IsObject()) {
      get.object->Set(kOwnerSlot, PointerValue(this));
      location_prototype.object->DefineAccessor(PartName(part), get.object, nullptr);
    }
  }
}

}  // namespace microbrowser::bindings
