#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "url/Url.h"

namespace microbrowser::engine {

// An href as it appears in a document, resolved against the document's own
// address. Absolute stays absolute; relative is joined to the base; anything
// the URL parser refuses comes back empty rather than as a guess.
//
// Internal to this module -- it is not on `public:` in MODULE.deps -- and a
// header rather than a static because two translation units need it: the one
// that follows a clicked link and the one that submits a form. Two copies of a
// URL resolution is two chances to disagree about what a page's own links mean.
inline std::optional<std::string> ResolveLink(std::string_view href,
                                              std::string_view document_url) {
  if (const std::optional<url::Url> absolute = url::Url::Parse(href)) {
    return absolute->Serialize();
  }
  const std::optional<url::Url> base = url::Url::Parse(document_url);
  if (!base.has_value()) {
    return std::nullopt;
  }
  const std::optional<url::Url> resolved = url::Url::Parse(href, *base);
  return resolved.has_value() ? std::optional<std::string>(resolved->Serialize()) : std::nullopt;
}

}  // namespace microbrowser::engine
