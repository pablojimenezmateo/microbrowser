#pragma once

#include <memory>
#include <string_view>

#include "dom/Node.h"

// Which parser a document gets, and why that is a decision rather than a call.
//
// Its own translation unit because `Page.cpp` is at its module's line cap, and the cap is written
// to mean a missing seam rather than a bigger file. This is a seam: everything above it is one
// document's lifecycle, and everything in it is about bytes and a MIME type.
namespace microbrowser::engine {

// Is this document XML? The MIME types HTML's "navigate" step routes to the XML parser, and no
// others: `text/html` is the HTML tree builder whatever the file is called.
bool IsXmlContentType(std::string_view content_type);

// `source`, as a document, by whichever parser `content_type` names.
std::unique_ptr<dom::Document> ParseDocumentFor(std::string_view source,
                                                std::string_view content_type);

}  // namespace microbrowser::engine
