#pragma once

#include "js/Interpreter.h"
#include "js/Value.h"

// The two shadow-DOM installers that do not need to be members of DomBindings,
// declared here so that they are not.
//
// **This header exists because of the module's line cap.** `DomBindings.h` sat
// one line under it, and the note above `max_tu_lines` in MODULE.deps has said
// for five raises now that the answer is a split rather than more headroom. A
// *member* function cannot leave a class's header, so the split has to start
// with the functions that were never members in the first place -- and these
// two are exactly that: everything they touch is the interpreter, the interface
// object they install onto, and the owner pointer that goes in `kOwnerSlot`.
// None of it is private state.
//
// Same standing as LiveRanges.h and Geometry.h: private to the module, because
// a binding is an implementation detail of the seam rather than part of it.

namespace microbrowser::bindings {

class DomBindings;

// The `shadowroot*` content attributes on HTMLTemplateElement, reflected.
// Reflection and nothing more -- the parser is the only thing that acts on
// them, and by the time script can write one it has already been past.
void InstallTemplateShadowReflection(DomBindings& owner, js::Interpreter& interpreter,
                                     const js::Value& template_interface);

// `attachInternals()`, and only the one piece of ElementInternals that has
// anything behind it here. See the definition for why the rest is absent rather
// than stubbed.
void InstallElementInternals(DomBindings& owner, js::Interpreter& interpreter,
                             const js::Value& html_element_interface);

}  // namespace microbrowser::bindings
