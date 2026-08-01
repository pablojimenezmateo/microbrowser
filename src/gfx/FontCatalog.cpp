#include "gfx/FontCatalog.h"

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
  std::size_t begin = 0;
  std::size_t end = family.size();
  while (begin < end && (family[begin] == ' ' || family[begin] == '\t')) {
    ++begin;
  }
  while (end > begin && (family[end - 1] == ' ' || family[end - 1] == '\t')) {
    --end;
  }
  std::string out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    out.push_back(Lower(family[i]));
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

int FontCatalog::MatchDistance(std::string_view family, int weight, bool italic,
                               const FontRequest& request, std::string_view wanted_family) {
  const std::string candidate = NormalizeFamily(family);
  const std::string wanted = NormalizeFamily(wanted_family);
  // A tier per criterion, each wider than everything below it, so the ordering
  // is family, then slant, then weight, and no amount of weight agreement can
  // outrank the right family. A single weighted score is exactly how that
  // ordering gets lost.
  int distance = 0;
  if (candidate != wanted) {
    distance += 1000000;
  }
  distance += italic == request.italic ? 0 : 10000;
  distance += std::min(std::abs(weight - request.weight), 9999);
  return distance;
}

int FontCatalog::BestLoadedDistance(const FontRequest& request) const {
  const std::string wanted = ResolveFamily(request.family);
  int best = kNoMatch;
  for (const std::unique_ptr<Face>& candidate : faces_) {
    best = std::min(best, MatchDistance(candidate->family, candidate->weight, candidate->italic,
                                        request, wanted));
  }
  return best;
}

const FontCatalog::Face* FontCatalog::Match(const FontRequest& request) const {
  // Two rounds: the family the page asked for, then the default. A face from
  // the wrong family is never a better answer than any face from the right one,
  // and falling back is a separate decision from ranking within a family.
  const std::string wanted = ResolveFamily(request.family);
  for (const std::string& family : {wanted, default_family_}) {
    const Face* best = nullptr;
    int best_distance = kNoMatch;
    for (const std::unique_ptr<Face>& candidate : faces_) {
      if (NormalizeFamily(candidate->family) != family) {
        continue;
      }
      const int distance =
          MatchDistance(candidate->family, candidate->weight, candidate->italic, request, family);
      if (best == nullptr || distance < best_distance) {
        best = candidate.get();
        best_distance = distance;
      }
    }
    if (best != nullptr) {
      return best;
    }
  }
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

}  // namespace microbrowser::gfx
