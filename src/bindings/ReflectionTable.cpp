// Which IDL attribute reflects which content attribute, and how.
//
// This is data, deliberately: the algorithms live once in
// ReflectedAttributes.cpp and this file says only *which* of them applies where.
// Written as accessors instead, HTML's reflection section becomes several
// hundred hand-copied getter/setter pairs, and the failure mode is not that one
// is missing -- it is that `td.colSpan` clamps and `col.span` does not, from the
// same paragraph of the same specification, with nothing to compare them
// against.
//
// The table is the whole of HTML's reflected surface for the elements this
// browser builds, rather than "the attributes a page writes through the
// property", which is what it used to be. That earlier line was a reasonable
// guess and the measurement disagreed with it: web-platform-tests'
// `html/dom/reflection-*.html` is 35,560 subtests over exactly this surface, and
// a page that reads `el.colSpan` and gets `undefined` does not fail loudly --
// it lays out one column wrong.
//
// Ordered by interface, and within an interface by the specification's own
// order: conforming attributes first, then the obsolete ones HTML still
// requires a browser to reflect.

#include "bindings/Reflection.h"

namespace microbrowser::bindings {
namespace {

// The keyword sets. Each is the specification's list in its canonical
// spelling; matching is ASCII case-insensitive and canonicalisation happens on
// the read side, so this is the only place a keyword is written down.
constexpr std::string_view kDirKeywords[] = {"ltr", "rtl", "auto"};
constexpr std::string_view kEnterKeyHintKeywords[] = {"enter", "done",   "go",  "next",
                                                      "previous", "search", "send"};
constexpr std::string_view kInputModeKeywords[] = {"none",  "text",    "tel",     "url",
                                                   "email", "numeric", "decimal", "search"};
// The empty string is a keyword here, and it is load-bearing: an empty
// `referrerpolicy` is the "no policy" state rather than an invalid value, so it
// has to round-trip rather than fall back.
constexpr std::string_view kReferrerPolicyKeywords[] = {"",
                                                        "no-referrer",
                                                        "no-referrer-when-downgrade",
                                                        "same-origin",
                                                        "origin",
                                                        "strict-origin",
                                                        "origin-when-cross-origin",
                                                        "strict-origin-when-cross-origin",
                                                        "unsafe-url"};
constexpr std::string_view kCrossOriginKeywords[] = {"anonymous", "use-credentials"};

// --- ARIA's reflected enumerations -------------------------------------------------------------
//
// Twenty-one of the `aria-*` attributes reflect as **limited to known values**, not as plain
// nullable strings, and the three defaults differ per attribute in ways no rule would predict:
// `aria-current=""` is `"false"` while `aria-current="nonsense"` is `"true"`, and `aria-haspopup`
// absent is `null` while `aria-haspopup="nonsense"` is `"false"`. Transcribed from ARIA's own
// reflection table, which `html/dom/elements-aria-enumerated.js` is the machine-readable copy of.
//
// The rest of the `aria-*` attributes stay `TextNullable`, which is what they are: three states,
// because `aria-checked` absent means "not a checkbox" and `aria-checked=""` means "a checkbox in
// no state".
constexpr std::string_view kTrueFalseKeywords[] = {"true", "false"};
constexpr std::string_view kTristateKeywords[] = {"true", "false", "mixed"};
constexpr std::string_view kAutoCompleteKeywords[] = {"inline", "list", "both", "none"};
constexpr std::string_view kAriaCurrentKeywords[] = {"page", "step", "location",
                                                    "date", "time", "true",     "false"};
constexpr std::string_view kHasPopupKeywords[] = {"true",   "false", "menu", "dialog",
                                                 "listbox", "tree",  "grid"};
constexpr std::string_view kAriaInvalidKeywords[] = {"true", "false", "spelling", "grammar"};
constexpr std::string_view kAriaLiveKeywords[] = {"polite", "assertive", "off"};
constexpr std::string_view kOrientationKeywords[] = {"horizontal", "vertical"};
constexpr std::string_view kAriaSortKeywords[] = {"ascending", "descending", "other", "none"};
constexpr std::string_view kEncTypeKeywords[] = {"application/x-www-form-urlencoded",
                                                 "multipart/form-data", "text/plain"};
constexpr std::string_view kFormMethodKeywords[] = {"get", "post", "dialog"};
constexpr std::string_view kAutocompleteKeywords[] = {"on", "off"};
constexpr std::string_view kButtonTypeKeywords[] = {"submit", "reset", "button"};
// All twenty-two input types, including `month` and `week` -- which is not a
// detail: the suite splits them into their own file
// (`reflection-forms-weekmonth.html`) precisely so that a browser missing them
// is caught rather than excused.
constexpr std::string_view kInputTypeKeywords[] = {
    "hidden", "text",     "search",         "tel",    "url",      "email",
    "password", "date",   "month",          "week",   "time",     "datetime-local",
    "number", "range",    "color",          "checkbox", "radio",   "file",
    "submit", "image",    "reset",          "button"};
constexpr std::string_view kDecodingKeywords[] = {"async", "sync", "auto"};
constexpr std::string_view kLoadingKeywords[] = {"lazy", "eager"};
constexpr std::string_view kPreloadKeywords[] = {"none", "metadata", "auto"};
constexpr std::string_view kTrackKindKeywords[] = {"subtitles", "captions", "descriptions",
                                                   "chapters", "metadata"};
constexpr std::string_view kScopeKeywords[] = {"row", "col", "rowgroup", "colgroup"};
constexpr std::string_view kLinkAsKeywords[] = {
    "fetch",  "audio",  "document", "embed",  "font",  "image", "manifest", "object",
    "report", "script", "sharedworker", "style", "track", "video", "worker", "xslt"};
constexpr std::string_view kMarqueeBehaviorKeywords[] = {"scroll", "slide", "alternate"};
constexpr std::string_view kMarqueeDirectionKeywords[] = {"up", "right", "down", "left"};

constexpr Reflection kReflections[] = {
    // -- Element ------------------------------------------------------------
    {"Element", "id", "id", Reflect::Text},
    {"Element", "className", "class", Reflect::Text},
    {"Element", "slot", "slot", Reflect::Text},


    // -- ARIA, on Element ----------------------------------------------------
    // Every `aria-*` attribute reflects as a nullable string, which is three
    // states rather than two and all three are load-bearing: `aria-checked`
    // absent means "not a checkbox", `aria-checked=""` means "a checkbox in no
    // state", and a getter that folded the first into "" would tell an assistive
    // technology the second.
    //
    // On `Element` rather than `HTMLElement`, which is where ARIA puts them --
    // an `<svg>` element carries a role too.
    //
    // Deliberately *not* the enumerated form that
    // `aria-attribute-reflection-enumerated.tentative.html` tests. That file is
    // w3c/aria PR 2484, a proposal, and it contradicts the stable test rather
    // than extending it: with `aria-busy` absent the proposal wants "false" and
    // `aria-attribute-reflection.html` wants null. Implementing the proposal
    // means failing the shipped specification, which is ADR 0012's rule about
    // stubs pointed at a percentage.

    {"Element", "role", "role", Reflect::TextNullable},
    {"Element", "ariaAtomic", "aria-atomic", Reflect::Enumerated, kTrueFalseKeywords, nullptr, "false", true},
    {"Element", "ariaAutoComplete", "aria-autocomplete", Reflect::Enumerated, kAutoCompleteKeywords, "none", "none", true},
    {"Element", "ariaBrailleLabel", "aria-braillelabel", Reflect::TextNullable},
    {"Element", "ariaBrailleRoleDescription", "aria-brailleroledescription", Reflect::TextNullable},
    {"Element", "ariaBusy", "aria-busy", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaChecked", "aria-checked", Reflect::Enumerated, kTristateKeywords, nullptr, nullptr, true},
    {"Element", "ariaColCount", "aria-colcount", Reflect::TextNullable},
    {"Element", "ariaColIndex", "aria-colindex", Reflect::TextNullable},
    {"Element", "ariaColIndexText", "aria-colindextext", Reflect::TextNullable},
    {"Element", "ariaColSpan", "aria-colspan", Reflect::TextNullable},
    {"Element", "ariaCurrent", "aria-current", Reflect::Enumerated, kAriaCurrentKeywords, "false", "true", true, 0, 0, 0, "false"},
    {"Element", "ariaDescription", "aria-description", Reflect::TextNullable},
    {"Element", "ariaDisabled", "aria-disabled", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaExpanded", "aria-expanded", Reflect::Enumerated, kTrueFalseKeywords, nullptr, nullptr, true},
    {"Element", "ariaHasPopup", "aria-haspopup", Reflect::Enumerated, kHasPopupKeywords, nullptr, "false", true},
    {"Element", "ariaHidden", "aria-hidden", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaInvalid", "aria-invalid", Reflect::Enumerated, kAriaInvalidKeywords, "false", "true", true, 0, 0, 0, "false"},
    {"Element", "ariaKeyShortcuts", "aria-keyshortcuts", Reflect::TextNullable},
    {"Element", "ariaLabel", "aria-label", Reflect::TextNullable},
    {"Element", "ariaLevel", "aria-level", Reflect::TextNullable},
    {"Element", "ariaLive", "aria-live", Reflect::Enumerated, kAriaLiveKeywords, "off", "off", true},
    {"Element", "ariaModal", "aria-modal", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaMultiLine", "aria-multiline", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaMultiSelectable", "aria-multiselectable", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaOrientation", "aria-orientation", Reflect::Enumerated, kOrientationKeywords, nullptr, nullptr, true},
    {"Element", "ariaPlaceholder", "aria-placeholder", Reflect::TextNullable},
    {"Element", "ariaPosInSet", "aria-posinset", Reflect::TextNullable},
    {"Element", "ariaPressed", "aria-pressed", Reflect::Enumerated, kTristateKeywords, nullptr, nullptr, true},
    {"Element", "ariaReadOnly", "aria-readonly", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaRelevant", "aria-relevant", Reflect::TextNullable},
    {"Element", "ariaRequired", "aria-required", Reflect::Enumerated, kTrueFalseKeywords, "false", "false", true},
    {"Element", "ariaRoleDescription", "aria-roledescription", Reflect::TextNullable},
    {"Element", "ariaRowCount", "aria-rowcount", Reflect::TextNullable},
    {"Element", "ariaRowIndex", "aria-rowindex", Reflect::TextNullable},
    {"Element", "ariaRowIndexText", "aria-rowindextext", Reflect::TextNullable},
    {"Element", "ariaRowSpan", "aria-rowspan", Reflect::TextNullable},
    {"Element", "ariaSelected", "aria-selected", Reflect::Enumerated, kTrueFalseKeywords, nullptr, nullptr, true},
    {"Element", "ariaSetSize", "aria-setsize", Reflect::TextNullable},
    {"Element", "ariaSort", "aria-sort", Reflect::Enumerated, kAriaSortKeywords, "none", "none", true},
    {"Element", "ariaValueMax", "aria-valuemax", Reflect::TextNullable},
    {"Element", "ariaValueMin", "aria-valuemin", Reflect::TextNullable},
    {"Element", "ariaValueNow", "aria-valuenow", Reflect::TextNullable},
    {"Element", "ariaValueText", "aria-valuetext", Reflect::TextNullable},

    // -- HTMLElement --------------------------------------------------------
    // The global attributes, which every element in the suite is tested for.
    {"HTMLElement", "title", "title", Reflect::Text},
    {"HTMLElement", "lang", "lang", Reflect::Text},
    {"HTMLElement", "dir", "dir", Reflect::Enumerated, kDirKeywords},
    {"HTMLElement", "accessKey", "accesskey", Reflect::Text},
    {"HTMLElement", "autofocus", "autofocus", Reflect::Boolean},
    // Presence, and the reason youtube's expandable metadata used to stay open:
    // Polymer writes `el.hidden = !isExpanded` from `hidden="[[!isExpanded]]"`.
    // Without a setter that reaches the content attribute the assignment was an
    // expando, the cascade never saw `[hidden]`, and search rows grew to ~900px.
    {"HTMLElement", "hidden", "hidden", Reflect::Boolean},
    {"HTMLElement", "tabIndex", "tabindex", Reflect::Long},
    {"HTMLElement", "enterKeyHint", "enterkeyhint", Reflect::Enumerated, kEnterKeyHintKeywords},
    {"HTMLElement", "inputMode", "inputmode", Reflect::Enumerated, kInputModeKeywords},

    // -- HTMLAnchorElement --------------------------------------------------
    {"HTMLAnchorElement", "href", "href", Reflect::Url},
    {"HTMLAnchorElement", "target", "target", Reflect::Text},
    {"HTMLAnchorElement", "download", "download", Reflect::Text},
    {"HTMLAnchorElement", "ping", "ping", Reflect::Text},
    {"HTMLAnchorElement", "rel", "rel", Reflect::Text},
    {"HTMLAnchorElement", "hreflang", "hreflang", Reflect::Text},
    {"HTMLAnchorElement", "type", "type", Reflect::Text},
    {"HTMLAnchorElement", "referrerPolicy", "referrerpolicy", Reflect::Enumerated,
     kReferrerPolicyKeywords},
    {"HTMLAnchorElement", "coords", "coords", Reflect::Text},
    {"HTMLAnchorElement", "charset", "charset", Reflect::Text},
    {"HTMLAnchorElement", "name", "name", Reflect::Text},
    {"HTMLAnchorElement", "rev", "rev", Reflect::Text},
    {"HTMLAnchorElement", "shape", "shape", Reflect::Text},

    // -- HTMLAreaElement ----------------------------------------------------
    {"HTMLAreaElement", "href", "href", Reflect::Url},
    {"HTMLAreaElement", "alt", "alt", Reflect::Text},
    {"HTMLAreaElement", "coords", "coords", Reflect::Text},
    {"HTMLAreaElement", "shape", "shape", Reflect::Text},
    {"HTMLAreaElement", "target", "target", Reflect::Text},
    {"HTMLAreaElement", "download", "download", Reflect::Text},
    {"HTMLAreaElement", "ping", "ping", Reflect::Text},
    {"HTMLAreaElement", "rel", "rel", Reflect::Text},
    {"HTMLAreaElement", "referrerPolicy", "referrerpolicy", Reflect::Enumerated,
     kReferrerPolicyKeywords},
    {"HTMLAreaElement", "hreflang", "hreflang", Reflect::Text},
    {"HTMLAreaElement", "type", "type", Reflect::Text},
    {"HTMLAreaElement", "noHref", "nohref", Reflect::Boolean},

    // -- HTMLBaseElement ----------------------------------------------------
    {"HTMLBaseElement", "href", "href", Reflect::Url},
    {"HTMLBaseElement", "target", "target", Reflect::Text},

    // -- HTMLBodyElement ----------------------------------------------------
    // The presentational colours are [LegacyNullToEmptyString]: `body.bgColor =
    // null` clears the attribute rather than asking for a colour named "null".
    {"HTMLBodyElement", "text", "text", Reflect::TextNullToEmpty},
    {"HTMLBodyElement", "link", "link", Reflect::TextNullToEmpty},
    {"HTMLBodyElement", "vLink", "vlink", Reflect::TextNullToEmpty},
    {"HTMLBodyElement", "aLink", "alink", Reflect::TextNullToEmpty},
    {"HTMLBodyElement", "bgColor", "bgcolor", Reflect::TextNullToEmpty},
    {"HTMLBodyElement", "background", "background", Reflect::Text},

    // -- HTMLBRElement ------------------------------------------------------
    {"HTMLBRElement", "clear", "clear", Reflect::Text},

    // -- HTMLButtonElement --------------------------------------------------
    {"HTMLButtonElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLButtonElement", "formAction", "formaction", Reflect::Url_OrDocumentAddress},
    {"HTMLButtonElement", "formEnctype", "formenctype", Reflect::Enumerated, kEncTypeKeywords, "",
     "application/x-www-form-urlencoded"},
    {"HTMLButtonElement", "formMethod", "formmethod", Reflect::Enumerated, kFormMethodKeywords, "",
     "get"},
    {"HTMLButtonElement", "formNoValidate", "formnovalidate", Reflect::Boolean},
    {"HTMLButtonElement", "formTarget", "formtarget", Reflect::Text},
    {"HTMLButtonElement", "name", "name", Reflect::Text},
    {"HTMLButtonElement", "type", "type", Reflect::Enumerated, kButtonTypeKeywords, "submit",
     "submit"},
    {"HTMLButtonElement", "value", "value", Reflect::Text},

    // -- HTMLCanvasElement --------------------------------------------------
    {"HTMLCanvasElement", "width", "width", Reflect::UnsignedLong, {}, "", nullptr, false, 300},
    {"HTMLCanvasElement", "height", "height", Reflect::UnsignedLong, {}, "", nullptr, false, 150},

    // -- HTMLDataElement ----------------------------------------------------
    {"HTMLDataElement", "value", "value", Reflect::Text},

    // -- HTMLDetailsElement / HTMLDialogElement -----------------------------
    {"HTMLDetailsElement", "open", "open", Reflect::Boolean},
    {"HTMLDialogElement", "open", "open", Reflect::Boolean},

    // -- The four list interfaces, which differ only in which ones HTML gave a
    //    `compact` to. -------------------------------------------------------
    {"HTMLDirectoryElement", "compact", "compact", Reflect::Boolean},
    {"HTMLDListElement", "compact", "compact", Reflect::Boolean},
    {"HTMLMenuElement", "compact", "compact", Reflect::Boolean},
    {"HTMLUListElement", "compact", "compact", Reflect::Boolean},
    {"HTMLUListElement", "type", "type", Reflect::Text},
    {"HTMLOListElement", "reversed", "reversed", Reflect::Boolean},
    {"HTMLOListElement", "start", "start", Reflect::Long, {}, "", nullptr, false, 1},
    {"HTMLOListElement", "type", "type", Reflect::Text},
    {"HTMLOListElement", "compact", "compact", Reflect::Boolean},
    {"HTMLLIElement", "value", "value", Reflect::Long},
    {"HTMLLIElement", "type", "type", Reflect::Text},

    // -- HTMLDivElement / the block-level `align` attributes ----------------
    {"HTMLDivElement", "align", "align", Reflect::Text},
    {"HTMLParagraphElement", "align", "align", Reflect::Text},
    {"HTMLHeadingElement", "align", "align", Reflect::Text},
    {"HTMLLegendElement", "align", "align", Reflect::Text},

    // -- HTMLEmbedElement ---------------------------------------------------
    {"HTMLEmbedElement", "src", "src", Reflect::Url},
    {"HTMLEmbedElement", "type", "type", Reflect::Text},
    {"HTMLEmbedElement", "width", "width", Reflect::Text},
    {"HTMLEmbedElement", "height", "height", Reflect::Text},
    {"HTMLEmbedElement", "align", "align", Reflect::Text},
    {"HTMLEmbedElement", "name", "name", Reflect::Text},

    // -- HTMLFieldSetElement ------------------------------------------------
    {"HTMLFieldSetElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLFieldSetElement", "name", "name", Reflect::Text},

    // -- HTMLFontElement ----------------------------------------------------
    {"HTMLFontElement", "color", "color", Reflect::TextNullToEmpty},
    {"HTMLFontElement", "face", "face", Reflect::Text},
    {"HTMLFontElement", "size", "size", Reflect::Text},

    // -- HTMLFormElement ----------------------------------------------------
    {"HTMLFormElement", "acceptCharset", "accept-charset", Reflect::Text},
    {"HTMLFormElement", "action", "action", Reflect::Url_OrDocumentAddress},
    {"HTMLFormElement", "autocomplete", "autocomplete", Reflect::Enumerated, kAutocompleteKeywords,
     "on", "on"},
    {"HTMLFormElement", "enctype", "enctype", Reflect::Enumerated, kEncTypeKeywords,
     "application/x-www-form-urlencoded", "application/x-www-form-urlencoded"},
    // `encoding` is a second name for the same attribute, which is why it is a
    // second row rather than an alias: two rows cannot drift, an alias can.
    {"HTMLFormElement", "encoding", "enctype", Reflect::Enumerated, kEncTypeKeywords,
     "application/x-www-form-urlencoded", "application/x-www-form-urlencoded"},
    {"HTMLFormElement", "method", "method", Reflect::Enumerated, kFormMethodKeywords, "get", "get"},
    {"HTMLFormElement", "name", "name", Reflect::Text},
    {"HTMLFormElement", "noValidate", "novalidate", Reflect::Boolean},
    {"HTMLFormElement", "target", "target", Reflect::Text},

    // -- HTMLFrameElement / HTMLFrameSetElement -----------------------------
    {"HTMLFrameElement", "name", "name", Reflect::Text},
    {"HTMLFrameElement", "scrolling", "scrolling", Reflect::Text},
    {"HTMLFrameElement", "src", "src", Reflect::Url},
    {"HTMLFrameElement", "frameBorder", "frameborder", Reflect::Text},
    {"HTMLFrameElement", "longDesc", "longdesc", Reflect::Url},
    {"HTMLFrameElement", "noResize", "noresize", Reflect::Boolean},
    {"HTMLFrameElement", "marginHeight", "marginheight", Reflect::TextNullToEmpty},
    {"HTMLFrameElement", "marginWidth", "marginwidth", Reflect::TextNullToEmpty},
    {"HTMLFrameSetElement", "cols", "cols", Reflect::Text},
    {"HTMLFrameSetElement", "rows", "rows", Reflect::Text},

    // -- HTMLHRElement ------------------------------------------------------
    {"HTMLHRElement", "align", "align", Reflect::Text},
    {"HTMLHRElement", "color", "color", Reflect::Text},
    {"HTMLHRElement", "noShade", "noshade", Reflect::Boolean},
    {"HTMLHRElement", "size", "size", Reflect::Text},
    {"HTMLHRElement", "width", "width", Reflect::Text},

    // -- HTMLHtmlElement ----------------------------------------------------
    {"HTMLHtmlElement", "version", "version", Reflect::Text},

    // -- HTMLIFrameElement --------------------------------------------------
    {"HTMLIFrameElement", "src", "src", Reflect::Url},
    {"HTMLIFrameElement", "srcdoc", "srcdoc", Reflect::Text},
    {"HTMLIFrameElement", "name", "name", Reflect::Text},
    {"HTMLIFrameElement", "allowFullscreen", "allowfullscreen", Reflect::Boolean},
    {"HTMLIFrameElement", "width", "width", Reflect::Text},
    {"HTMLIFrameElement", "height", "height", Reflect::Text},
    {"HTMLIFrameElement", "referrerPolicy", "referrerpolicy", Reflect::Enumerated,
     kReferrerPolicyKeywords},
    {"HTMLIFrameElement", "align", "align", Reflect::Text},
    {"HTMLIFrameElement", "scrolling", "scrolling", Reflect::Text},
    {"HTMLIFrameElement", "frameBorder", "frameborder", Reflect::Text},
    {"HTMLIFrameElement", "longDesc", "longdesc", Reflect::Url},
    {"HTMLIFrameElement", "marginHeight", "marginheight", Reflect::TextNullToEmpty},
    {"HTMLIFrameElement", "marginWidth", "marginwidth", Reflect::TextNullToEmpty},

    // -- HTMLImageElement ---------------------------------------------------
    {"HTMLImageElement", "alt", "alt", Reflect::Text},
    {"HTMLImageElement", "src", "src", Reflect::Url},
    {"HTMLImageElement", "srcset", "srcset", Reflect::Text},
    {"HTMLImageElement", "sizes", "sizes", Reflect::Text},
    {"HTMLImageElement", "crossOrigin", "crossorigin", Reflect::Enumerated, kCrossOriginKeywords,
     nullptr, "anonymous", true},
    {"HTMLImageElement", "useMap", "usemap", Reflect::Text},
    {"HTMLImageElement", "isMap", "ismap", Reflect::Boolean},
    {"HTMLImageElement", "width", "width", Reflect::UnsignedLong},
    {"HTMLImageElement", "height", "height", Reflect::UnsignedLong},
    {"HTMLImageElement", "referrerPolicy", "referrerpolicy", Reflect::Enumerated,
     kReferrerPolicyKeywords},
    {"HTMLImageElement", "decoding", "decoding", Reflect::Enumerated, kDecodingKeywords, "auto",
     "auto"},
    {"HTMLImageElement", "loading", "loading", Reflect::Enumerated, kLoadingKeywords, "eager",
     "eager"},
    {"HTMLImageElement", "name", "name", Reflect::Text},
    {"HTMLImageElement", "lowsrc", "lowsrc", Reflect::Url},
    {"HTMLImageElement", "align", "align", Reflect::Text},
    {"HTMLImageElement", "hspace", "hspace", Reflect::UnsignedLong},
    {"HTMLImageElement", "vspace", "vspace", Reflect::UnsignedLong},
    {"HTMLImageElement", "longDesc", "longdesc", Reflect::Url},
    {"HTMLImageElement", "border", "border", Reflect::TextNullToEmpty},

    // -- HTMLInputElement ---------------------------------------------------
    {"HTMLInputElement", "accept", "accept", Reflect::Text},
    {"HTMLInputElement", "alt", "alt", Reflect::Text},
    {"HTMLInputElement", "autocomplete", "autocomplete", Reflect::Text},
    {"HTMLInputElement", "defaultChecked", "checked", Reflect::Boolean},
    {"HTMLInputElement", "dirName", "dirname", Reflect::Text},
    {"HTMLInputElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLInputElement", "formAction", "formaction", Reflect::Url_OrDocumentAddress},
    {"HTMLInputElement", "formEnctype", "formenctype", Reflect::Enumerated, kEncTypeKeywords, "",
     "application/x-www-form-urlencoded"},
    {"HTMLInputElement", "formMethod", "formmethod", Reflect::Enumerated, kFormMethodKeywords, "",
     "get"},
    {"HTMLInputElement", "formNoValidate", "formnovalidate", Reflect::Boolean},
    {"HTMLInputElement", "formTarget", "formtarget", Reflect::Text},
    {"HTMLInputElement", "height", "height", Reflect::UnsignedLong},
    {"HTMLInputElement", "max", "max", Reflect::Text},
    {"HTMLInputElement", "maxLength", "maxlength", Reflect::Long_NonNegative, {}, "", nullptr,
     false, -1},
    {"HTMLInputElement", "min", "min", Reflect::Text},
    {"HTMLInputElement", "minLength", "minlength", Reflect::Long_NonNegative, {}, "", nullptr,
     false, -1},
    {"HTMLInputElement", "multiple", "multiple", Reflect::Boolean},
    {"HTMLInputElement", "name", "name", Reflect::Text},
    {"HTMLInputElement", "pattern", "pattern", Reflect::Text},
    {"HTMLInputElement", "placeholder", "placeholder", Reflect::Text},
    {"HTMLInputElement", "readOnly", "readonly", Reflect::Boolean},
    {"HTMLInputElement", "required", "required", Reflect::Boolean},
    {"HTMLInputElement", "size", "size", Reflect::UnsignedLong_NonZero, {}, "", nullptr, false, 20},
    {"HTMLInputElement", "src", "src", Reflect::Url},
    {"HTMLInputElement", "step", "step", Reflect::Text},
    {"HTMLInputElement", "type", "type", Reflect::Enumerated, kInputTypeKeywords, "text", "text"},
    {"HTMLInputElement", "width", "width", Reflect::UnsignedLong},
    {"HTMLInputElement", "defaultValue", "value", Reflect::Text},
    {"HTMLInputElement", "align", "align", Reflect::Text},
    {"HTMLInputElement", "useMap", "usemap", Reflect::Text},
    // Not reflected by HTML -- `value` and `checked` are the control's *state*,
    // which tracks the attribute only until something edits it. They are here
    // because this browser has no separate value state yet, and the engine and
    // a page have to read the same one. See the note in FormAlgorithms.cpp.
    {"HTMLInputElement", "value", "value", Reflect::Text},
    {"HTMLInputElement", "checked", "checked", Reflect::Boolean},

    // -- HTMLLabelElement ---------------------------------------------------
    {"HTMLLabelElement", "htmlFor", "for", Reflect::Text},

    // -- HTMLLinkElement ----------------------------------------------------
    {"HTMLLinkElement", "href", "href", Reflect::Url},
    {"HTMLLinkElement", "crossOrigin", "crossorigin", Reflect::Enumerated, kCrossOriginKeywords,
     nullptr, "anonymous", true},
    {"HTMLLinkElement", "rel", "rel", Reflect::Text},
    {"HTMLLinkElement", "as", "as", Reflect::Enumerated, kLinkAsKeywords},
    {"HTMLLinkElement", "media", "media", Reflect::Text},
    {"HTMLLinkElement", "nonce", "nonce", Reflect::Nonce},
    {"HTMLLinkElement", "integrity", "integrity", Reflect::Text},
    {"HTMLLinkElement", "hreflang", "hreflang", Reflect::Text},
    {"HTMLLinkElement", "type", "type", Reflect::Text},
    {"HTMLLinkElement", "referrerPolicy", "referrerpolicy", Reflect::Enumerated,
     kReferrerPolicyKeywords},
    {"HTMLLinkElement", "charset", "charset", Reflect::Text},
    {"HTMLLinkElement", "rev", "rev", Reflect::Text},
    {"HTMLLinkElement", "target", "target", Reflect::Text},

    // -- HTMLMapElement -----------------------------------------------------
    {"HTMLMapElement", "name", "name", Reflect::Text},

    // -- HTMLMarqueeElement -------------------------------------------------
    {"HTMLMarqueeElement", "behavior", "behavior", Reflect::Enumerated, kMarqueeBehaviorKeywords,
     "scroll", "scroll"},
    {"HTMLMarqueeElement", "bgColor", "bgcolor", Reflect::Text},
    {"HTMLMarqueeElement", "direction", "direction", Reflect::Enumerated, kMarqueeDirectionKeywords,
     "left", "left"},
    {"HTMLMarqueeElement", "height", "height", Reflect::Text},
    {"HTMLMarqueeElement", "hspace", "hspace", Reflect::UnsignedLong},
    {"HTMLMarqueeElement", "scrollAmount", "scrollamount", Reflect::UnsignedLong, {}, "", nullptr,
     false, 6},
    {"HTMLMarqueeElement", "scrollDelay", "scrolldelay", Reflect::UnsignedLong, {}, "", nullptr,
     false, 85},
    {"HTMLMarqueeElement", "trueSpeed", "truespeed", Reflect::Boolean},
    {"HTMLMarqueeElement", "vspace", "vspace", Reflect::UnsignedLong},
    {"HTMLMarqueeElement", "width", "width", Reflect::Text},

    // -- HTMLMediaElement (and the two tags that are one) --------------------
    {"HTMLMediaElement", "src", "src", Reflect::Url},
    {"HTMLMediaElement", "crossOrigin", "crossorigin", Reflect::Enumerated, kCrossOriginKeywords,
     nullptr, "anonymous", true},
    // HTML makes `preload`'s missing *and* invalid value defaults
    // implementation-defined, and the one thing it may not be is the empty
    // string -- a page reading `video.preload` has to get a state it can act on.
    {"HTMLMediaElement", "preload", "preload", Reflect::Enumerated, kPreloadKeywords, "metadata",
     "metadata"},
    {"HTMLMediaElement", "autoplay", "autoplay", Reflect::Boolean},
    {"HTMLMediaElement", "loop", "loop", Reflect::Boolean},
    {"HTMLMediaElement", "controls", "controls", Reflect::Boolean},
    {"HTMLMediaElement", "defaultMuted", "muted", Reflect::Boolean},
    {"HTMLMediaElement", "loading", "loading", Reflect::Enumerated, kLoadingKeywords, "eager",
     "eager"},
    {"HTMLVideoElement", "width", "width", Reflect::UnsignedLong},
    {"HTMLVideoElement", "height", "height", Reflect::UnsignedLong},
    {"HTMLVideoElement", "poster", "poster", Reflect::Url},
    {"HTMLVideoElement", "playsInline", "playsinline", Reflect::Boolean},

    // -- HTMLMetaElement ----------------------------------------------------
    {"HTMLMetaElement", "name", "name", Reflect::Text},
    {"HTMLMetaElement", "httpEquiv", "http-equiv", Reflect::Text},
    {"HTMLMetaElement", "content", "content", Reflect::Text},
    {"HTMLMetaElement", "media", "media", Reflect::Text},
    {"HTMLMetaElement", "scheme", "scheme", Reflect::Text},

    // -- HTMLMeterElement ---------------------------------------------------
    {"HTMLMeterElement", "value", "value", Reflect::Double},
    {"HTMLMeterElement", "min", "min", Reflect::Double},
    {"HTMLMeterElement", "max", "max", Reflect::Double, {}, "", nullptr, false, 1},
    {"HTMLMeterElement", "low", "low", Reflect::Double},
    {"HTMLMeterElement", "high", "high", Reflect::Double},
    {"HTMLMeterElement", "optimum", "optimum", Reflect::Double},

    // -- HTMLModElement -----------------------------------------------------
    {"HTMLModElement", "cite", "cite", Reflect::Url},
    {"HTMLModElement", "dateTime", "datetime", Reflect::Text},

    // -- HTMLObjectElement --------------------------------------------------
    {"HTMLObjectElement", "data", "data", Reflect::Url},
    {"HTMLObjectElement", "type", "type", Reflect::Text},
    {"HTMLObjectElement", "name", "name", Reflect::Text},
    {"HTMLObjectElement", "useMap", "usemap", Reflect::Text},
    {"HTMLObjectElement", "width", "width", Reflect::Text},
    {"HTMLObjectElement", "height", "height", Reflect::Text},
    {"HTMLObjectElement", "align", "align", Reflect::Text},
    {"HTMLObjectElement", "archive", "archive", Reflect::Text},
    {"HTMLObjectElement", "code", "code", Reflect::Text},
    {"HTMLObjectElement", "declare", "declare", Reflect::Boolean},
    {"HTMLObjectElement", "hspace", "hspace", Reflect::UnsignedLong},
    {"HTMLObjectElement", "standby", "standby", Reflect::Text},
    {"HTMLObjectElement", "vspace", "vspace", Reflect::UnsignedLong},
    {"HTMLObjectElement", "codeBase", "codebase", Reflect::Url},
    {"HTMLObjectElement", "codeType", "codetype", Reflect::Text},
    {"HTMLObjectElement", "border", "border", Reflect::TextNullToEmpty},

    // -- HTMLOptGroupElement / HTMLOptionElement ----------------------------
    {"HTMLOptGroupElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLOptGroupElement", "label", "label", Reflect::Text},
    {"HTMLOptionElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLOptionElement", "label", "label", Reflect::Text},
    {"HTMLOptionElement", "defaultSelected", "selected", Reflect::Boolean},
    {"HTMLOptionElement", "value", "value", Reflect::Text},
    // As with `input.value`: the control's state, not HTML's reflection.
    {"HTMLOptionElement", "selected", "selected", Reflect::Boolean},

    // -- HTMLOutputElement --------------------------------------------------
    {"HTMLOutputElement", "name", "name", Reflect::Text},

    // -- HTMLParamElement ---------------------------------------------------
    {"HTMLParamElement", "name", "name", Reflect::Text},
    {"HTMLParamElement", "value", "value", Reflect::Text},
    {"HTMLParamElement", "type", "type", Reflect::Text},
    {"HTMLParamElement", "valueType", "valuetype", Reflect::Text},

    // -- HTMLPreElement -----------------------------------------------------
    {"HTMLPreElement", "width", "width", Reflect::Long},

    // -- HTMLProgressElement ------------------------------------------------
    // Positive-only, and the difference from a plain double is not academic: a
    // set of zero leaves `max` at whatever it was, so `progress.max = 0` cannot
    // divide the bar by nothing.
    {"HTMLProgressElement", "max", "max", Reflect::Double_Positive, {}, "", nullptr, false, 1},

    // -- HTMLQuoteElement ---------------------------------------------------
    {"HTMLQuoteElement", "cite", "cite", Reflect::Url},

    // -- HTMLScriptElement --------------------------------------------------
    {"HTMLScriptElement", "src", "src", Reflect::Url},
    {"HTMLScriptElement", "type", "type", Reflect::Text},
    {"HTMLScriptElement", "noModule", "nomodule", Reflect::Boolean},
    {"HTMLScriptElement", "charset", "charset", Reflect::Text},
    {"HTMLScriptElement", "async", "async", Reflect::Boolean},
    {"HTMLScriptElement", "defer", "defer", Reflect::Boolean},
    {"HTMLScriptElement", "crossOrigin", "crossorigin", Reflect::Enumerated, kCrossOriginKeywords,
     nullptr, "anonymous", true},
    {"HTMLScriptElement", "integrity", "integrity", Reflect::Text},
    {"HTMLScriptElement", "nonce", "nonce", Reflect::Nonce},
    {"HTMLScriptElement", "event", "event", Reflect::Text},
    {"HTMLScriptElement", "htmlFor", "for", Reflect::Text},

    // -- HTMLSelectElement --------------------------------------------------
    {"HTMLSelectElement", "autocomplete", "autocomplete", Reflect::Text},
    {"HTMLSelectElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLSelectElement", "multiple", "multiple", Reflect::Boolean},
    {"HTMLSelectElement", "name", "name", Reflect::Text},
    {"HTMLSelectElement", "required", "required", Reflect::Boolean},
    {"HTMLSelectElement", "size", "size", Reflect::UnsignedLong},

    // -- HTMLSlotElement ----------------------------------------------------
    {"HTMLSlotElement", "name", "name", Reflect::Text},

    // -- HTMLSourceElement --------------------------------------------------
    {"HTMLSourceElement", "src", "src", Reflect::Url},
    {"HTMLSourceElement", "type", "type", Reflect::Text},
    {"HTMLSourceElement", "srcset", "srcset", Reflect::Text},
    {"HTMLSourceElement", "sizes", "sizes", Reflect::Text},
    {"HTMLSourceElement", "media", "media", Reflect::Text},

    // -- HTMLStyleElement ---------------------------------------------------
    {"HTMLStyleElement", "media", "media", Reflect::Text},
    {"HTMLStyleElement", "nonce", "nonce", Reflect::Nonce},
    {"HTMLStyleElement", "type", "type", Reflect::Text},

    // -- The table interfaces ------------------------------------------------
    {"HTMLTableElement", "align", "align", Reflect::Text},
    {"HTMLTableElement", "border", "border", Reflect::Text},
    {"HTMLTableElement", "frame", "frame", Reflect::Text},
    {"HTMLTableElement", "rules", "rules", Reflect::Text},
    {"HTMLTableElement", "summary", "summary", Reflect::Text},
    {"HTMLTableElement", "width", "width", Reflect::Text},
    {"HTMLTableElement", "bgColor", "bgcolor", Reflect::TextNullToEmpty},
    {"HTMLTableElement", "cellPadding", "cellpadding", Reflect::TextNullToEmpty},
    {"HTMLTableElement", "cellSpacing", "cellspacing", Reflect::TextNullToEmpty},

    {"HTMLTableCaptionElement", "align", "align", Reflect::Text},

    {"HTMLTableColElement", "span", "span", Reflect::UnsignedLong_Clamped, {}, "", nullptr, false, 1,
     1, 1000},
    {"HTMLTableColElement", "align", "align", Reflect::Text},
    {"HTMLTableColElement", "ch", "char", Reflect::Text},
    {"HTMLTableColElement", "chOff", "charoff", Reflect::Text},
    {"HTMLTableColElement", "vAlign", "valign", Reflect::Text},
    {"HTMLTableColElement", "width", "width", Reflect::Text},

    {"HTMLTableSectionElement", "align", "align", Reflect::Text},
    {"HTMLTableSectionElement", "ch", "char", Reflect::Text},
    {"HTMLTableSectionElement", "chOff", "charoff", Reflect::Text},
    {"HTMLTableSectionElement", "vAlign", "valign", Reflect::Text},

    {"HTMLTableRowElement", "align", "align", Reflect::Text},
    {"HTMLTableRowElement", "ch", "char", Reflect::Text},
    {"HTMLTableRowElement", "chOff", "charoff", Reflect::Text},
    {"HTMLTableRowElement", "vAlign", "valign", Reflect::Text},
    {"HTMLTableRowElement", "bgColor", "bgcolor", Reflect::TextNullToEmpty},

    // The two clamps differ, and they differ in the specification: a cell may
    // span zero rows (meaning "to the end of the section") and may not span
    // zero columns.
    {"HTMLTableCellElement", "colSpan", "colspan", Reflect::UnsignedLong_Clamped, {}, "", nullptr,
     false, 1, 1, 1000},
    {"HTMLTableCellElement", "rowSpan", "rowspan", Reflect::UnsignedLong_Clamped, {}, "", nullptr,
     false, 1, 0, 65534},
    {"HTMLTableCellElement", "headers", "headers", Reflect::Text},
    {"HTMLTableCellElement", "scope", "scope", Reflect::Enumerated, kScopeKeywords},
    {"HTMLTableCellElement", "abbr", "abbr", Reflect::Text},
    {"HTMLTableCellElement", "align", "align", Reflect::Text},
    {"HTMLTableCellElement", "axis", "axis", Reflect::Text},
    {"HTMLTableCellElement", "height", "height", Reflect::Text},
    {"HTMLTableCellElement", "width", "width", Reflect::Text},
    {"HTMLTableCellElement", "ch", "char", Reflect::Text},
    {"HTMLTableCellElement", "chOff", "charoff", Reflect::Text},
    {"HTMLTableCellElement", "noWrap", "nowrap", Reflect::Boolean},
    {"HTMLTableCellElement", "vAlign", "valign", Reflect::Text},
    {"HTMLTableCellElement", "bgColor", "bgcolor", Reflect::TextNullToEmpty},

    // -- HTMLTextAreaElement -------------------------------------------------
    {"HTMLTextAreaElement", "autocomplete", "autocomplete", Reflect::Text},
    {"HTMLTextAreaElement", "cols", "cols", Reflect::UnsignedLong_Fallback, {}, "", nullptr, false,
     20},
    {"HTMLTextAreaElement", "dirName", "dirname", Reflect::Text},
    {"HTMLTextAreaElement", "disabled", "disabled", Reflect::Boolean},
    {"HTMLTextAreaElement", "maxLength", "maxlength", Reflect::Long_NonNegative, {}, "", nullptr,
     false, -1},
    {"HTMLTextAreaElement", "minLength", "minlength", Reflect::Long_NonNegative, {}, "", nullptr,
     false, -1},
    {"HTMLTextAreaElement", "name", "name", Reflect::Text},
    {"HTMLTextAreaElement", "placeholder", "placeholder", Reflect::Text},
    {"HTMLTextAreaElement", "readOnly", "readonly", Reflect::Boolean},
    {"HTMLTextAreaElement", "required", "required", Reflect::Boolean},
    {"HTMLTextAreaElement", "rows", "rows", Reflect::UnsignedLong_Fallback, {}, "", nullptr, false,
     2},
    {"HTMLTextAreaElement", "wrap", "wrap", Reflect::Text},
    {"HTMLTextAreaElement", "value", "value", Reflect::TextareaValue},

    // -- HTMLTimeElement ----------------------------------------------------
    {"HTMLTimeElement", "dateTime", "datetime", Reflect::Text},

    // -- HTMLTrackElement ---------------------------------------------------
    {"HTMLTrackElement", "kind", "kind", Reflect::Enumerated, kTrackKindKeywords, "subtitles",
     "metadata"},
    {"HTMLTrackElement", "src", "src", Reflect::Url},
    {"HTMLTrackElement", "srclang", "srclang", Reflect::Text},
    {"HTMLTrackElement", "label", "label", Reflect::Text},
    {"HTMLTrackElement", "default", "default", Reflect::Boolean},
};

// The reflections that hang off `Document` and describe some *other* element.
// HTML keeps them for compatibility with documents written before CSS, and they
// are the reason this is a second table rather than six more rows above: the
// receiver and the element carrying the attribute are not the same object.
constexpr DocumentReflection kDocumentReflections[] = {
    {{"Document", "dir", "dir", Reflect::Enumerated, kDirKeywords}, false},
    {{"Document", "fgColor", "text", Reflect::TextNullToEmpty}, true},
    {{"Document", "linkColor", "link", Reflect::TextNullToEmpty}, true},
    {{"Document", "vlinkColor", "vlink", Reflect::TextNullToEmpty}, true},
    {{"Document", "alinkColor", "alink", Reflect::TextNullToEmpty}, true},
    {{"Document", "bgColor", "bgcolor", Reflect::TextNullToEmpty}, true},
};

}  // namespace

std::span<const Reflection> ReflectionTable() { return kReflections; }

std::span<const DocumentReflection> DocumentReflectionTable() { return kDocumentReflections; }

}  // namespace microbrowser::bindings
