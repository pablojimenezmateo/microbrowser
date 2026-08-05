#pragma once

#include <optional>

#include "dom/Node.h"

namespace microbrowser::html {

// Which elements can hold focus, and in what order Tab visits them.
//
// Its own header rather than more predicates in FormControl.h because the
// question is not about forms: a link, a `contenteditable` div and anything
// carrying `tabindex` are focusable and none of them is a form control. It is
// element semantics, which is what this module is for -- and keeping it here
// rather than in the engine is what makes "can this hold focus" have one
// answer for the click path, the Tab walk and `element.focus()`. Three answers
// is how a page ends up with a field the mouse can reach and Tab cannot.

// The parsed `tabindex` attribute, or nothing when it is absent or not an
// integer. Attacker-controlled text, so it saturates rather than wrapping: a
// value outside the range below is clamped, because `tabindex="99999999999999"`
// must order the element last rather than overflow into ordering it first.
std::optional<int> TabIndex(const dom::Element& element);

// Whether `element` can hold focus at all: a click on it, or `focus()` from
// script, moves focus to it.
//
// A negative `tabindex` is focusable and not tab-reachable, which is the whole
// reason the attribute takes negative values -- a scripted menu wants to focus
// its own items without putting every one of them in the Tab order.
bool IsFocusable(const dom::Element& element);

// Whether Tab stops on `element`. Everything focusable except the ones with a
// negative `tabindex`.
bool IsTabReachable(const dom::Element& element);

}  // namespace microbrowser::html
