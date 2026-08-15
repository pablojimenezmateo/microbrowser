#pragma once

#include "html/Encoding.h"
#include "js/Interpreter.h"

// Facts about a document that only `src/engine` knows, published onto the realm
// its script runs in. ADR 0008's inversion, in its smallest form.
//
// **On the realm's global rather than on `DomBindings`, and that is the right
// home rather than the cheap one.** A document's character set is per *document*,
// and since ADR 0042 §5 a same-origin `<iframe>` is a realm of the same
// interpreter with a global of its own -- so the global is exactly the object
// that is one-per-document. A member on the binding layer would have been one
// per layer, which is the same thing today and stops being it the moment two
// documents share one.
//
// It also costs `DomBindings` nothing, which matters: that class is at 1,000 of
// its 1,000 permitted lines and its `MODULE.deps` note has asked for a split
// rather than a raise seven times now. A fact that was never a member does not
// become one.
//
// The slot name is not an identifier, so a page can neither read it nor shadow
// it -- the same convention `#domInterfaces` and `#domWrappers` already use.

namespace microbrowser::bindings {

// Written once per document, after `DomBindings::Install` and before any script.
void SetDocumentEncoding(js::Interpreter& interpreter, html::Encoding encoding);

// What HTML's "encoding-parse a URL" encodes a query with. UTF-8 when nothing
// published one, which is what a document with no legacy encoding behind it is.
html::Encoding DocumentEncodingOf(js::Interpreter& interpreter);

}  // namespace microbrowser::bindings
