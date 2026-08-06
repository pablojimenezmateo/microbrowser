#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "gfx/FontCatalog.h"

namespace microbrowser::platform {

// The fonts installed on this machine, resolved on demand.
//
// Two phases, and the split is the point. Scanning parses each font file once
// to learn the family, weight and slant it claims, then throws the bytes away —
// a desktop has hundreds of font files and holding them all would cost hundreds
// of megabytes to answer questions about a handful. Loading happens on the
// first request that actually selects a face.
//
// Lives in platform rather than gfx because it reads the filesystem, and gfx is
// the module that does not know what an operating system is. gfx defines the
// seam (gfx::FontProvider); this is the implementation that knows where Linux
// keeps its fonts.
class SystemFontProvider : public gfx::FontProvider {
 public:
  explicit SystemFontProvider(gfx::FontLibrary& library) : catalog_(library) {}

  // Indexes every font file under the standard directories. Returns how many
  // faces were indexed. Safe to call more than once; already-indexed paths are
  // skipped.
  std::size_t Scan();

  // Indexes one directory tree. Exposed so a test can point at a fixture
  // directory instead of at whatever the machine happens to have installed.
  std::size_t ScanDirectory(const std::filesystem::path& directory);

  // The family a request falls back to. Set to the first sans-serif-looking
  // family found unless a caller overrides it: a page that names a family this
  // machine does not have must still render text.
  void SetDefaultFamily(std::string family);
  const std::string& DefaultFamily() const { return default_family_; }

  gfx::Font* FontFor(const gfx::FontRequest& request) override;

  // A face on this machine that covers `code_point`.
  //
  // **The bug this exists for:** a page asking for `sans-serif` got DejaVu Sans, which has no CJK
  // glyphs, so a Japanese paragraph rendered as boxes on a machine with 31 Japanese faces installed.
  // This is where the machine's faces are, so this is where the fallback has to live.
  //
  // It loads candidates *lazily and in index order*, stopping at the first that covers the code
  // point, and remembers the answer per code point block -- because a page of Japanese asks this
  // question once per character and the answer is the same face every time. Without the cache this
  // would be a face load per character.
  gfx::Font* FontForCodePoint(const gfx::FontRequest& request, char32_t code_point) override;

  // A page's own `@font-face`, into the same catalog the system fonts land in.
  //
  // Forwarded rather than inherited-by-default, and the bug it fixes is worth the
  // sentence: `FontProvider::RegisterWebFont` returns false so that a provider with
  // no way to load bytes refuses honestly, and this class is the provider every
  // real binary uses. Without this override, `@font-face` worked in the tests --
  // which build a `FontCatalog` directly -- and in nothing else.
  bool RegisterWebFont(std::string family, int weight, bool italic,
                       std::vector<std::byte> bytes) override {
    return catalog_.RegisterWebFont(std::move(family), weight, italic, std::move(bytes));
  }

  std::size_t IndexedFaces() const { return index_.size(); }
  std::size_t LoadedFaces() const { return catalog_.FaceCount(); }

 private:
  struct Indexed {
    std::filesystem::path path;
    std::string family;  // as the face reports it
    int weight = 400;
    bool italic = false;
    bool loaded = false;
  };

  // Index of the best unloaded match, or npos.
  std::size_t BestUnloaded(const gfx::FontRequest& request, int& distance_out) const;
  bool Load(Indexed& entry);

  // What a request resolves to, remembered.
  //
  // `FontFor` is three full scans -- every *file* on the machine to find a
  // better unloaded candidate, every loaded face to find what it would beat,
  // and then the catalog's own match -- and layout asks it for the width, the
  // line height and the ascent of every text run. On en.wikipedia.org/wiki/CSS
  // that was 985,000 calls, and 227 of that page's 259 seconds.
  //
  // The three scans exist to answer "is there a file worth paging in for this
  // request", which is a question about the *request*, not about how many times
  // it is asked. A page uses a handful of stacks, so this stays small.
  //
  // Size is part of the key here and not in the catalog's: this hands back a
  // `Font`, which is a face at a size.
  struct ResolvedKey {
    std::vector<std::string> families;
    std::uint32_t size_bits = 0;
    int weight = 400;
    bool italic = false;

    friend bool operator<(const ResolvedKey& a, const ResolvedKey& b) {
      if (a.size_bits != b.size_bits) {
        return a.size_bits < b.size_bits;
      }
      if (a.weight != b.weight) {
        return a.weight < b.weight;
      }
      if (a.italic != b.italic) {
        return static_cast<int>(a.italic) < static_cast<int>(b.italic);
      }
      return a.families < b.families;
    }
  };

  gfx::FontCatalog catalog_;
  std::vector<Indexed> index_;
  std::string default_family_;
  // Dropped whenever a face is loaded, because a newly loaded file is exactly
  // the thing that can beat an answer already given. That is the only event
  // that can change one: the index is built once, and `Load` is the single
  // place a face enters the catalog from it.
  std::map<ResolvedKey, gfx::Font*> resolved_;
  // Which family covered a code point, keyed by its 256-code-point block. A page of Japanese asks
  // once per character and the answer is the same face every time, so this turns thousands of
  // coverage probes into one per block -- and a block is the right granularity because scripts are
  // laid out in contiguous ranges.
  //
  // An empty string means "nothing on this machine covers it", which is worth caching too: the
  // alternative is re-probing every installed face for every character of an undrawable run.
  std::map<std::uint32_t, std::string> coverage_;
};

// Reads a whole file. Empty on any failure, including a file too large to be a
// font: a font file arrives from the filesystem rather than the network here,
// but a bound that only holds for trusted input is a bound that will be wrong
// the first time @font-face reuses this.
std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path,
                                     std::size_t max_bytes = 64u * 1024u * 1024u);

}  // namespace microbrowser::platform
