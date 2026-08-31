#pragma once

#include "js/Interpreter.h"

// `document.fonts` -- the readiness half of the CSS Font Loading API, and only that half.
//
// **Why this exists at all is a measurement rather than a feature request.** 714 web-platform-tests
// files begin `<body onload="document.fonts.ready.then(() => { checkLayout('.grid'); })">`, 292 of
// them in `css/css-grid/` alone. With `document.fonts` undefined the handler throws before it
// reaches `checkLayout`, `done()` is never called, and the file reports **no subtests at all** -- so
// it is not in this project's denominator or in Firefox's comparison, and no amount of work on the
// feature the file tests can make it visible. That is the plan's `blocked` column, and it was 292
// of `css/css-grid/`'s 326 blocked files in one cause.
//
// **What is here is `ready` and `status`, and the rest is absent rather than stubbed** (ADR 0012).
// `check()`, `load()`, `add()`, `delete()`, iteration and the `loading`/`loadingdone` events are not
// defined, so a page feature-detecting any of them finds it missing and takes its fallback -- which
// is the whole of that ADR's argument. A `check()` that answered without consulting the font
// database would be worse than none.
//
// **The deviation to know about.** `ready` settles when the document's load event fires, which is
// the moment the engine's own load-complete predicate has already required `font_fetches_` to be
// empty -- so for a document's initial fonts it is exactly right. A font requested *after* load, by
// a script or by a stylesheet a script inserted, does not put `ready` back to pending: the
// specification says it should, and doing it needs a per-load signal from the engine that does not
// exist yet. Every one of the 714 files reads `ready` inside `onload`, so none of them can see the
// difference; a page that loads a font later can. It is written here rather than discovered later.
//
// No state on `DomBindings`, which is at its permitted line count and whose `MODULE.deps` note has
// asked for a split rather than a raise seven times. The set and its promise live in the JavaScript
// heap on the document object, where the collector can see them -- the same reason
// `customElements.whenDefined` parks its pending promises on the registry.

namespace microbrowser::bindings {

// This document's `FontFaceSet`, made on first use and cached on the document object. Null only
// when the heap could not allocate. Per *document* rather than per binding layer, because since
// ADR 0042 §5 a same-origin `<iframe>` is another document sharing this interpreter.
js::Object* FontFaceSetFor(js::Interpreter& interpreter, js::Object& document);

// Resolves this document's `ready` promise and moves `status` to "loaded". Called from
// `DomBindings::NotifyLoad`, *before* the load event is dispatched, so that a handler which does
// `document.fonts.ready.then(f)` gets a promise that is already settled and sees `f` run in the
// microtask drain at the end of that dispatch rather than never.
void SettleFontsReady(js::Interpreter& interpreter, js::Object& document);

}  // namespace microbrowser::bindings
