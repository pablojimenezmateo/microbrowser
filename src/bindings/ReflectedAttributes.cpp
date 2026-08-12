// The IDL attributes that reflect content attributes.
//
// `el.value = 'x'` and `el.setAttribute('value', 'x')` are the same act, and a
// browser where they are not is one where half a page's writes land somewhere
// nothing reads. Before this file every one of these was a plain property on
// the wrapper object: the assignment succeeded, the value read back, and the
// element it was supposed to describe never changed -- so a form submitted
// without it, the cascade never saw the class, and nothing anywhere reported a
// problem.
//
// It is the shape of the failure that makes it worth its own file rather than
// another handful of accessors: the survey's reddit challenge is
//
//     Object.assign(document.createElement("input"), {name: n, type: "hidden", value: e})
//
// which is three reflected attributes in one expression and no `setAttribute`
// anywhere. Getting `Object.assign` to reach a DOM element is not a special
// case -- it is [[Set]] finding a setter on the prototype chain, which is
// exactly what these are.
//
// The table is deliberately not the whole of HTML. It is the attributes a page
// writes through the property rather than through `setAttribute`, which is a
// much shorter list and one that can be checked by reading it.

#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

enum class Reflect : std::uint8_t {
  // A string both ways: the attribute's value, or "" when it is absent.
  Text,
  // Presence: `disabled` is true when the attribute is there whatever it says,
  // and setting it false removes the attribute rather than writing "false".
  Presence,
  // A string, with `type` defaulting to "text" when absent -- which is what a
  // page that branches on `input.type` expects, and the one default worth
  // spelling out because a missing `type` *is* a text input.
  InputType,
  // The one that is not a content attribute at all: a textarea's value is its
  // text. Read as the `value` attribute when something set one and as the
  // element's text otherwise, which is exactly what the engine's own
  // ControlValue does -- the two have to agree or a page reads back something
  // different from what it submits.
  TextareaValue,
};

struct Reflection {
  const char* interface;
  const char* property;
  const char* attribute;
  Reflect kind;
};

constexpr Reflection kReflections[] = {
    {"Element", "id", "id", Reflect::Text},
    {"Element", "className", "class", Reflect::Text},
    {"HTMLElement", "title", "title", Reflect::Text},
    {"HTMLElement", "lang", "lang", Reflect::Text},
    {"HTMLElement", "dir", "dir", Reflect::Text},
    // Presence, and the reason youtube's expandable metadata used to stay open:
    // Polymer writes `el.hidden = !isExpanded` from `hidden="[[!isExpanded]]"`.
    // Without a setter that reaches the content attribute the assignment was an
    // expando, the cascade never saw `[hidden]`, and search rows grew to ~900px.
    {"HTMLElement", "hidden", "hidden", Reflect::Presence},

    // `href` is HTMLHyperlinkElementUtils — see InstallHyperlinkElementUtils.
    {"HTMLAnchorElement", "target", "target", Reflect::Text},
    {"HTMLAnchorElement", "rel", "rel", Reflect::Text},

    {"HTMLImageElement", "src", "src", Reflect::Text},
    {"HTMLImageElement", "alt", "alt", Reflect::Text},

    {"HTMLInputElement", "name", "name", Reflect::Text},
    {"HTMLInputElement", "type", "type", Reflect::InputType},
    {"HTMLInputElement", "value", "value", Reflect::Text},
    {"HTMLInputElement", "placeholder", "placeholder", Reflect::Text},
    {"HTMLInputElement", "checked", "checked", Reflect::Presence},
    {"HTMLInputElement", "disabled", "disabled", Reflect::Presence},
    {"HTMLInputElement", "required", "required", Reflect::Presence},
    {"HTMLInputElement", "readOnly", "readonly", Reflect::Presence},

    {"HTMLButtonElement", "name", "name", Reflect::Text},
    {"HTMLButtonElement", "type", "type", Reflect::Text},
    {"HTMLButtonElement", "value", "value", Reflect::Text},
    {"HTMLButtonElement", "disabled", "disabled", Reflect::Presence},

    {"HTMLSelectElement", "name", "name", Reflect::Text},
    {"HTMLSelectElement", "disabled", "disabled", Reflect::Presence},
    {"HTMLSelectElement", "multiple", "multiple", Reflect::Presence},
    {"HTMLSelectElement", "required", "required", Reflect::Presence},

    {"HTMLOptionElement", "value", "value", Reflect::Text},
    {"HTMLOptionElement", "label", "label", Reflect::Text},
    {"HTMLOptionElement", "selected", "selected", Reflect::Presence},
    {"HTMLOptionElement", "disabled", "disabled", Reflect::Presence},

    {"HTMLTextAreaElement", "name", "name", Reflect::Text},
    {"HTMLTextAreaElement", "placeholder", "placeholder", Reflect::Text},
    {"HTMLTextAreaElement", "value", "value", Reflect::TextareaValue},
    {"HTMLTextAreaElement", "disabled", "disabled", Reflect::Presence},
    {"HTMLTextAreaElement", "readOnly", "readonly", Reflect::Presence},

    {"HTMLFormElement", "action", "action", Reflect::Text},
    {"HTMLFormElement", "method", "method", Reflect::Text},
    {"HTMLFormElement", "enctype", "enctype", Reflect::Text},
    {"HTMLFormElement", "target", "target", Reflect::Text},
    {"HTMLFormElement", "name", "name", Reflect::Text},

    {"HTMLScriptElement", "src", "src", Reflect::Text},
    {"HTMLScriptElement", "type", "type", Reflect::Text},
    {"HTMLScriptElement", "nonce", "nonce", Reflect::Text},
    {"HTMLScriptElement", "defer", "defer", Reflect::Presence},
    {"HTMLScriptElement", "async", "async", Reflect::Presence},
    {"HTMLScriptElement", "noModule", "nomodule", Reflect::Presence},

    {"HTMLLinkElement", "href", "href", Reflect::Text},
    {"HTMLLinkElement", "rel", "rel", Reflect::Text},
    {"HTMLLinkElement", "type", "type", Reflect::Text},

    {"HTMLIFrameElement", "src", "src", Reflect::Text},
    // `<video>` and `<audio>`'s `src`, which was missing -- so `video.src = url` set a plain JavaScript
    // property on the wrapper and the element never saw it. Found by an MSE page whose `sourceopen`
    // never fired: the attach hook in SetElementAttribute was right and nothing was reaching it.
    //
    // On both interfaces rather than a shared `HTMLMediaElement`, because this engine's interface
    // table is flat -- the same reason `InstallMediaElement` is called twice.
    {"HTMLVideoElement", "src", "src", Reflect::Text},
    {"HTMLVideoElement", "preload", "preload", Reflect::Text},
    {"HTMLVideoElement", "poster", "poster", Reflect::Text},
    {"HTMLAudioElement", "src", "src", Reflect::Text},
    {"HTMLAudioElement", "preload", "preload", Reflect::Text},
    {"HTMLIFrameElement", "name", "name", Reflect::Text},
};

dom::Element* ElementOf(const js::Value& value) {
  dom::Node* node = NodeOf(value);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

}  // namespace

void DomBindings::SetElementAttribute(dom::Element& element, const std::string& name,
                                      const std::string& value) {
  // The old value is read before the write, because that is what a reaction is
  // given and there is no second chance to ask.
  const std::string* previous = element.GetAttribute(name);
  const Value old_value = previous == nullptr ? Value::Null() : Value::String(*previous);
  element.SetAttribute(name, value);
  AfterAttributeWrite(element, name, old_value, Value::String(value));
}

void DomBindings::SetElementAttributeNS(dom::Element& element, dom::NamespaceRef name_space,
                                        const std::string& qualified_name,
                                        std::uint32_t prefix_length, const std::string& value) {
  const std::string_view local =
      std::string_view(qualified_name)
          .substr(prefix_length == 0 || prefix_length >= qualified_name.size()
                      ? 0
                      : prefix_length + 1);
  const dom::Attribute* previous = element.GetAttributeNS(name_space, local);
  const Value old_value = previous == nullptr ? Value::Null() : Value::String(previous->value);
  const std::string local_name(local);
  const std::string uri(name_space.Uri());
  element.SetAttributeNS(std::move(name_space), qualified_name, prefix_length, value);
  // The reaction and the record are told the *local* name, which is what the
  // specification hands `attributeChangedCallback` beside the namespace.
  AfterAttributeWrite(element, local_name, old_value, Value::String(value), uri);
}

void DomBindings::RemoveElementAttributeNS(dom::Element& element,
                                           const dom::NamespaceRef& name_space,
                                           std::string_view local_name) {
  const dom::Attribute* previous = element.GetAttributeNS(name_space, local_name);
  if (previous == nullptr) {
    return;
  }
  const Value old_value = Value::String(previous->value);
  const std::string name(local_name);
  const std::string uri(name_space.Uri());
  element.RemoveAttributeNS(name_space, local_name);
  RunAttributeReaction(element, name, old_value, Value::Null());
  RecordMutation(element, "attributes", name, old_value, {}, {}, uri);
}

void DomBindings::AfterAttributeWrite(dom::Element& element, const std::string& name,
                                      const js::Value& old_value, const js::Value& new_value,
                                      std::string_view attribute_namespace) {
  const std::string value = new_value.IsString() ? *new_value.string : std::string();
  // **A media element's `src` set to an object URL is an *attach*, not a fetch.** This is the one path
  // by which a `MediaSource` reaches an element -- `video.src = URL.createObjectURL(source)` -- and it
  // has to be noticed here, at the write, because that is where every spelling of it converges:
  // `video.src =`, `setAttribute('src', …)` and a `srcObject`-style helper all end up on this line.
  // Attaching is also what *opens* the source and queues `sourceopen` (a task —
  // TD-0040), which is how every player learns it may start appending.
  if (media_ != nullptr && name == "src" && value.rfind("blob:", 0) == 0 &&
      media_->IsMedia(element) && media_->AttachMediaSource(element, value)) {
    ScheduleMediaSourceOpened(media_->SourceForObjectUrl(value));
  }
  // Keep binding tokens in the attribute map for getAttribute / Polymer
  // annotation parsing, but do not deliver attributeChangedCallback — that
  // path JSON.parses Array/Object types and is what hung youtube (TD-0017).
  if (!IsTemplateBindingToken(value)) {
    RunAttributeReaction(element, name, old_value, new_value);
  }
  RecordMutation(element, "attributes", name, old_value, {}, {}, attribute_namespace);
}

void DomBindings::RemoveElementAttribute(dom::Element& element, const std::string& name) {
  const std::string* previous = element.GetAttribute(name);
  if (previous == nullptr) {
    // "If attr is null, then return" -- removing an attribute that is not
    // there is not a mutation, and an observer told about it counts one record
    // where the specification says none. The NS form already returned here.
    return;
  }
  const Value old_value = Value::String(*previous);
  element.RemoveAttribute(name);
  // The reaction is told the new value is null, which is how a class
  // distinguishes "set to empty" from "gone".
  RunAttributeReaction(element, name, old_value, Value::Null());
  RecordMutation(element, "attributes", name, old_value, {}, {});
}

void DomBindings::InstallReflections() {
  for (const Reflection& entry : kReflections) {
    const Value* prototype = interfaces_.object->GetOwn(entry.interface);
    if (prototype == nullptr || !prototype->IsObject()) {
      continue;
    }
    const char* attribute = entry.attribute;
    const Reflect kind = entry.kind;

    const Value get = interpreter_->NewNativeValue(entry.property, [attribute,
                                                                    kind](NativeCall& call) {
      dom::Element* element = ElementOf(call.self);
      if (element == nullptr) {
        return Value::Undefined();
      }
      const std::string* value = element->GetAttribute(attribute);
      switch (kind) {
        case Reflect::Presence:
          return Value::Bool(value != nullptr);
        case Reflect::InputType:
          return Value::String(value == nullptr || value->empty() ? std::string("text") : *value);
        case Reflect::TextareaValue:
          return Value::String(value == nullptr ? element->TextContent() : *value);
        case Reflect::Text:
          break;
      }
      return Value::String(value == nullptr ? std::string() : *value);
    });
    const Value set = interpreter_->NewNativeValue(entry.property, [attribute,
                                                                    kind](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      dom::Element* element = ElementOf(call.self);
      if (owner == nullptr || element == nullptr) {
        return Value::Undefined();
      }
      const Value assigned = Argument(call.arguments, 0);
      if (kind == Reflect::Presence) {
        // Presence, so a false is a removal. Writing "false" into the attribute
        // would leave the element disabled, which is the opposite of what the
        // page asked for and the reason this is not one code path with Text.
        if (js::ToBoolean(assigned)) {
          owner->SetElementAttribute(*element, attribute, std::string());
        } else {
          owner->RemoveElementAttribute(*element, attribute);
        }
        return Value::Undefined();
      }
      // DOMString conversion runs toString/valueOf (Web IDL). Pure js::ToString
      // invents "[object Object]" for Location/URL — youtube then requested
      // `https://www.youtube.com/[object%20Object]` (seen as consent continue=
      // after redirect). Same class as TD-0027; href on <a> already coerced.
      std::string text;
      if (!CoerceToString(call, assigned, text)) {
        return call.ThrownValue();
      }
      owner->SetElementAttribute(*element, attribute, text);
      return Value::Undefined();
    });
    if (!get.IsObject() || !set.IsObject()) {
      continue;
    }
    get.object->Set(kOwnerSlot, PointerValue(this));
    set.object->Set(kOwnerSlot, PointerValue(this));
    prototype->object->DefineAccessor(entry.property, get.object, set.object);
  }
  InstallHyperlinkElementUtils();
  InstallFrameElement();
}

void DomBindings::InstallFrameElement() {
  // `HTMLIFrameElement.contentDocument` and `.contentWindow` -- ADR 0027 §1.
  //
  // **The origin check is not here and cannot be.** This module may not see `src/url`, so it has no
  // way to compare two origins; `src/engine` attaches the child's document to the `<iframe>`
  // element only when the two are same-origin, so a cross-origin frame simply has nothing here to
  // return. That is the check being *structural* rather than a test a future caller could forget,
  // which is what ADR 0027 §2 asks for and what ADR 0008's allow-list exists to make possible.
  const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn("HTMLIFrameElement")
                                                  : nullptr;
  if (prototype == nullptr || !prototype->IsObject() || interpreter_ == nullptr) {
    return;
  }

  const auto nested = [](NativeCall& call) -> dom::Document* {
    dom::Element* element = ElementOf(call.self);
    return element == nullptr ? nullptr : element->NestedDocument();
  };

  const Value content_document =
      interpreter_->NewNativeValue("contentDocument", [nested](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Document* document = nested(call);
        if (owner == nullptr || document == nullptr) {
          // Null rather than undefined, and for both reasons at once: it is what the DOM answers
          // for an absent node, and it is what a cross-origin frame answers. A page cannot tell the
          // two apart, which is correct -- "is there a document there" is itself information about
          // another origin.
          return Value::Null();
        }
        return owner->WrapperFor(document);
      });
  if (content_document.IsObject()) {
    content_document.object->Set(kOwnerSlot, PointerValue(this));
    prototype->object->DefineAccessor("contentDocument", content_document.object, nullptr);
  }

  // `contentWindow` is deliberately **not** the child's global object.
  //
  // It cannot be: each browsing context has its own `js::Interpreter` and therefore its own heap,
  // which is what makes ADR 0027 §5's process split an extraction rather than a rewrite -- and an
  // object from one heap handed to another is a use-after-free waiting for the first collection.
  // What a same-origin page actually uses `contentWindow` for is `.document`, and that is answered
  // here; the full same-origin window -- a page reaching a global its own frame's script set --
  // needs a realm concept in `src/js`, which is written up in the session log as the next cost of
  // this ADR.
  //
  // Absent entirely for a cross-origin frame rather than a `WindowProxy`, which is a **known
  // deviation from ADR 0027 §2** and the reason `postMessage` across frames is not here yet.
  const Value content_window =
      interpreter_->NewNativeValue("contentWindow", [nested](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Document* document = nested(call);
        if (owner == nullptr || document == nullptr) {
          return Value::Null();
        }
        const Value window = call.interpreter.NewObjectValue();
        if (!window.IsObject()) {
          return Value::Null();
        }
        window.object->Set("document", owner->WrapperFor(document));
        return window;
      });
  if (content_window.IsObject()) {
    content_window.object->Set(kOwnerSlot, PointerValue(this));
    prototype->object->DefineAccessor("contentWindow", content_window.object, nullptr);
  }
}

void DomBindings::InstallHyperlinkElementUtils() {
  // HTMLHyperlinkElementUtils on `<a>`. youtube's searchbox resolves
  // `location.href` through `document.createElement('a'); a.href = url;
  // a.pathname` (`n0n`). Without `pathname` that call threw and Enter never
  // navigated (TD-0026).
  const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn("HTMLAnchorElement")
                                                  : nullptr;
  if (prototype == nullptr || !prototype->IsObject()) {
    return;
  }

  // The attribute as written, and the URL it means. They are different strings and the difference is
  // the whole of HTMLHyperlinkElementUtils: `href` and every part below it answer about the *parsed*
  // URL -- resolved against the document's base and with its query encoded in the document's
  // character set (ADR 0025 §2) -- so `<a href="?q=日本">` on a Shift_JIS page reports
  // `%93%FA%96%7B`, which is what a click on it would send.
  //
  // A URL that does not parse falls back to the attribute, which is the specification's answer and
  // not a shortcut: `href` on such an element is defined to be the attribute's value, and reporting
  // the empty string instead would make `a.href` unusable as the "what did the author write" it is
  // also used for.
  const auto href_attribute = [](dom::Element* element) -> std::string {
    if (element == nullptr) {
      return {};
    }
    const std::string* value = element->GetAttribute("href");
    return value == nullptr ? std::string() : *value;
  };
  const auto href_string = [href_attribute](DomBindings* owner,
                                            dom::Element* element) -> std::string {
    const std::string written = href_attribute(element);
    if (owner == nullptr || owner->network_ == nullptr || written.empty()) {
      return written;
    }
    std::string resolved = owner->network_->ResolveDocumentUrl(written);
    return resolved.empty() ? written : resolved;
  };

  const Value href_get = interpreter_->NewNativeValue("href", [href_string](NativeCall& call) {
    return Value::String(href_string(OwnerOf(call), ElementOf(call.self)));
  });
  const Value href_set = interpreter_->NewNativeValue("href", [](NativeCall& call) {
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

  const auto install_part = [this, prototype, href_string](const char* name, auto pick) {
    const Value get = interpreter_->NewNativeValue(name, [href_string, pick](NativeCall& call) {
      const HrefParts parts = SplitHref(href_string(OwnerOf(call), ElementOf(call.self)));
      return Value::String(pick(parts));
    });
    if (get.IsObject()) {
      get.object->Set(kOwnerSlot, PointerValue(this));
      prototype->object->DefineAccessor(name, get.object, nullptr);
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
}

}  // namespace microbrowser::bindings
