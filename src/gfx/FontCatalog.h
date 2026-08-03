#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gfx/Font.h"

namespace microbrowser::gfx {

// Answers "which face draws this request".
//
// An interface because font selection is policy and painting is mechanism, and
// the two change for different reasons: a test wants one face and no
// filesystem, a browser wants a system database, a web page wants @font-face
// downloads layered on top. All three are the same seam.
class FontProvider {
 public:
  virtual ~FontProvider() = default;

  FontProvider(const FontProvider&) = delete;
  FontProvider& operator=(const FontProvider&) = delete;

  // Null when nothing matches. A text run with no font paints nothing, which
  // is a legitimate outcome — a page can name a family that does not exist —
  // and must not be a crash or a fallback chosen silently three layers down.
  //
  // The returned Font is owned by the provider and is valid until the provider
  // is destroyed or a new face is registered.
  virtual Font* FontFor(const FontRequest& request) = 0;

 protected:
  FontProvider() = default;
};

// A provider over faces handed to it as bytes.
//
// Deliberately does no I/O. Loading a font file means touching the filesystem,
// and gfx is the module that does not know what an operating system is — see
// its MODULE.deps. platform::LoadSystemFonts fills one of these.
//
// Matching is nearest-weight within a family, because that is what a page
// means by `font-weight: 500` when the family ships 400 and 700 and nothing
// between. Synthetic bolding and obliquing are deliberately absent: they are a
// rendering decision that needs a measurement to justify, and a wrong-weight
// real face beats a smeared fake one.
class FontCatalog : public FontProvider {
 public:
  explicit FontCatalog(FontLibrary& library) : library_(&library) {}

  // False for bytes FreeType will not parse. Font files are attacker
  // controlled once @font-face exists, so this is a routine outcome.
  bool Register(std::string family, int weight, bool italic, std::vector<std::byte> bytes);

  // Registers a face under the family, weight and slant it reports about
  // itself. This is the form a font database wants: a filename says "Bold"
  // only by convention.
  bool Register(std::vector<std::byte> bytes);

  // Points a generic family at a real one. CSS names five generics -- serif,
  // sans-serif, monospace, cursive, fantasy -- and none of them is a font. A
  // page asking for `monospace` and getting the default sans is not a
  // near-miss; it is code samples in a proportional face.
  //
  // A table rather than a hardcoded list because which real family answers
  // `monospace` is a property of the machine, and gfx does not know what
  // machine it is on.
  void SetGenericFamily(std::string generic, std::string family);

  // The family used for an empty or unmatched request. Set once, at startup,
  // by whoever built the catalog: a request that matches nothing must still
  // render, or a page with one bad family name renders no text at all.
  void SetDefaultFamily(std::string family) { default_family_ = NormalizeFamily(family); }

  Font* FontFor(const FontRequest& request) override;

  std::size_t FaceCount() const { return faces_.size(); }

  // The library these faces are loaded from. Exposed so a font database that
  // owns a catalog can parse a candidate file's metadata without loading it
  // into the catalog, rather than carrying a second FontLibrary around.
  FontLibrary& Library() const { return *library_; }

  // How well a face answers a request, given that its family is the
  // `family_rank`-th entry of FamilyCandidates() — negative when it is not on
  // that list at all. Lower is better.
  //
  // A tier per criterion, each wider than everything below it, so the ordering
  // is family position, then slant, then weight: a page that asked for Verdana
  // and got its second choice in exactly the right weight has still been given
  // the wrong font. A single weighted score is precisely how that ordering gets
  // lost.
  //
  // Public and static because font *selection* and font *loading* are in
  // different modules — platform's database has to rank files it has not
  // loaded — and two implementations of this ordering would drift.
  static int MatchDistance(int weight, bool italic, const FontRequest& request, int family_rank);

  // Where `family` sits on `candidates`, or -1. Normalizes, so a caller holding
  // a family name as the face reports it does not have to.
  static int FamilyRank(const std::vector<std::string>& candidates, std::string_view family);

  // The distance of the face this catalog would pick today, or kNoMatch when it
  // has nothing. A loader compares a candidate against this to decide whether
  // reading the file is worth it.
  static constexpr int kNoMatch = 1 << 30;
  int BestLoadedDistance(const FontRequest& request) const;

  // The families to try, best first: every family the request named with
  // generics resolved and duplicates dropped, then the default.
  //
  // Public, and the single source of truth for the question, because the font
  // database in platform ranks files it has not loaded against the same list.
  // Two implementations of "which family did we settle on" would drift, and the
  // symptom would be a file paged in that the matcher then declines to use.
  std::vector<std::string> FamilyCandidates(const FontRequest& request) const;

  // The family one name actually means: a generic resolved, an empty name
  // replaced by the default.
  std::string ResolveFamily(std::string_view requested) const;

  // Case-insensitive, whitespace-trimmed. `Font-Family: DejaVu Sans` and
  // `dejavu sans` name the same family, and CSS says so.
  static std::string NormalizeFamily(std::string_view family);

 private:
  struct Face {
    std::string family;  // normalized
    int weight = 400;
    bool italic = false;
    FontFace face;
  };

  const Face* Match(const FontRequest& request) const;

  FontLibrary* library_;
  // unique_ptr because a Font holds a FontFace*, so a face's address must
  // outlive every Font resolved from it — a vector that reallocates would
  // leave every cached Font pointing at freed memory.
  std::vector<std::unique_ptr<Face>> faces_;
  std::string default_family_;
  std::map<std::string, std::string> generics_;
  // Keyed on the face and the size's exact bits: two requests for 15.5px must
  // share a Font, and 15.5 and 15.4 must not.
  std::map<std::pair<const Face*, std::uint32_t>, Font> sized_;
};

}  // namespace microbrowser::gfx
