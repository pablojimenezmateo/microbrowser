#pragma once

#include <cstdint>
#include <string_view>

namespace microbrowser::csp {

// Subresource Integrity: `integrity="sha384-…"` on a `<script>` or a
// `<link rel=stylesheet>`.
//
// ADR 0020 §4. In this module rather than beside its caller because it is the
// same kind of thing as Content-Security-Policy and wants the same treatment:
// the *page* declares a restriction on its own resources, and the browser
// enforces it against bytes a third party supplied. It is the only mechanism
// here that protects the user against the site's *own CDN* -- a threat the site
// chose to defend against, and one we would otherwise silently discard.
//
// The metadata is a page's and the bytes are a server's, so both are hostile
// and neither is trusted: a token that does not parse is dropped, and a
// resource whose only tokens were dropped is treated as having no metadata --
// which is the specification's answer and the safe one, because the alternative
// is a typo in an attribute taking a site offline.

enum class IntegrityResult : std::uint8_t {
  // No usable hash was named. The resource is allowed: an `integrity` this
  // browser cannot read must not be stricter than no `integrity` at all.
  NoMetadata,
  Match,
  // At least one usable hash was named and none of the strongest ones matched.
  // The resource must not be executed or applied.
  Mismatch,
};

IntegrityResult CheckIntegrity(std::string_view metadata, std::string_view bytes);

// Whether `metadata` names at least one hash this browser understands.
//
// The caller needs this *before* the bytes exist, because it decides how the
// request is made: a cross-origin resource with integrity metadata has to be
// fetched with CORS, or a page could use the check itself as an oracle -- one
// guess at a time -- for content it is not allowed to read. See
// Engine::StartSubresources.
bool HasIntegrityMetadata(std::string_view metadata);

}  // namespace microbrowser::csp
