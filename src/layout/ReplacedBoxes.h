#pragma once

#include <string>
#include <string_view>

#include "layout/Box.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::layout {

// How large a replaced element is, and what it says.
//
// Private to src/layout. Extracted from LayoutEngine.cpp when that file went over its module cap,
// and the cap was pointing at something real: this is a coherent question -- "an element whose
// content comes from outside CSS: how big is it, and what text does the user agent put in it?" --
// asked by the box builder and by nothing else. `<img>`, the form controls, and now `<video>` and
// `<audio>` (ADR 0028 §1) all answer it differently, and the answers are a table rather than part
// of the layout algorithm.

// White space collapsed to single spaces, with the leading and trailing ones kept -- they carry
// information across an element boundary. Declared here rather than duplicated because the box
// builder and the replaced-element text both need it, and two collapsers would be two answers to
// "what does this element display".
std::string CollapseWhitespace(std::string_view text);

// Whether this element's content comes from outside CSS, so its children generate no boxes.
bool IsReplacedElement(const dom::Element& element);

// Whether the user agent draws its own controls for it. ADR 0028 §1 puts media controls in
// ADR 0018's category: boxes the user agent creates inside a page rather than widgets `src/ui`
// owns -- which is why an `<audio>` with them has a size and one without has none.
bool HasDefaultControls(const dom::Element& element);

// The used width and height, in the order the cascade, the presentational attributes, the aspect
// ratio and the intrinsic size are consulted.
float ReplacedWidth(const Box& box);
float ReplacedHeight(const Box& box);

// What a form control displays: a `<select>`'s selected option, a `<button>`'s label, a text
// field's value or placeholder.
std::string FormControlText(const dom::Element& element);

}  // namespace microbrowser::layout
