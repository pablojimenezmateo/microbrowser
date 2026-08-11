#pragma once

#include <string>

#include "dom/Node.h"

// XML serialization: a node written back out as XML, prefixes and all.
//
// This is **not** `dom::Node::Serialize`, and the difference is the whole
// reason it is a separate function rather than a flag. HTML serialization
// writes a tag name and its attributes; XML serialization has to decide, for
// every element and every attribute, *which prefix* to write it under -- which
// depends on what is in scope at that point in the tree, on what the element
// itself declares, and on a counter for prefixes it has to invent
// (`ns1`, `ns2`, …). Two elements with the same name and namespace serialize
// differently depending on where they are.
//
// The algorithm is the one in "DOM Parsing and Serialization", followed
// literally, including the parts of it that are widely agreed to be strange --
// a serializer that quietly improved on it would produce documents that no
// other engine round-trips, which is the failure mode this project's ADR 0012
// is about.
//
// One deliberate deviation, and it is the one every engine makes: an attribute
// whose local name is `xmlns` and which has no prefix is treated as a
// default-namespace declaration **whatever namespace it is in**. The
// specification only says that for attributes in the xmlns namespace, so
// `element.setAttribute("xmlns", "…")` -- which puts the attribute in *no*
// namespace -- would otherwise be written out beside a contradicting one that
// the element's real namespace produced. See the "matching on local name"
// cases in `domparsing/XMLSerializer-serializeToString.html`.
//
// Nothing here is a sanitizer. A round trip through it is not a security
// boundary, for the same reason `dom::Node::Serialize`'s comment says so.

namespace microbrowser::xml {

std::string SerializeXml(const dom::Node& node);

}  // namespace microbrowser::xml
