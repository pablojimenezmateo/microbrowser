#pragma once

#include <string_view>

// **Which tag gets which interface**, and nothing else.
//
// Split out of NodeInterfaces.cpp when that file passed the module's line cap,
// which is the cap working as intended: this is a table, and the file it came
// from is the *chain* -- prototypes, constructors, and the machinery that hangs
// one off another. Three worktrees added to that file in one week and not one
// of them touched this list, which is the other half of the argument.
//
// Private to the module, like BindingSupport.h and LiveRanges.h, and for the
// same reason: a binding is an implementation detail of the seam.

namespace microbrowser::bindings {

// Which tag gets which interface.
//
// Every tag HTML gives an interface of its own, and the interface's parent
// when that is not HTMLElement.
//
// It used to be a short list of "tags a page is likely to name", and the
// argument for that was wrong in a way the suite made obvious:
// `Node-cloneNode.html` asks `assert_true("HTMLAreaElement" in window)` before
// it clones an `<area>`, and 48 of its subtests failed on that line alone --
// not on cloning. A page tests for a type before it has one, and the cost of a
// missing name is a ReferenceError rather than a false `instanceof`.
//
// A tag *not* here is an HTMLElement, which is right for `<abbr>` and wrong for
// `<foo>` -- an unknown tag is an HTMLUnknownElement. That distinction needs a
// list of the tags HTML defines at all, which is C10's, not this table's.
struct TagInterface {
  std::string_view tag;
  const char* interface;
  // Null means HTMLElement. The two that are not are `<svg>`, which is an
  // Element with no HTML semantics at all, and the media pair.
  const char* parent = nullptr;
};

constexpr TagInterface kTagInterfaces[] = {
    {"a", "HTMLAnchorElement"},
    {"area", "HTMLAreaElement"},
    {"audio", "HTMLAudioElement", "HTMLMediaElement"},
    {"base", "HTMLBaseElement"},
    {"blockquote", "HTMLQuoteElement"},
    {"body", "HTMLBodyElement"},
    {"br", "HTMLBRElement"},
    {"button", "HTMLButtonElement"},
    {"canvas", "HTMLCanvasElement"},
    {"caption", "HTMLTableCaptionElement"},
    {"col", "HTMLTableColElement"},
    {"colgroup", "HTMLTableColElement"},
    {"data", "HTMLDataElement"},
    {"datalist", "HTMLDataListElement"},
    {"del", "HTMLModElement"},
    {"details", "HTMLDetailsElement"},
    {"dialog", "HTMLDialogElement"},
    {"dir", "HTMLDirectoryElement"},
    {"div", "HTMLDivElement"},
    {"dl", "HTMLDListElement"},
    {"embed", "HTMLEmbedElement"},
    {"fieldset", "HTMLFieldSetElement"},
    {"font", "HTMLFontElement"},
    {"form", "HTMLFormElement"},
    {"frame", "HTMLFrameElement"},
    {"frameset", "HTMLFrameSetElement"},
    {"h1", "HTMLHeadingElement"},
    {"h2", "HTMLHeadingElement"},
    {"h3", "HTMLHeadingElement"},
    {"h4", "HTMLHeadingElement"},
    {"h5", "HTMLHeadingElement"},
    {"h6", "HTMLHeadingElement"},
    {"head", "HTMLHeadElement"},
    {"hr", "HTMLHRElement"},
    {"html", "HTMLHtmlElement"},
    {"iframe", "HTMLIFrameElement"},
    {"img", "HTMLImageElement"},
    {"input", "HTMLInputElement"},
    {"ins", "HTMLModElement"},
    {"label", "HTMLLabelElement"},
    {"legend", "HTMLLegendElement"},
    {"li", "HTMLLIElement"},
    {"link", "HTMLLinkElement"},
    {"listing", "HTMLPreElement"},
    {"map", "HTMLMapElement"},
    {"marquee", "HTMLMarqueeElement"},
    {"menu", "HTMLMenuElement"},
    {"meta", "HTMLMetaElement"},
    {"meter", "HTMLMeterElement"},
    {"object", "HTMLObjectElement"},
    {"ol", "HTMLOListElement"},
    {"optgroup", "HTMLOptGroupElement"},
    {"option", "HTMLOptionElement"},
    {"output", "HTMLOutputElement"},
    {"p", "HTMLParagraphElement"},
    {"param", "HTMLParamElement"},
    {"picture", "HTMLPictureElement"},
    {"pre", "HTMLPreElement"},
    {"progress", "HTMLProgressElement"},
    {"q", "HTMLQuoteElement"},
    {"script", "HTMLScriptElement"},
    {"select", "HTMLSelectElement"},
    {"slot", "HTMLSlotElement"},
    {"source", "HTMLSourceElement"},
    {"span", "HTMLSpanElement"},
    {"style", "HTMLStyleElement"},
    {"table", "HTMLTableElement"},
    {"tbody", "HTMLTableSectionElement"},
    {"td", "HTMLTableCellElement"},
    {"template", "HTMLTemplateElement"},
    {"textarea", "HTMLTextAreaElement"},
    {"tfoot", "HTMLTableSectionElement"},
    {"th", "HTMLTableCellElement"},
    {"thead", "HTMLTableSectionElement"},
    {"time", "HTMLTimeElement"},
    {"title", "HTMLTitleElement"},
    {"tr", "HTMLTableRowElement"},
    {"track", "HTMLTrackElement"},
    {"ul", "HTMLUListElement"},
    {"video", "HTMLVideoElement", "HTMLMediaElement"},
    {"xmp", "HTMLPreElement"},
    // `<svg>` is an Element and not an HTMLElement. Only the root tag is
    // listed: the elements *inside* an SVG subtree are not distinguished,
    // because `src/html` has no foreign content (TreeBuilder.h says so) and so
    // this DOM has no namespace to ask about. When foreign content lands, this
    // is where its tags go.
    {"svg", "SVGElement", "Element"},
};

// The HTML elements whose interface is plain `HTMLElement`.
//
// This list is what makes `<foo>` an **HTMLUnknownElement** rather than an HTMLElement, which is
// the distinction the header comment above said was owed. It is not cosmetic: HTML defines
// `HTMLUnknownElement` precisely so that a page can tell an element the language knows from one it
// invented, and a browser that answered `HTMLElement` for both would say `<blink>` is a real
// element. `html/semantics/interfaces.html` asserts it for all 138 tags it knows.
//
// From the HTML Standard's element index. The deprecated ones that are *still* elements are here
// (`center`, `big`, `strike`, `tt`, `acronym`, `nobr`, `plaintext`) and the ones that were removed
// are deliberately not (`applet`, `bgsound`, `blink`, `command`, `directory`, `isindex`, `keygen`,
// `nextid`, `spacer`) -- those are unknown, which is what every browser reports.
constexpr std::string_view kPlainHtmlElements[] = {
    "abbr", "acronym", "address", "article", "aside", "b", "basefont", "bdi", "bdo", "big",
    "center", "cite", "code", "dd", "dfn", "dt", "em", "figcaption", "figure", "footer",
    "header", "hgroup", "i", "kbd", "main", "mark", "nav", "nobr", "noembed", "noframes",
    "noscript", "plaintext", "rb", "rp", "rt", "rtc", "ruby", "s", "samp", "search", "section",
    "small", "strike", "strong", "sub", "summary", "sup", "tt", "u", "var", "wbr"
};

// A valid custom element name, which HTML gives `HTMLElement` rather than `HTMLUnknownElement`:
// starts with a lowercase ASCII letter, contains a hyphen, and holds no uppercase. `foo-bar` is
// one and `foo-BAR` is not -- the suite tests exactly that pair, because a name a page *cannot*
// register must not answer as though it could.
inline bool IsValidCustomElementName(std::string_view tag) {
  if (tag.empty() || tag.front() < 'a' || tag.front() > 'z') {
    return false;
  }
  bool hyphen = false;
  for (const char c : tag) {
    if (c == '-') {
      hyphen = true;
    }
    if (c >= 'A' && c <= 'Z') {
      return false;
    }
  }
  return hyphen;
}

// The interface for `tag`, or `"HTMLUnknownElement"` when HTML defines no element by that name.
// Never the empty string: the caller used to read one as "HTMLElement", which is the answer that
// hid this whole distinction.
const char* InterfaceForTag(std::string_view tag) {
  for (const TagInterface& entry : kTagInterfaces) {
    if (entry.tag == tag) {
      return entry.interface;
    }
  }
  for (const std::string_view known : kPlainHtmlElements) {
    if (known == tag) {
      return "HTMLElement";
    }
  }
  return IsValidCustomElementName(tag) ? "HTMLElement" : "HTMLUnknownElement";
}

}  // namespace microbrowser::bindings
