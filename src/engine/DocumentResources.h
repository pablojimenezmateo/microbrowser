#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "css/MediaQuery.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "gfx/Image.h"
#include "engine/Subresource.h"

namespace microbrowser::engine {

// What one document needs fetching and what came back. A struct with no invariant,
// budgeted so that growth in "what a document's resources are" stays a decision.
struct DocumentResources {
  std::vector<SubresourceRequest> pending_sheets;
  // The `<style>` text inside each shadow root, with the root it belongs to.
  // Kept as the pair rather than added straight to the resolver because
  // RebuildAuthorStyleSheets throws the resolver away and rebuilds it, and a
  // component's styles have to come back with it. ADR 0019 §3.
  std::vector<std::pair<const dom::Node*, std::string>> shadow_sheets;
  // The `@font-face` blocks the author sheets declared, and the URLs already
  // asked for. Kept because RebuildAuthorStyleSheets throws the parsed sheet
  // away, and a face declared in the first sheet must survive the second
  // arriving.
  std::vector<css::FontFace> font_faces;
  std::set<std::string, std::less<>> requested_fonts;
  std::vector<std::size_t> pending_sheet_slots;
  std::vector<std::optional<std::string>> author_sheet_slots;
  // The same sheets, parsed. RebuildAuthorStyleSheets runs whenever a sheet
  // arrives, a shadow root's `<style>` changes or the viewport does, and it
  // re-tokenized every sheet from text on each one -- 626,658 CSS tokens per
  // rebuild on youtube.com, six rebuilds, 820ms. A stylesheet's text does not
  // change once it has arrived, so the parse can be kept.
  //
  // The viewport is part of the key and has to be: `@media` is evaluated at
  // *parse* time today, so the parsed form is only valid for the size it was
  // parsed at. That is TD-0002 -- the end state keeps the condition on the
  // rule and asks during the cascade, and then this cache needs no key at all.
  // The same sheets, parsed, positionally against the two lists above.
  //
  // Deliberately *not* keyed by the sheet's text. Keeping the text as a key
  // would mean a second copy of every stylesheet on the page, which is a
  // worse trade than the re-parse it avoids. Instead the cache is dropped at
  // the three places that can invalidate it, and there are only three:
  // CollectStyleSheets, which clears the slots and re-derives them from the
  // document so slot 3 need not still mean sheet 3; SetAuthorStyleSheet,
  // where a pending sheet's text arrives; and CollectShadowStyleSheets
  // returning true, which is exactly its "the text moved" answer. An
  // invalidation missed here is not a slow page, it is the wrong rules, so
  // they are listed by name at each site.
  //
  // The viewport is the fourth, and unlike the others it drops everything:
  // `@media` is evaluated at *parse* time (TD-0002), so a parsed sheet is
  // only valid for the size it was parsed at. When the condition moves onto
  // the rule, this key goes away with it.
  std::vector<std::optional<css::StyleSheet>> author_sheet_parsed;
  std::vector<std::optional<css::StyleSheet>> shadow_sheets_parsed;
  css::MediaContext parsed_at_viewport;
  std::vector<std::string> pending_images;
  std::map<std::string, std::shared_ptr<const gfx::Image>, std::less<>> images;
  // The lazy images this document has not asked for yet, and their chosen
  // URL. Keyed by element because "is it near the scrollport" is a question
  // about a box, and two `<img loading="lazy">` sharing a URL are two boxes.
  std::map<const dom::Element*, std::string> deferred_images;
  // Every image URL the loader has already been told to fetch. It survives
  // CollectImages, which rebuilds `pending_images` from the document from
  // scratch -- without this, every stylesheet that lands would re-request the
  // whole page's images.
  std::set<std::string, std::less<>> requested_images;
  // Which candidate each <img> resolved to. Recorded rather than recomputed
  // because selection depends on the viewport and the fetch does not: an
  // element whose chosen URL changed after its image was fetched would
  // otherwise render as nothing at all.
  std::map<const dom::Element*, std::string> selected_image_urls;
};

}  // namespace microbrowser::engine
