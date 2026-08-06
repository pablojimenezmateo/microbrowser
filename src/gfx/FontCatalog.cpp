#include "gfx/FontCatalog.h"

#include "gfx/SfntContainer.h"
#include "gfx/Woff2.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char Lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

std::string FontCatalog::NormalizeFamily(std::string_view family) {
  // Trimmed at both ends, and internal runs collapsed to one space: an unquoted
  // CSS family name is a sequence of identifiers, so `Helvetica Neue` and
  // `Helvetica  Neue` are the same name and a stylesheet may write either.
  // Comparing them as raw text is a silent no-match that renders as the wrong
  // font rather than as an error.
  std::string out;
  out.reserve(family.size());
  bool pending_space = false;
  for (const char c : family) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(Lower(c));
  }
  return out;
}

bool FontCatalog::Register(std::string family, int weight, bool italic,
                           std::vector<std::byte> bytes) {
  std::optional<FontFace> face = FontFace::Load(*library_, std::move(bytes));
  if (!face.has_value()) {
    return false;
  }
  // The sized Fonts already handed out stay valid, and must: they are keyed on
  // Face addresses, which unique_ptr keeps stable, and callers cache things
  // against them. Clearing here would leave every such cache holding a pointer
  // to a destroyed Font, which the allocator then hands back for a *different*
  // face -- a stale hit that renders one font's text with another's metrics.
  auto entry = std::make_unique<Face>();
  entry->family = NormalizeFamily(family);
  entry->weight = weight;
  entry->italic = italic;
  entry->face = std::move(*face);
  if (default_family_.empty()) {
    default_family_ = entry->family;
  }
  faces_.push_back(std::move(entry));
  AddPerformanceCounter(PerfCounterId::FontFacesRegistered);
  return true;
}

bool FontCatalog::Register(std::vector<std::byte> bytes) {
  std::optional<FontFace> probe = FontFace::Load(*library_, bytes);
  if (!probe.has_value()) {
    return false;
  }
  return Register(probe->FamilyName(), probe->Weight(), probe->IsItalic(), std::move(bytes));
}

void FontCatalog::SetGenericFamily(std::string generic, std::string family) {
  generics_[NormalizeFamily(generic)] = NormalizeFamily(family);
}

std::string FontCatalog::ResolveFamily(std::string_view requested) const {
  const std::string normalized = NormalizeFamily(requested);
  if (const auto alias = generics_.find(normalized); alias != generics_.end()) {
    return alias->second;
  }
  return normalized.empty() ? default_family_ : normalized;
}

std::vector<std::string> FontCatalog::FamilyCandidates(const FontRequest& request) const {
  std::vector<std::string> candidates;
  candidates.reserve(request.families.size() + 1);
  const auto add = [&candidates](std::string family) {
    if (family.empty() || candidates.size() >= kMaxFontFamilies ||
        std::find(candidates.begin(), candidates.end(), family) != candidates.end()) {
      return;
    }
    candidates.push_back(std::move(family));
  };
  for (const std::string& family : request.families) {
    add(ResolveFamily(family));
  }
  // Always last, never first: the default is what a page gets when it named
  // nothing this machine has, and a page that named nothing at all resolves to
  // it through ResolveFamily's empty case anyway.
  add(default_family_);
  return candidates;
}

int FontCatalog::FamilyRank(const std::vector<std::string>& candidates, std::string_view family) {
  const std::string normalized = NormalizeFamily(family);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i] == normalized) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FontCatalog::MatchDistance(int weight, bool italic, const FontRequest& request,
                               int family_rank) {
  if (family_rank < 0) {
    return kNoMatch;
  }
  // The family tier times the longest list anything will build stays well under
  // kNoMatch, so "on the list, badly" can never be confused with "not on it".
  static_assert(static_cast<int>(kMaxFontFamilies) * 1000000 < kNoMatch);
  int distance = std::min(family_rank, static_cast<int>(kMaxFontFamilies)) * 1000000;
  distance += italic == request.italic ? 0 : 10000;
  distance += std::min(std::abs(weight - request.weight), 9999);
  return distance;
}

int FontCatalog::BestLoadedDistance(const FontRequest& request) const {
  const std::vector<std::string> candidates = FamilyCandidates(request);
  int best = kNoMatch;
  for (const std::unique_ptr<Face>& candidate : faces_) {
    best = std::min(best, MatchDistance(candidate->weight, candidate->italic, request,
                                        FamilyRank(candidates, candidate->family)));
  }
  return best;
}

const FontCatalog::Face* FontCatalog::Match(const FontRequest& request) const {
  // One pass over every loaded face, ranked by where its family sits on the
  // page's list. The list already ends in the default, so falling back is not a
  // second round with different rules -- it is the last entry losing to nothing
  // better, which is why a face from the page's second choice can never be
  // beaten by a nearer weight in its third.
  const std::vector<std::string> candidates = FamilyCandidates(request);
  const Face* best = nullptr;
  int best_distance = kNoMatch;
  for (const std::unique_ptr<Face>& candidate : faces_) {
    const int distance = MatchDistance(candidate->weight, candidate->italic, request,
                                       FamilyRank(candidates, candidate->family));
    if (distance < best_distance) {
      best = candidate.get();
      best_distance = distance;
    }
  }
  if (best != nullptr) {
    return best;
  }
  // Nothing on the list is loaded and nothing answers the default either --
  // which happens before any face has been registered under a recognised name.
  // Some text beats none.
  return faces_.empty() ? nullptr : faces_.front().get();
}

Font* FontCatalog::FontFor(const FontRequest& request) {
  if (!(request.size > 0.0f) || !std::isfinite(request.size)) {
    // A size arriving from a stylesheet or an IPC frame can be anything.
    return nullptr;
  }
  const Face* face = Match(request);
  if (face == nullptr) {
    return nullptr;
  }

  const auto key = std::make_pair(face, std::bit_cast<std::uint32_t>(request.size));
  const auto existing = sized_.find(key);
  if (existing != sized_.end()) {
    AddPerformanceCounter(PerfCounterId::FontLookupHits);
    return &existing->second;
  }
  AddPerformanceCounter(PerfCounterId::FontLookupMisses);
  // const_cast because Font needs a mutable face — FreeType carries the active
  // size inside FT_Face, so setting a size mutates it. The catalog owns the
  // face; Match returns const only because matching does not modify it.
  FontFace& mutable_face = const_cast<Face*>(face)->face;
  const auto inserted = sized_.emplace(key, Font(mutable_face, request.size));
  return &inserted.first->second;
}

bool FontCatalog::CoversCodePoint(const FontRequest& request, char32_t code_point) const {
  const Face* matched = Match(request);
  return matched != nullptr && matched->face.GlyphForCodepoint(code_point) != 0;
}

Font* FontCatalog::FontForCodePoint(const FontRequest& request, char32_t code_point) {
  // The requested family first: a font stack is what the author asked for, and falling back before
  // checking it would ignore them. Asked of the *face* rather than of the `Font`, because coverage is
  // a property of the face and a `Font` is a face at a size.
  if (Font* preferred = FontFor(request); preferred != nullptr) {
    if (CoversCodePoint(request, code_point)) {
      return preferred;
    }
    // Every registered face, in registration order. A catalog holds a handful -- a test's fixtures, a
    // page's `@font-face` set -- so this is a short walk and not an index lookup. `SystemFontProvider`
    // is where the machine's thousands of faces are, and it overrides this for that reason.
    for (const std::unique_ptr<Face>& candidate : faces_) {
      if (candidate->face.GlyphForCodepoint(code_point) == 0) {
        continue;
      }
      FontRequest fallback = request;
      fallback.families = {candidate->family};
      if (Font* found = FontFor(fallback); found != nullptr) {
        AddPerformanceCounter(PerfCounterId::FontFallbacks);
        return found;
      }
    }
    // Nothing covers it. The preferred face is returned rather than null: it draws `.notdef`, which is
    // a visible box, and a box is the honest glyph for a code point this machine cannot draw. Null
    // would drop the character silently, which is worse -- the reader cannot tell text is missing.
    return preferred;
  }
  return nullptr;
}

bool FontCatalog::RegisterWebFont(std::string family, int weight, bool italic,
                                  std::vector<std::byte> bytes) {
  // A downloaded font is the one attacker-controlled input this browser hands to a
  // large C library, so its container is checked here rather than trusted to
  // FreeType's own reads -- ADR 0024 §3. The other half of that section holds by
  // construction: nothing in this browser asks for `Hinting::Normal`, so the
  // TrueType bytecode interpreter -- the most exploited part of FreeType -- never
  // runs. If hinting is ever turned on for system fonts, a face that arrived over
  // the network must be exempted here.
  if (IsWoff2(bytes)) {
    // Unwrapped here rather than at the caller, because "which container did this
    // face arrive in" is a question about bytes and this is the class that already
    // takes bytes. A WOFF2 this decoder refuses -- a transformed `glyf`, today --
    // is a face that does not register, and the page renders in the next family of
    // its stack.
    const std::optional<Woff2Font> unwrapped = DecodeWoff2(bytes);
    if (!unwrapped.has_value()) {
      return false;
    }
    // The reassembled sfnt is checked too. It is this decoder's own output, so a
    // failure here is a bug in it rather than a hostile file -- which is exactly why
    // it is worth checking: the alternative to finding it here is FreeType finding
    // it, or not.
    if (!SfntContainerIsSane(unwrapped->sfnt)) {
      return false;
    }
    return Register(std::move(family), weight, italic, unwrapped->sfnt);
  }
  if (!SfntContainerIsSane(bytes)) {
    return false;
  }
  return Register(std::move(family), weight, italic, std::move(bytes));
}

}  // namespace microbrowser::gfx
