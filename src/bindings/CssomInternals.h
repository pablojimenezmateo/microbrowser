#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/Geometry.h"
#include "css/Cssom.h"
#include "css/StyleResolver.h"
#include "dom/Node.h"
#include "js/Interpreter.h"

namespace microbrowser::bindings {

using CssomSheetStorage = std::shared_ptr<std::string>;

inline CssomSheetStorage* CssomSheetStoragePtr(const js::Value& sheet) {
  if (!sheet.IsObject()) {
    return nullptr;
  }
  const js::Value* marker = sheet.object->GetOwn(kCSSStyleSheetMarkerSlot);
  if (marker == nullptr || !js::ToBoolean(*marker)) {
    return nullptr;
  }
  const js::Value* slot = sheet.object->GetOwn(kCSSSheetStorageSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<CssomSheetStorage*>(static_cast<std::uintptr_t>(slot->number));
}

inline dom::Element* CssomSheetOwnerOf(const js::Value& sheet) {
  if (!sheet.IsObject()) {
    return nullptr;
  }
  const js::Value* slot = sheet.object->GetOwn(kSheetOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Element*>(static_cast<std::uintptr_t>(slot->number));
}

inline std::string CssomSheetText(const js::Value& sheet) {
  if (CssomSheetStorage* storage = CssomSheetStoragePtr(sheet);
      storage != nullptr && *storage != nullptr) {
    return **storage;
  }
  const dom::Element* owner = CssomSheetOwnerOf(sheet);
  if (owner == nullptr) {
    return {};
  }
  if (owner->TagName() == "style") {
    std::string text;
    for (const std::unique_ptr<dom::Node>& child : owner->Children()) {
      if (child->IsText()) {
        text += static_cast<const dom::Text&>(*child).Data();
      }
    }
    return text;
  }
  if (const std::string* text = owner->LinkedStyleSheetText()) {
    return *text;
  }
  return {};
}

inline js::Object* CssomHostObject(const js::Value& object) {
  if (!object.IsObject()) {
    return nullptr;
  }
  js::Object* raw = BehindProxies(object.object);
  if (raw == nullptr) {
    return nullptr;
  }
  if (const js::Value* parent = raw->GetOwn(kCssomCssTextSlot);
      parent != nullptr && parent->IsObject()) {
    return BehindProxies(parent->object);
  }
  return raw;
}

inline const js::Value* CssomSheetSlotOf(const js::Value& object) {
  js::Object* host = CssomHostObject(object);
  return host == nullptr ? nullptr : host->GetOwn(kCssomSheetSlot);
}

inline css::CssomRule* LocateCssomRule(std::vector<css::CssomRule>& roots, const js::Value& object) {
  std::vector<std::size_t> path;
  js::Object* at = CssomHostObject(object);
  for (int depth = 0; at != nullptr && depth < 16; ++depth) {
    const js::Value* index = at->GetOwn(kCssomIndexSlot);
    if (index == nullptr || !index->IsNumber() || index->number < 0.0) {
      return nullptr;
    }
    path.push_back(static_cast<std::size_t>(index->number));
    const js::Value* parent = at->GetOwn(kCssomParentSlot);
    if (parent == nullptr || !parent->IsObject()) {
      break;
    }
    at = BehindProxies(parent->object);
  }
  if (path.empty()) {
    return nullptr;
  }
  std::vector<css::CssomRule>* list = &roots;
  css::CssomRule* rule = nullptr;
  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    if (*it >= list->size()) {
      return nullptr;
    }
    rule = &(*list)[*it];
    list = &rule->children;
  }
  return rule;
}

inline const css::CssomRule* LocateCssomRule(const std::vector<css::CssomRule>& roots,
                                             const js::Value& object) {
  return LocateCssomRule(const_cast<std::vector<css::CssomRule>&>(roots), object);
}

void InstallCssomStylePrototype(js::Interpreter& interpreter, const js::Value& interfaces,
                                const js::Value& owner,
                                std::function<void(const js::Value&, std::string)> write_sheet,
                                GeometrySource* geometry);

void InstallCssomMediaList(js::Interpreter& interpreter, const js::Value& interfaces,
                           const js::Value& owner,
                           std::function<void(const js::Value&, std::string)> write_sheet);

inline bool IsCssomRuleThis(const js::Value& self) {
  js::Object* host = CssomHostObject(self);
  return host != nullptr && host->GetOwn(kCssomIndexSlot) != nullptr;
}

inline bool IsCssomSheetThis(const js::Value& self) {
  return CssomSheetStoragePtr(self) != nullptr || CssomSheetOwnerOf(self) != nullptr;
}

// CSSOM stores a declaration unless the name is not a CSS property at all.
// `CanonicaliseDeclaration` returns `Unknown` both for `display` (no serializer
// here) and for `unknown` (not a property). The cascade's SupportsDeclaration
// is too strict the other way: `left` is a real property this engine has not
// implemented, and specified style still has to keep it.
inline bool CssomKeepsUnknownDeclaration(std::string_view property, std::string_view value) {
  if (value.empty()) {
    return true;
  }
  if (property.find('-') != std::string_view::npos) {
    return true;
  }
  if (css::SupportsDeclaration(property, value)) {
    return true;
  }
  static constexpr std::string_view kBare[] = {
      "all",     "azimuth",  "background", "border",  "bottom",    "clear",     "clip",
      "color",   "columns",  "contain",    "content", "cue",       "cursor",    "direction",
      "display", "elevation","filter",     "flex",    "float",     "font",      "gap",
      "height",  "hyphens",  "inset",      "left",    "margin",    "opacity",   "order",
      "orphans", "overflow", "padding",    "pause",   "pitch",     "position",  "quotes",
      "resize",  "richness", "right",      "size",    "speak",     "stress",    "top",
      "visibility", "volume", "widows",    "width",   "zoom",
  };
  for (const std::string_view name : kBare) {
    if (property == name) {
      return true;
    }
  }
  return false;
}

inline bool IsCssomStyleThis(const js::Value& self) {
  js::Object* raw = self.IsObject() ? BehindProxies(self.object) : nullptr;
  if (raw == nullptr) {
    return false;
  }
  return raw->GetOwn(kComputedStyleSlot) != nullptr || raw->GetOwn(kNodeSlot) != nullptr ||
         raw->GetOwn(kCssomCssTextSlot) != nullptr;
}

}  // namespace microbrowser::bindings
