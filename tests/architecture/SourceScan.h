#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "architecture/ModuleManifest.h"

namespace microbrowser::tests::architecture {

// One source file, addressed the way a violation message should read.
struct SourceFile {
  std::string path;  // repo-relative, e.g. "src/gfx/Canvas.cpp"
  std::string text;

  bool IsHeader() const { return path.size() > 2 && path.compare(path.size() - 2, 2, ".h") == 0; }
};

using SourceSet = std::vector<SourceFile>;

struct Violation {
  std::string file;
  int line = 0;
  std::string message;
};

// Every rule has this shape. That is what makes control fixtures possible: a
// rule can be run against a hand-written in-memory SourceSet that is *known* to
// violate it, proving the rule can actually fail.
//
// A lint that has never been shown to fail is not a lint. microide shipped a
// close-on-exec rule whose pattern silently matched nothing for months; it
// passed green the entire time. Every rule here carries both a positive and a
// negative fixture for exactly that reason.
struct Rule {
  std::string name;
  std::vector<Violation> (*check)(const SourceSet&, const ModuleManifests&);
};

// --- Loading -----------------------------------------------------------------

SourceSet LoadSourceTree(const std::filesystem::path& repo_root);
ModuleManifests LoadModuleManifests(const std::filesystem::path& repo_root,
                                    std::vector<Violation>& errors);

// --- Path helpers ------------------------------------------------------------

// "src/gfx/Canvas.cpp" -> "gfx". Empty for a path not under src/<module>/.
std::string ModuleOf(std::string_view repo_relative_path);

// "src/gfx/Canvas.cpp" -> "Canvas.cpp".
std::string FileNameOf(std::string_view repo_relative_path);

// --- Text helpers ------------------------------------------------------------

// Replaces the contents of comments and string/char literals with spaces,
// preserving every byte offset and newline. Rules that match code patterns run
// on the masked text, so a pattern named in a comment is not a violation —
// which is otherwise the most common source of false positives in a lint like
// this.
std::string MaskCommentsAndStrings(std::string_view text);

struct IncludeDirective {
  int line = 0;
  std::string target;   // the text between the delimiters
  bool angled = false;  // <...> rather than "..."
};

std::vector<IncludeDirective> ExtractIncludes(std::string_view text);

// Maps an angled include to the sanctioned third-party group it belongs to
// (per docs/adr/0001-third-party-dependencies.md). Empty for standard-library
// and POSIX headers, which are never restricted.
std::string ExternGroupFor(std::string_view include_target);

// --- Class scanning ----------------------------------------------------------

struct ClassInfo {
  std::string name;
  int start_line = 0;
  std::size_t header_lines = 0;    // opening line through closing brace, inclusive
  std::size_t public_methods = 0;  // declarations with a parameter list in a public section
  std::size_t members = 0;         // non-static data members, all access levels
  // Distinct project modules named by data-member types. A class whose members
  // reach into many modules at once is the mechanical definition of a god
  // object, and is much easier to detect than to argue about.
  std::vector<std::string> member_modules;
};

// Scans a class/struct definition body by brace depth.
//
// Deliberately not a C++ parser. It understands the style this codebase
// actually uses — one type per header, no class bodies inside macros, no
// preprocessor conditionals splitting a class definition — and the architecture
// lint enforces that style, so the two hold each other up. Anything it cannot
// confidently attribute is not counted, so its error direction is
// under-counting (a missed violation), never over-counting (a false failure).
std::vector<ClassInfo> ExtractClasses(std::string_view text);

}  // namespace microbrowser::tests::architecture
