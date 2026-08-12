#pragma once

#include "js/Interpreter.h"
#include "js/Value.h"

// `contentDocument` and `contentWindow` on `<iframe>` -- ADR 0027 §1.
//
// A free function rather than a `DomBindings` method, for the reason
// ShadowDom.h gives at length: that header is at the module's line cap, and the
// answer the note above `max_tu_lines` has been asking for is a split rather
// than another raise. This installer was never a member -- everything it
// touches is the interpreter, the interface object it installs onto, the owner
// pointer that goes in `kOwnerSlot`, and the public `WrapperFor`.
//
// Private to the module, same standing as ShadowDom.h and LiveRanges.h.

namespace microbrowser::bindings {

class DomBindings;

// **The origin check is not here and cannot be.** This module has no way to
// decide whether two documents are same-origin without seeing `src/url` from
// the binding layer's own code paths; `src/engine` attaches a child document to
// the `<iframe>` element only when the two *are* same-origin, so a cross-origin
// frame has nothing here to return. That is the check being structural rather
// than a test a future caller could forget -- ADR 0027 §2, ADR 0008.
void InstallFrameElement(DomBindings& owner, js::Interpreter& interpreter,
                         const js::Value& iframe_interface);

}  // namespace microbrowser::bindings
