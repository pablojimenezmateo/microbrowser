#include "platform/SystemFonts.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <optional>
#include <system_error>
#include <utility>

#include "util/Env.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::platform {

namespace {

// Where a Linux desktop keeps fonts. Ordered so that a user's own fonts are
// found before the system's, which is what a user installing a font expects.
const char* const kSystemFontDirectories[] = {
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "/usr/share/texmf/fonts/opentype",
};

bool IsFontFile(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Bitmap-only formats (.pcf, .bdf) are deliberately absent: they have no
  // outlines, and this rasterizer draws outlines.
  return extension == ".ttf" || extension == ".otf" || extension == ".ttc" ||
         extension == ".otc";
}

// What each CSS generic family should actually be, most-wanted first. A
// generic chosen at random from whatever sorted first would make text rendering
// depend on which fonts happen to be installed in which order, and would render
// code samples in a proportional face about half the time.
struct GenericFamily {
  const char* generic;
  std::array<const char*, 6> candidates;
};

constexpr GenericFamily kGenericFamilies[] = {
    {"sans-serif", {"DejaVu Sans", "Liberation Sans", "Noto Sans", "FreeSans", "Ubuntu", "Arial"}},
    {"serif",
     {"DejaVu Serif", "Liberation Serif", "Noto Serif", "FreeSerif", "Times New Roman", nullptr}},
    {"monospace",
     {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "FreeMono", "Courier New",
      "Ubuntu Mono"}},
    // Neither has a plausible answer on a Linux desktop, and picking a random
    // display face is worse than being honest about it.
    {"cursive", {"DejaVu Sans", "Liberation Sans", "Noto Sans", nullptr, nullptr, nullptr}},
    {"fantasy", {"DejaVu Sans", "Liberation Sans", "Noto Sans", nullptr, nullptr, nullptr}},
};

}  // namespace

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path, std::size_t max_bytes) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > max_bytes) {
    return {};
  }

  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    // A short read means the file changed underneath us. A truncated font is
    // not a font.
    return {};
  }
  return bytes;
}

std::size_t SystemFontProvider::ScanDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) {
    return 0;
  }

  std::size_t added = 0;
  const std::filesystem::recursive_directory_iterator end;
  // Manual advance with an error_code: a symlink loop or an unreadable
  // subdirectory must skip that entry rather than throw out of a scan.
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      break;
    }
    const std::filesystem::directory_entry& entry = *iterator;
    if (!entry.is_regular_file(error) || error || !IsFontFile(entry.path())) {
      error.clear();
      continue;
    }
    if (std::any_of(index_.begin(), index_.end(),
                    [&entry](const Indexed& known) { return known.path == entry.path(); })) {
      continue;
    }

    // Parsed for its metadata and then dropped. This is the whole reason the
    // index and the catalog are separate: keeping every face alive would cost
    // a few hundred megabytes to answer a question about six of them.
    std::vector<std::byte> bytes = ReadFileBytes(entry.path());
    if (bytes.empty()) {
      continue;
    }
    std::optional<gfx::FontFace> face = gfx::FontFace::Load(catalog_.Library(), bytes);
    if (!face.has_value()) {
      continue;
    }
    std::string family = face->FamilyName();
    if (family.empty()) {
      continue;
    }
    index_.push_back(Indexed{entry.path(), std::move(family), face->Weight(), face->IsItalic(),
                             false});
    ++added;
  }
  return added;
}

std::size_t SystemFontProvider::Scan() {
  std::size_t added = 0;
  if (const char* home = util::EnvValue("HOME"); home != nullptr) {
    added += ScanDirectory(std::filesystem::path(home) / ".local/share/fonts");
    added += ScanDirectory(std::filesystem::path(home) / ".fonts");
  }
  for (const char* directory : kSystemFontDirectories) {
    added += ScanDirectory(directory);
  }

  const auto installed = [this](const char* family) {
    return family != nullptr &&
           std::any_of(index_.begin(), index_.end(),
                       [family](const Indexed& e) { return e.family == family; });
  };

  for (const GenericFamily& generic : kGenericFamilies) {
    for (const char* candidate : generic.candidates) {
      if (installed(candidate)) {
        catalog_.SetGenericFamily(generic.generic, candidate);
        if (default_family_.empty() && std::string_view(generic.generic) == "sans-serif") {
          SetDefaultFamily(candidate);
        }
        break;
      }
    }
  }
  if (default_family_.empty() && !index_.empty()) {
    // Nothing recognized. Some text is better than none, and the first indexed
    // family is at least a real face.
    SetDefaultFamily(index_.front().family);
  }
  return added;
}

void SystemFontProvider::SetDefaultFamily(std::string family) {
  default_family_ = std::move(family);
  catalog_.SetDefaultFamily(default_family_);
}

std::size_t SystemFontProvider::BestUnloaded(const gfx::FontRequest& request,
                                             int& distance_out) const {
  std::size_t best = std::string::npos;
  distance_out = gfx::FontCatalog::kNoMatch;
  // Only families on the page's list, ranked the way the catalog ranks them --
  // which includes the default, as its last entry. That last entry is what
  // makes text appear at all on a page whose whole font stack names families
  // this machine does not have: without it nothing is ever loaded, the catalog
  // has no face to fall back *to*, and every run paints nothing.
  //
  // Anything off the list stays on disk. A loader that fell back further would
  // page in a file from an unrelated family every time a page named one this
  // machine does not have, and the catalog would then decline to use it.
  const std::vector<std::string> candidates = catalog_.FamilyCandidates(request);
  for (std::size_t i = 0; i < index_.size(); ++i) {
    if (index_[i].loaded) {
      continue;
    }
    const int distance = gfx::FontCatalog::MatchDistance(
        index_[i].weight, index_[i].italic, request,
        gfx::FontCatalog::FamilyRank(candidates, index_[i].family));
    if (distance < distance_out) {
      best = i;
      distance_out = distance;
    }
  }
  return best;
}

bool SystemFontProvider::Load(Indexed& entry) {
  entry.loaded = true;  // set first: a file that fails to parse must not be retried every frame
  // A newly loaded face can beat an answer already given, and this is the only
  // event that can. Dropped before the load rather than after so that a
  // registration failing part-way cannot leave a stale entry behind.
  resolved_.clear();
  std::vector<std::byte> bytes = ReadFileBytes(entry.path);
  if (bytes.empty()) {
    return false;
  }
  return catalog_.Register(entry.family, entry.weight, entry.italic, std::move(bytes));
}


bool SystemFontProvider::RegisterWebFont(std::string family, int weight, bool italic,
                                         std::vector<std::byte> bytes) {
  // `resolved_` is dropped here for the same reason `Load` drops it, and the
  // comment there -- "a newly loaded face can beat an answer already given, and
  // this is the only event that can" -- stopped being true the day `@font-face`
  // landed. A face arriving from the network is the other event.
  //
  // What it cost to forget is not a stale measurement: it is a web font that
  // never applies at all. A page laid out before its face arrives asks for
  // `chws-font`, gets the fallback, and the answer is remembered; the relayout
  // the arriving font triggers then asks the same question and is handed the
  // same stale answer. Whether a page's own font takes effect was therefore a
  // race between the fetch and the first layout -- which is what made
  // `css/css-text/text-spacing-trim/` render one of two pictures across runs of
  // the same binary, and seven of the eight reftests task F10 had to name.
  //
  // Before the registration rather than after, so a `Register` that fails
  // part-way cannot leave behind an answer computed with a face that is not
  // there.
  resolved_.clear();
  return catalog_.RegisterWebFont(std::move(family), weight, italic, std::move(bytes));
}

gfx::Font* SystemFontProvider::FontForCodePoint(const gfx::FontRequest& request,
                                                char32_t code_point) {
  gfx::Font* preferred = FontFor(request);
  // The requested stack first: it is what the author asked for, and a fallback that ran before
  // checking it would ignore them. This is also the overwhelmingly common path -- Latin text in a
  // Latin font -- and it costs one glyph-index lookup.
  if (preferred != nullptr && catalog_.CoversCodePoint(request, code_point)) {
    return preferred;
  }

  // Remembered per 256-code-point block. A page of Japanese asks this question once per character and
  // the answer is the same face every time, so without the cache this is a face load per character.
  // A block is the right granularity because scripts occupy contiguous ranges.
  const std::uint32_t block = static_cast<std::uint32_t>(code_point) >> 8;
  if (const auto remembered = coverage_.find(block); remembered != coverage_.end()) {
    if (remembered->second.empty()) {
      // Nothing on this machine covers this block, and that is worth caching: the alternative is
      // re-probing every installed face for every character of an undrawable run.
      return preferred;
    }
    gfx::FontRequest cached = request;
    cached.families = {remembered->second};
    if (gfx::Font* found = FontFor(cached); found != nullptr) {
      return found;
    }
  }

  // Load candidates in index order until one covers it. Lazily, because the index holds every face on
  // the machine and loading them all to answer one question is how a browser takes a second to draw a
  // paragraph -- and the block cache means this runs once per script rather than once per character.
  for (Indexed& entry : index_) {
    if (!entry.loaded && !Load(entry)) {
      continue;
    }
    gfx::FontRequest candidate = request;
    candidate.families = {entry.family};
    if (!catalog_.CoversCodePoint(candidate, code_point)) {
      continue;
    }
    coverage_[block] = entry.family;
    util::AddPerformanceCounter(util::PerfCounterId::FontFallbacks);
    return FontFor(candidate);
  }
  coverage_[block] = std::string();
  return preferred;
}

gfx::Font* SystemFontProvider::FontFor(const gfx::FontRequest& request) {
  // Asked once per distinct request rather than once per question about it.
  // See `resolved_`: everything below this is three passes over the machine's
  // fonts, and the answer depends on the request rather than on the asking.
  const ResolvedKey key{request.families, std::bit_cast<std::uint32_t>(request.size),
                        request.weight, request.italic};
  if (const auto cached = resolved_.find(key); cached != resolved_.end()) {
    util::AddPerformanceCounter(util::PerfCounterId::FontResolveCacheHits);
    return cached->second;
  }
  util::AddPerformanceCounter(util::PerfCounterId::FontResolveCacheMisses);

  // Load a file only when it would answer this request *better* than anything
  // already loaded. That is what keeps a page using six faces from paging in
  // every font on the machine, and what still lets the bold face arrive when
  // the regular one is already in.
  int candidate_distance = gfx::FontCatalog::kNoMatch;
  const std::size_t candidate = BestUnloaded(request, candidate_distance);
  if (candidate != std::string::npos && candidate_distance < catalog_.BestLoadedDistance(request)) {
    Load(index_[candidate]);
  }
  gfx::Font* resolved = catalog_.FontFor(request);
  // After the load, so the entry records what this request settles on with that
  // file in rather than without it.
  resolved_.emplace(key, resolved);
  return resolved;
}

}  // namespace microbrowser::platform
