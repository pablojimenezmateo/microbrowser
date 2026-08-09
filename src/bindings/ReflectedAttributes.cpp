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
  // **A media element's `src` set to an object URL is an *attach*, not a fetch.** This is the one path
  // by which a `MediaSource` reaches an element -- `video.src = URL.createObjectURL(source)` -- and it
  // has to be noticed here, at the write, because that is where every spelling of it converges:
  // `video.src =`, `setAttribute('src', …)` and a `srcObject`-style helper all end up on this line.
  // Attaching is also what *opens* the source and fires `sourceopen`, which is how every player learns
  // it may start appending.
  if (media_ != nullptr && name == "src" && value.rfind("blob:", 0) == 0 &&
      media_->IsMedia(element) && media_->AttachMediaSource(element, value)) {
    DeliverMediaSourceOpenedFor(value);
  }
  // Keep binding tokens in the attribute map for getAttribute / Polymer
  // annotation parsing, but do not deliver attributeChangedCallback — that
  // path JSON.parses Array/Object types and is what hung youtube (TD-0017).
  if (!IsTemplateBindingToken(value)) {
    RunAttributeReaction(element, name, old_value, Value::String(value));
  }
  RecordMutation(element, "attributes", name, old_value, {}, {});
}

void DomBindings::RemoveElementAttribute(dom::Element& element, const std::string& name) {
  const std::string* previous = element.GetAttribute(name);
  const Value old_value = previous == nullptr ? Value::Null() : Value::String(*previous);
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
      owner->SetElementAttribute(*element, attribute, js::ToString(assigned));
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

  const auto href_string = [](dom::Element* element) -> std::string {
    if (element == nullptr) {
      return {};
    }
    const std::string* value = element->GetAttribute("href");
    return value == nullptr ? std::string() : *value;
  };

  const Value href_get = interpreter_->NewNativeValue("href", [href_string](NativeCall& call) {
    return Value::String(href_string(ElementOf(call.self)));
  });
  const Value href_set = interpreter_->NewNativeValue("href", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Element* element = ElementOf(call.self);
    if (owner == nullptr || element == nullptr) {
      return Value::Undefined();
    }
    owner->SetElementAttribute(*element, "href", js::ToString(Argument(call.arguments, 0)));
    return Value::Undefined();
  });
  if (href_get.IsObject() && href_set.IsObject()) {
    href_get.object->Set(kOwnerSlot, PointerValue(this));
    href_set.object->Set(kOwnerSlot, PointerValue(this));
    prototype->object->DefineAccessor("href", href_get.object, href_set.object);
  }

  const auto install_part = [this, prototype, href_string](const char* name, auto pick) {
    const Value get = interpreter_->NewNativeValue(name, [href_string, pick](NativeCall& call) {
      const HrefParts parts = SplitHref(href_string(ElementOf(call.self)));
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
