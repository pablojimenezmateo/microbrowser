// The subresources a document names: its stylesheets, and its images.
//
// Split out of Page.cpp when that file reached its module cap, and the split is
// a real seam rather than an arbitrary cut. Everything here answers one
// question -- *what else does this document need fetching, and what does it do
// with the answer* -- and none of it turns a document into boxes, which is what
// the rest of Page.cpp does.
//
// The one thing worth knowing before changing anything here: an image is keyed
// by the URL **as the document wrote it**, not as it resolves. Resolving is the
// loader's job, and doing it here as well is how the two end up disagreeing and
// an <img> draws nothing.

#include "engine/Page.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "css/StyleSheet.h"
#include "engine/ImageSelection.h"
#include "gfx/SvgDecoder.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Splits an attribute on ASCII whitespace, per the HTML spec's
// "space-separated tokens".
std::vector<std::string_view> SplitTokens(std::string_view value) {
  std::vector<std::string_view> tokens;
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' ||
                                value[i] == '\r' || value[i] == '\f')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < value.size() && value[i] != ' ' && value[i] != '\t' && value[i] != '\n' &&
           value[i] != '\r' && value[i] != '\f') {
      ++i;
    }
    if (i > start) {
      tokens.push_back(value.substr(start, i - start));
    }
  }
  return tokens;
}

// The text of an element's direct text children, concatenated.
std::string DirectText(const dom::Element& element) {
  std::string text;
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsText()) {
      text += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return text;
}

// The `nonce` an element carries, or empty. A string rather than a pointer so
// that "no nonce" and "an empty nonce" are the same thing, which is what the
// policy needs: an empty nonce never matches a nonce-source.
std::string_view NonceOf(const dom::Element& element) {
  const std::string* nonce = element.GetAttribute("nonce");
  return nonce == nullptr ? std::string_view{} : std::string_view(*nonce);
}

// The `integrity` and `crossorigin` of one element, as one value. Here rather
// than at each of its two callers so that a `<script>` and a `<link>` cannot
// come to read the pair differently -- which is the whole risk in ADR 0020 §4's
// rule that the two are read together.
SubresourceRequest RequestFor(const dom::Element& element, std::string url) {
  SubresourceRequest request;
  request.url = std::move(url);
  if (const std::string* integrity = element.GetAttribute("integrity")) {
    request.integrity = *integrity;
  }
  if (const std::string* cross_origin = element.GetAttribute("crossorigin")) {
    request.cross_origin = *cross_origin;
  }
  return request;
}

bool IsLinkedStyleSheet(const dom::Element& link) {
  if (link.TagName() != "link") {
    return false;
  }
  // `rel` is a space-separated set of tokens, and a sheet is only a sheet
  // when "stylesheet" is one of them: `rel="alternate stylesheet"` is not
  // applied, and `rel="preload"` is not a stylesheet at all.
  const std::string* rel = link.GetAttribute("rel");
  if (rel == nullptr) {
    return false;
  }
  bool is_stylesheet = false;
  bool is_alternate = false;
  for (const std::string_view token : SplitTokens(*rel)) {
    is_stylesheet = is_stylesheet || util::EqualsAsciiCaseInsensitive(token, "stylesheet");
    is_alternate = is_alternate || util::EqualsAsciiCaseInsensitive(token, "alternate");
  }
  return is_stylesheet && !is_alternate;
}

}  // namespace

void Page::ApplyDocumentHeadPolicy() {
  if (document_ == nullptr) {
    return;
  }
  // The `<meta>` policies first, then the `<base>`: a policy the document
  // declares governs that document's own `<base href>`, and doing them in the
  // other order would let a page point its relative URLs anywhere and then
  // declare a policy that pretends it did not.
  for (const dom::Element* meta : document_->ElementsByTagName("meta")) {
    const std::string* equiv = meta->GetAttribute("http-equiv");
    if (equiv == nullptr ||
        !util::EqualsAsciiCaseInsensitive(*equiv, "content-security-policy")) {
      continue;
    }
    const std::string* content = meta->GetAttribute("content");
    if (content == nullptr || content->empty()) {
      continue;
    }
    policy_.AddFromMeta(*content);
    AddPerformanceCounter(PerfCounterId::CspPolicies);
  }
  // `<base href>`, which is the first thing in this browser to change what a
  // relative URL in a document means. The *first* one with an href wins, which
  // is the specification's rule -- a second `<base>` is ignored, so a script
  // that appends one cannot retarget every link on the page.
  for (const dom::Element* base : document_->ElementsByTagName("base")) {
    const std::string* href = base->GetAttribute("href");
    if (href == nullptr || href->empty()) {
      continue;
    }
    policy_.SetBase(*href);
    break;
  }
}

bool Page::CollectShadowStyleSheets() {
  std::vector<std::pair<const dom::Node*, std::string>> found;
  if (document_ != nullptr) {
    for (const dom::SharedConstructableSheet& sheet : document_->AdoptedStyleSheets()) {
      if (sheet != nullptr && !sheet->empty()) {
        // Null scope: document-wide adopted sheets use the same scope as author
        // sheets (nullptr), not the document node pointer.
        found.emplace_back(nullptr, *sheet);
      }
    }
    document_->ForEachDescendant([&found](const dom::Node& node) {
      if (!node.IsElement()) {
        return;
      }
      const dom::DocumentFragment* root = static_cast<const dom::Element&>(node).ShadowRoot();
      if (root == nullptr) {
        return;
      }
      // Only this root's own `<style>` elements. A nested shadow root inside it
      // has its own scope, and it is reached when the walk gets to its host.
      root->ForEachDescendant([&found, root](const dom::Node& inner) {
        if (inner.IsElement() && static_cast<const dom::Element&>(inner).TagName() == "style") {
          found.emplace_back(root, DirectText(static_cast<const dom::Element&>(inner)));
        }
      });
      for (const dom::SharedConstructableSheet& sheet :
           static_cast<const dom::DocumentFragment*>(root)->AdoptedStyleSheets()) {
        if (sheet != nullptr && !sheet->empty()) {
          found.emplace_back(root, *sheet);
        }
      }
    });
  }
  if (found == resources_.shadow_sheets) {
    // Unchanged, so nothing is re-parsed. This is what makes calling it after
    // every mutation affordable: the comparison is over the *text*, so a
    // component whose contents change without its stylesheet changing costs one
    // walk rather than a re-parse.
    return false;
  }
  resources_.shadow_sheets = std::move(found);
  // The text moved, which is the one thing this function reports and the one
  // thing that invalidates a parsed shadow sheet. Dropped here rather than in
  // the rebuild so that the list and its parses cannot get out of step.
  resources_.shadow_sheets_parsed.clear();
  return true;
}

void Page::CollectStyleSheets() {
  resources_.pending_sheets.clear();
  resources_.pending_sheet_slots.clear();
  resources_.author_sheet_slots.clear();
  // The slots are about to be re-derived from the document, so slot 3 need not
  // still mean the sheet slot 3 meant -- a script that inserts a `<style>`
  // renumbers everything after it. See DocumentResources::author_sheet_parsed.
  resources_.author_sheet_parsed.clear();
  if (document_ == nullptr) {
    return;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() == "style") {
      const std::string text = DirectText(element);
      // `style-src` governs an inline sheet exactly as `script-src` governs an
      // inline script, and a refused sheet is not applied rather than applied
      // and then hidden -- ADR 0020 §3, enforced and not logged.
      if (!policy_.AllowsInline(csp::Directive::Style, NonceOf(element), text)) {
        AddPerformanceCounter(PerfCounterId::CspInlineBlocked);
        return;
      }
      resources_.author_sheet_slots.emplace_back(text);
      return;
    }
    if (!IsLinkedStyleSheet(element)) {
      return;
    }
    const std::string* href = element.GetAttribute("href");
    if (href == nullptr || href->empty()) {
      return;
    }
    // Refused before it is requested, which is what enforcement means: a
    // stylesheet the policy forbids must not become a request the server sees.
    if (!policy_.AllowsUrl(csp::Directive::Style, *href, NonceOf(element))) {
      return;
    }
    resources_.pending_sheets.push_back(RequestFor(element, *href));
    resources_.pending_sheet_slots.push_back(resources_.author_sheet_slots.size());
    resources_.author_sheet_slots.push_back(std::nullopt);
  });
  CollectShadowStyleSheets();
  RebuildAuthorStyleSheets();
}

void Page::RebuildAuthorStyleSheets() {
  // Every author sheet is re-tokenized and re-parsed here, and this runs again
  // whenever a shadow root's `<style>` changes or the viewport does. On
  // youtube.com that is 626,658 CSS tokens per rebuild. The scope is what makes
  // the re-parse visible; see TD-0002 for why it is still a re-parse.
  util::PerformanceTrace::Scope rebuild_scope("engine::RebuildAuthorStyleSheets");
  ResetResolver();
  resources_.font_faces.clear();

  // A sheet's text does not change once it has arrived, so its parse is kept.
  // What *can* change under it is the viewport, because `@media` is evaluated
  // at parse time -- TD-0002 -- so a resize throws the whole cache away rather
  // than serving rules that were selected for a different width.
  if (!(resources_.parsed_at_viewport == viewport_)) {
    resources_.author_sheet_parsed.clear();
    resources_.shadow_sheets_parsed.clear();
    resources_.parsed_at_viewport = viewport_;
  }
  resources_.author_sheet_parsed.resize(resources_.author_sheet_slots.size());

  for (std::size_t slot = 0; slot < resources_.author_sheet_slots.size(); ++slot) {
    const std::optional<std::string>& css = resources_.author_sheet_slots[slot];
    if (!css.has_value()) {
      continue;
    }
    std::optional<css::StyleSheet>& cached = resources_.author_sheet_parsed[slot];
    if (!cached.has_value()) {
      // With the viewport, so `@media (min-width: …)` is answered rather than
      // dropped. This is why SetViewport re-parses: the answer is baked in here.
      util::PerformanceTrace::Scope parse("css::ParseStyleSheet");
      cached = css::ParseStyleSheet(*css, viewport_);
    }
    // The faces before the rules, because a face is not a rule: it is kept for
    // the loader rather than added to the cascade.
    resources_.font_faces.insert(resources_.font_faces.end(), cached->font_faces.begin(),
                                 cached->font_faces.end());
    {
      // Copies every rule into the resolver's own entry list, and the resolver
      // is thrown away and rebuilt whenever anything changes -- so this is paid
      // per rebuild, per rule. See TD-0004.
      util::PerformanceTrace::Scope add("css::AddStyleSheet");
      resolver_.AddStyleSheet(*cached, css::Origin::Author);
    }
    CollectKeyframes(*cached);
  }
  // And each shadow root's own sheets, *scoped* to it: a rule inside a component
  // applies within that component and nowhere else, which is the whole of what
  // ADR 0019 §3 asks for. Added after the document's, so document order still
  // decides between two rules of equal specificity in the same tree.
  //
  // Cached positionally against `shadow_sheets`, which is safe for exactly one
  // reason: CollectShadowStyleSheets replaces that whole list when anything in
  // it moved, and drops this one at the same moment. The two are read together
  // and invalidated together.
  resources_.shadow_sheets_parsed.resize(resources_.shadow_sheets.size());
  for (std::size_t i = 0; i < resources_.shadow_sheets.size(); ++i) {
    const auto& [scope, css] = resources_.shadow_sheets[i];
    std::optional<css::StyleSheet>& cached = resources_.shadow_sheets_parsed[i];
    if (!cached.has_value()) {
      util::PerformanceTrace::Scope parse("css::ParseStyleSheet");
      cached = css::ParseStyleSheet(css, viewport_);
    }
    // A component's `@font-face` is *not* scoped: the font database is the
    // document's, which is what the specification says and is why a component can
    // ship a font at all.
    resources_.font_faces.insert(resources_.font_faces.end(), cached->font_faces.begin(),
                                 cached->font_faces.end());
    resolver_.AddStyleSheet(*cached, css::Origin::Author, scope);
    CollectKeyframes(*cached);
  }
  // Background images are queued during `EnsureBoxTree` at layout time, or
  // during `RestyleWithoutLayout` when the box tree already exists -- TD-0005.
  CollectImages();
  if (boxes_ != nullptr) {
    RestyleWithoutLayout();
    layout_.box_tree_cascade_generation = resolver_.Generation();
    layout_.document_version = document_->MutationVersion();
  } else {
    InvalidateBoxTree();
  }
}

void Page::InvalidateBoxTree() {
  boxes_.reset();
  layout_.box_by_element.clear();
  layout_.box_tree_cascade_generation = 0;
  layout_.laid_out_width = -1.0f;
}

void Page::RebuildElementBoxIndex() {
  layout_.box_by_element.clear();
  if (boxes_ == nullptr) {
    return;
  }
  // Preorder: the first box an element generates wins, matching the old
  // `BoxFor` walk. Anonymous and text boxes have no origin and are skipped.
  const auto consider = [this](layout::Box& box) {
    if (const dom::Element* origin = box.Origin()) {
      layout_.box_by_element.emplace(origin, &box);
    }
  };
  consider(*boxes_);
  // ForEachDescendant is const; the map needs mutable Box* for scroll writes.
  std::function<void(layout::Box&)> walk = [&](layout::Box& box) {
    for (std::unique_ptr<layout::Box>& child : box.MutableChildren()) {
      consider(*child);
      walk(*child);
    }
  };
  walk(*boxes_);
}

void Page::EnsureBoxTree() {
  if (document_ == nullptr) {
    InvalidateBoxTree();
    return;
  }
  RefreshDocumentStates();
  const std::uint64_t cascade = resolver_.Generation();
  const std::uint64_t doc_ver = document_->MutationVersion();
  if (boxes_ != nullptr && layout_.document_version == doc_ver &&
      layout_.box_tree_cascade_generation == cascade) {
    AddPerformanceCounter(PerfCounterId::BoxTreeBuildSkipped);
    return;
  }
  const layout::LayoutEngine engine(resolver_, text_ctx_.Measurer(), this);
  util::PerformanceTrace::Scope build("engine::BuildBoxTree");
  boxes_ = engine.BuildBoxTree(*document_);
  layout_.document_version = doc_ver;
  layout_.box_tree_cascade_generation = cascade;
  RebuildElementBoxIndex();
}

void Page::CollectImages() {
  util::PerformanceTrace::Scope collect("engine::CollectImages");
  resources_.pending_images.clear();
  resources_.selected_image_urls.clear();
  resources_.deferred_images.clear();
  if (document_ == nullptr) {
    return;
  }
  // Deduplicated: a page that shows one icon forty times fetches and decodes
  // it once. That matters more for background images than for <img>, since a
  // sprite is by definition the same file behind every icon on the page.
  const auto want = [this](const std::string& src) {
    if (src.empty() || std::find(resources_.pending_images.begin(),
                                 resources_.pending_images.end(),
                                 src) != resources_.pending_images.end()) {
      return;
    }
    // `img-src`, at the one place every image URL in this document passes --
    // an `<img>`'s chosen candidate and a background the cascade named both
    // arrive here, and gating them separately is two chances to miss one.
    if (!policy_.AllowsUrl(csp::Directive::Img, src)) {
      return;
    }
    resources_.pending_images.push_back(src);
  };
  // Which candidate an <img> wants is a question about the viewport as well as
  // about the element, so the answer is recorded here and read again at layout
  // rather than computed twice. A viewport that changes afterwards does not
  // re-select: the bytes for the new candidate were never fetched, so the only
  // thing re-selecting would achieve is an empty box where an image was.
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    std::string selected = SelectImageSource(*image, viewport_);
    if (selected.empty()) {
      continue;
    }
    // `loading="lazy"` holds the URL back rather than dropping it: the element
    // still knows which candidate it wants, so layout draws it the moment the
    // bytes arrive. Already-requested URLs are not re-deferred, or an image
    // fetched a screen ago would go back to being lazy every time a stylesheet
    // landed and re-collected.
    if (ImageLoadingIsLazy(*image) &&
        resources_.requested_images.find(selected) == resources_.requested_images.end()) {
      resources_.deferred_images[image] = selected;
      AddPerformanceCounter(PerfCounterId::EngineImagesDeferred);
    } else {
      want(selected);
    }
    resources_.selected_image_urls[image] = std::move(selected);
  }
  // Background images are named by the *cascade*, not by an attribute, so
  // finding them means resolving style -- which happens in `EnsureBoxTree`
  // during the same pass that builds the box tree. TD-0005.
}

namespace {

// Whether this browser can decode a source with this declared `format()`.
//
// The hint is advisory -- the bytes decide -- but it is what lets an undecodable
// entry be skipped *without fetching it*, which is the only reason authors write
// it. An empty hint is accepted: a bare `url(x.ttf)` is the common spelling and
// refusing it would skip most faces on the web.
//
// `woff2` is accepted since ADR 0024's container landed -- it is what the web
// actually ships, so refusing it refused nearly every face on nearly every page.
// A WOFF2 whose `glyf` is transformed is still refused, but *after* the fetch and
// by the decoder, because that is a fact about the file rather than about its
// declared format. `woff` -- the older zlib container -- is still refused before
// the network, because nothing here unwraps it.
bool CanDecodeFontFormat(std::string_view format) {
  return format.empty() || format == "truetype" || format == "opentype" ||
         format == "truetype-variations" || format == "opentype-variations" ||
         format == "woff2";
}

// Every code point the document's text uses, deduplicated.
//
// This is what makes `unicode-range` worth honouring: Google Fonts serves eight to
// ten `@font-face` blocks per family that differ *only* in this descriptor, and a
// page of English text needs one of them. Fetching all ten is ten TLS-warm requests
// and roughly 200KB for 20KB of useful glyphs.
//
// The bound matters more than the exactness. Past `kMaxDistinct` distinct code
// points this returns nothing and the caller treats every face as needed: a page
// with a thousand distinct characters is a page that plausibly needs every subset,
// and an unbounded set here would be a per-character allocation driven by document
// content.
std::vector<std::uint32_t> TextCodePoints(const dom::Node& root) {
  constexpr std::size_t kMaxDistinct = 1024;
  std::set<std::uint32_t> seen;
  bool overflowed = false;
  const std::function<void(const dom::Node&)> walk = [&](const dom::Node& node) {
    if (overflowed) {
      return;
    }
    // A `<style>` element's contents are CSS and a `<script>`'s are code -- text in
    // the tree that is never drawn. Counting it is not a rounding error: Google's
    // *symbols* subset covers `U+0001-000C`, so the newlines inside a stylesheet
    // matched it and every page with a `@font-face` block fetched an emoji font.
    if (node.IsElement()) {
      const std::string& tag = static_cast<const dom::Element&>(node).TagName();
      if (tag == "style" || tag == "script" || tag == "title" || tag == "template") {
        return;
      }
    }
    if (node.IsText()) {
      const std::string& data = static_cast<const dom::Text&>(node).Data();
      std::size_t at = 0;
      std::uint32_t code = 0;
      while (util::DecodeUtf8(data, at, code)) {
        // A C0 control is never drawn -- a newline is a line break and a tab is
        // white space -- so it must not be what makes a subset look needed. Space
        // itself is kept: it is a glyph with an advance, and it is in the subset
        // every page needs anyway.
        if (code < 0x20u) {
          continue;
        }
        seen.insert(code);
        if (seen.size() > kMaxDistinct) {
          overflowed = true;
          return;
        }
      }
    }
    for (const std::unique_ptr<dom::Node>& child : node.Children()) {
      walk(*child);
    }
  };
  walk(root);
  if (overflowed) {
    return {};
  }
  return std::vector<std::uint32_t>(seen.begin(), seen.end());
}

}  // namespace

std::vector<Page::PendingFontFace> Page::TakeUnrequestedFontFaces() {
  std::vector<PendingFontFace> wanted;
  // Once for the whole list rather than once per face: a page with ten subsets of
  // one family would otherwise walk its own text ten times.
  const std::vector<std::uint32_t> code_points =
      document_ != nullptr ? TextCodePoints(*document_) : std::vector<std::uint32_t>();
  for (const css::FontFace& face : resources_.font_faces) {
    // A face that covers none of the text on the page is not fetched at all. It is
    // still *remembered* as unfetched, so text arriving later -- a script writing
    // Cyrillic into the page -- gets its subset on the next collection.
    if (!code_points.empty() && !face.CoversAnyOf(code_points)) {
      AddPerformanceCounter(PerfCounterId::GfxWebFontsOutOfRange);
      continue;
    }
    for (const css::FontFaceSource& source : face.sources) {
      if (!CanDecodeFontFormat(source.format)) {
        continue;
      }
      // The author's order is a fallback chain, so the first decodable source wins
      // and the rest are not fetched.
      //
      // Keyed by **URL alone**, which is a deliberate change from keying it with the
      // family and weight too: Google serves a variable font by naming the *same*
      // file from a `font-weight: 400` block and a `font-weight: 700` one, and the
      // old key fetched 23KB twice for it. One fetch, and `AddWebFont` registers the
      // bytes under every face that named the file.
      if (resources_.requested_fonts.insert(source.url).second) {
        wanted.push_back(PendingFontFace{source.url, face.family, face.weight, face.italic});
      }
      break;
    }
  }
  return wanted;
}

bool Page::AddWebFont(const PendingFontFace& face, std::vector<std::byte> bytes) {
  // Every face that named this file, not just the one whose fetch it was: a variable
  // font is one file serving several weights, and the fetch was deduplicated by URL.
  // The descriptors are what a `font-family` stack matches against, so a file
  // registered under one weight answers for one weight.
  std::vector<PendingFontFace> registrations;
  for (const css::FontFace& declared : resources_.font_faces) {
    for (const css::FontFaceSource& source : declared.sources) {
      if (source.url != face.url) {
        continue;
      }
      const PendingFontFace entry{face.url, declared.family, declared.weight, declared.italic};
      const bool already = std::any_of(
          registrations.begin(), registrations.end(), [&entry](const PendingFontFace& seen) {
            return seen.family == entry.family && seen.weight == entry.weight &&
                   seen.italic == entry.italic;
          });
      if (!already) {
        registrations.push_back(entry);
      }
      break;
    }
  }
  if (registrations.empty()) {
    registrations.push_back(face);
  }
  bool registered = false;
  for (const PendingFontFace& entry : registrations) {
    // A copy per registration, because the provider takes ownership -- FreeType
    // keeps the buffer for the lifetime of the face it built from it.
    std::vector<std::byte> copy = entry.family == registrations.back().family &&
                                          entry.weight == registrations.back().weight &&
                                          entry.italic == registrations.back().italic
                                      ? std::move(bytes)
                                      : bytes;
    registered = text_ctx_.Text().Fonts().RegisterWebFont(entry.family, entry.weight, entry.italic,
                                               std::move(copy)) ||
                 registered;
  }
  if (!registered) {
    AddPerformanceCounter(PerfCounterId::GfxWebFontsRefused);
    return false;
  }
  AddPerformanceCounter(PerfCounterId::GfxWebFontsRegistered,
                        static_cast<std::uint64_t>(registrations.size()));
  // Text measured before the face arrived was measured in a different font, so
  // every line box on the page is wrong. This is what `font-display: swap` looks
  // like from the inside, and it is the whole reason a face arriving late is not
  // free.
  InvalidateBoxTree();
  return true;
}

std::vector<std::string> Page::TakeUnrequestedImages() {
  std::vector<std::string> wanted;
  for (const std::string& src : resources_.pending_images) {
    if (resources_.requested_images.insert(src).second) {
      wanted.push_back(src);
    }
  }
  return wanted;
}

bool Page::RevealLazyImages() {
  if (resources_.deferred_images.empty() || boxes_ == nullptr) {
    return false;
  }
  // The scrollport, grown by one of itself in every direction. Not a number
  // chosen from a specification -- there is none -- but from what the feature
  // is for: far enough that the bytes are usually there by the time the image
  // is scrolled to, near enough that a feed of two hundred thumbnails fetches
  // a handful rather than all of them.
  const bindings::GeometryRect viewport = QueryViewport();
  const float margin_x = viewport.width;
  const float margin_y = viewport.height;
  bool revealed = false;
  for (auto it = resources_.deferred_images.begin(); it != resources_.deferred_images.end();) {
    const std::optional<bindings::BoxGeometry> box = QueryBox(*it->first);
    // No box is not "far away": a `display: none` image has no position to be
    // near anything, and it stays deferred until something gives it one.
    const bool near =
        box.has_value() && box->border_box.x - margin_x <= viewport.width &&
        box->border_box.x + box->border_box.width + margin_x >= 0.0f &&
        box->border_box.y - margin_y <= viewport.height &&
        box->border_box.y + box->border_box.height + margin_y >= 0.0f;
    if (!near) {
      ++it;
      continue;
    }
    resources_.pending_images.push_back(it->second);
    it = resources_.deferred_images.erase(it);
    revealed = true;
    AddPerformanceCounter(PerfCounterId::EngineImagesRevealed);
  }
  return revealed;
}

gfx::IntSize Page::RequestedImageSize(std::string_view src) const {
  gfx::IntSize size;
  if (document_ == nullptr) {
    return size;
  }
  // The largest request wins, and both axes are taken independently. One
  // resource may be drawn at several sizes, and a vector rasterized at the
  // smallest of them is blurry everywhere else; rasterizing at the largest
  // only ever scales down, which the painter resamples cleanly.
  for (const dom::Element* image : document_->ElementsByTagName("img")) {
    // The selected candidate rather than the `src` attribute: an <img> whose
    // srcset chose a different URL is still the element that says how big the
    // thing at that URL should be drawn.
    const auto selected = resources_.selected_image_urls.find(image);
    if (selected == resources_.selected_image_urls.end() || selected->second != src) {
      continue;
    }
    for (const char* attribute : {"width", "height"}) {
      const std::string* text = image->GetAttribute(attribute);
      if (text == nullptr) {
        continue;
      }
      const std::optional<int> value = util::ParseInt(*text);
      if (!value.has_value() || *value <= 0 || *value > gfx::kMaxSvgEdge) {
        continue;
      }
      int& axis = attribute[0] == 'w' ? size.width : size.height;
      axis = std::max(axis, *value);
    }
  }
  return size;
}

void Page::AddImage(std::string src, std::shared_ptr<const gfx::Image> image) {
  if (image == nullptr || !image->IsValid()) {
    return;
  }
  const std::string key = src;
  resources_.images[std::move(src)] = std::move(image);
  // The box tree sized its replaced boxes against what was available then.
  InvalidateBoxTree();
  DeliverImageLoad(key);
}

void Page::DeliverImageLoad(const std::string& src) {
  if (document_ == nullptr) {
    return;
  }
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() != "img") {
      return;
    }
    const auto selected = resources_.selected_image_urls.find(&element);
    if (selected == resources_.selected_image_urls.end() || selected->second != src) {
      return;
    }
    script_.NotifyElementEvent(element, "load");
  });
}

std::optional<gfx::SurfaceId> Page::SurfaceForElement(const dom::Element& element) const {
  if (element.TagName() != "video") {
    return std::nullopt;
  }
  return video_.SurfaceFor(element);
}

void Page::WantImage(std::string_view src) const {
  if (src.empty()) {
    return;
  }
  Page& self = const_cast<Page&>(*this);
  if (std::find(self.resources_.pending_images.begin(), self.resources_.pending_images.end(),
                src) != self.resources_.pending_images.end()) {
    return;
  }
  if (!policy_.AllowsUrl(csp::Directive::Img, std::string(src))) {
    return;
  }
  self.resources_.pending_images.push_back(std::string(src));
}

std::shared_ptr<const gfx::Image> Page::ImageFor(std::string_view src) const {
  const auto found = resources_.images.find(src);
  return found == resources_.images.end() ? nullptr : found->second;
}

std::shared_ptr<const gfx::Image> Page::ImageForElement(const dom::Element& element, int css_width,
                                                       int css_height) const {
  // A `<canvas>` is its own image source (ADR 0029 §2): the bitmap the page drew, taken through the same
  // hook an `<img>` uses. That is the whole of what canvas cost layout -- a replaced element whose
  // pixels come from somewhere other than the network.
  if (element.TagName() == "canvas") {
    return canvases_.Snapshot(element);
  }
  // Inline `<svg>`: serialize the element and rasterize at the used size. Same
  // decoder an `<img src="….svg">` uses; without this the logo is a 0×0 box.
  if (element.TagName() == "svg") {
    const std::string markup = element.Serialize();
    const auto bytes = std::as_bytes(std::span<const char>(markup.data(), markup.size()));
    gfx::SvgDecodeResult decoded = gfx::DecodeSvg(bytes, css_width, css_height);
    if (!decoded.Ok() || !decoded.image.IsValid()) {
      return nullptr;
    }
    return std::make_shared<gfx::Image>(std::move(decoded.image));
  }
  const auto selected = resources_.selected_image_urls.find(&element);
  if (selected == resources_.selected_image_urls.end()) {
    // An <img> a script created after the images were collected. Nothing was
    // fetched for it, so there is nothing to draw -- which is what the box
    // already looked like, rather than a new kind of failure.
    return nullptr;
  }
  return ImageFor(selected->second);
}

void Page::AddStyleSheet(std::size_t pending_index, std::string_view css) {
  if (pending_index >= resources_.pending_sheet_slots.size()) {
    return;
  }
  const std::size_t slot = resources_.pending_sheet_slots[pending_index];
  if (slot >= resources_.author_sheet_slots.size()) {
    return;
  }
  resources_.author_sheet_slots[slot] = std::string(css);
  // This slot's text has just arrived, so whatever was parsed for it is not it.
  if (slot < resources_.author_sheet_parsed.size()) {
    resources_.author_sheet_parsed[slot].reset();
  }
  RebuildAuthorStyleSheets();
}

}  // namespace microbrowser::engine
