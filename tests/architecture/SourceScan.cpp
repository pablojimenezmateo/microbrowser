#include "architecture/SourceScan.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "TestSupport.h"
#include "util/StringUtil.h"

namespace microbrowser::tests::architecture {

namespace {

bool HasSourceExtension(const std::filesystem::path& path) {
  const std::string extension = path.extension().string();
  return extension == ".h" || extension == ".cpp";
}

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Whole-word search on already-masked text.
std::size_t FindWord(std::string_view text, std::string_view word, std::size_t from) {
  for (std::size_t at = text.find(word, from); at != std::string_view::npos;
       at = text.find(word, at + 1)) {
    const bool left_ok = at == 0 || !IsIdentifierChar(text[at - 1]);
    const std::size_t after = at + word.size();
    const bool right_ok = after >= text.size() || !IsIdentifierChar(text[after]);
    if (left_ok && right_ok) {
      return at;
    }
  }
  return std::string_view::npos;
}

// Index of the first non-whitespace character at or after `from`, or npos.
// Whitespace includes newlines: clang-format is free to break between `new`
// and the type it allocates, and a rule that missed those would be a rule that
// a reformat could silently switch off.
std::size_t SkipWhitespace(std::string_view text, std::size_t from) {
  while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from])) != 0) {
    ++from;
  }
  return from < text.size() ? from : std::string_view::npos;
}

int LineOfOffset(std::string_view text, std::size_t offset) {
  int line = 1;
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
    }
  }
  return line;
}

// One entry per sanctioned dependency group. An include that matches nothing
// here is either standard library or POSIX, both unrestricted; an include that
// matches is only legal in a module whose manifest names the group.
struct ExternPrefix {
  std::string_view prefix;
  std::string_view group;
};

constexpr ExternPrefix kExternPrefixes[] = {
    {"SDL3/", "SDL3"},
    {"ft2build.h", "freetype"},
    {"freetype/", "freetype"},
    {"hb.h", "harfbuzz"},
    {"hb-", "harfbuzz"},
    {"openssl/", "openssl"},
    {"zlib.h", "zlib"},
    {"zconf.h", "zlib"},
    {"brotli/", "brotli"},
    {"third_party/stb/", "stb"},
};

}  // namespace

SourceSet LoadSourceTree(const std::filesystem::path& repo_root) {
  SourceSet files;
  const std::filesystem::path src_root = repo_root / "src";
  if (!std::filesystem::exists(src_root)) {
    return files;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::recursive_directory_iterator(src_root)) {
    if (!entry.is_regular_file() || !HasSourceExtension(entry.path())) {
      continue;
    }
    SourceFile file;
    file.path = std::filesystem::relative(entry.path(), repo_root).generic_string();
    file.text = ReadFile(entry.path());
    files.push_back(std::move(file));
  }
  // Deterministic order so violation lists are stable and diffable.
  std::sort(files.begin(), files.end(),
            [](const SourceFile& a, const SourceFile& b) { return a.path < b.path; });
  return files;
}

ModuleManifests LoadModuleManifests(const std::filesystem::path& repo_root,
                                    std::vector<Violation>& errors) {
  ModuleManifests manifests;
  const std::filesystem::path src_root = repo_root / "src";
  if (!std::filesystem::exists(src_root)) {
    return manifests;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(src_root)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string module = entry.path().filename().string();
    const std::filesystem::path manifest_path = entry.path() / "MODULE.deps";
    if (!std::filesystem::exists(manifest_path)) {
      errors.push_back(Violation{"src/" + module + "/MODULE.deps", 0,
                                 "module has no MODULE.deps; every module must declare its "
                                 "purpose, dependencies, public surface, and budgets"});
      continue;
    }
    ManifestParseResult parsed = ParseModuleManifest(module, ReadFile(manifest_path));
    for (const std::string& error : parsed.errors) {
      errors.push_back(Violation{"src/" + module + "/MODULE.deps", 0, error});
    }
    manifests.emplace(module, std::move(parsed.manifest));
  }
  return manifests;
}

std::string ModuleOf(std::string_view repo_relative_path) {
  constexpr std::string_view kPrefix = "src/";
  if (!util::StartsWith(repo_relative_path, kPrefix)) {
    return {};
  }
  const std::string_view rest = repo_relative_path.substr(kPrefix.size());
  const std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  return std::string(rest.substr(0, slash));
}

std::string FileNameOf(std::string_view repo_relative_path) {
  const std::size_t slash = repo_relative_path.find_last_of('/');
  if (slash == std::string_view::npos) {
    return std::string(repo_relative_path);
  }
  return std::string(repo_relative_path.substr(slash + 1));
}

std::vector<std::size_t> FindCallSites(std::string_view masked, std::string_view name) {
  std::vector<std::size_t> offsets;
  for (std::size_t at = FindWord(masked, name, 0); at != std::string_view::npos;
       at = FindWord(masked, name, at + 1)) {
    // A member call on one of our own objects is not the C function. A
    // qualified one is: `std::strcpy` is exactly what is being banned.
    if (at > 0 && masked[at - 1] == '.') {
      continue;
    }
    if (at > 1 && masked[at - 1] == '>' && masked[at - 2] == '-') {
      continue;
    }
    const std::size_t open = SkipWhitespace(masked, at + name.size());
    if (open != std::string_view::npos && masked[open] == '(') {
      offsets.push_back(at);
    }
  }
  return offsets;
}

std::vector<std::size_t> FindManualHeapExpressions(std::string_view masked) {
  std::vector<std::size_t> offsets;

  for (std::size_t at = FindWord(masked, "new", 0); at != std::string_view::npos;
       at = FindWord(masked, "new", at + 1)) {
    // `new` must be followed by the type being allocated. When the next token
    // is `(` this is placement new or an `operator new` declaration, and
    // neither takes ownership of anything.
    const std::size_t next = SkipWhitespace(masked, at + 3);
    if (next != std::string_view::npos && IsIdentifierChar(masked[next])) {
      offsets.push_back(at);
    }
  }

  for (std::size_t at = FindWord(masked, "delete", 0); at != std::string_view::npos;
       at = FindWord(masked, "delete", at + 1)) {
    std::size_t next = SkipWhitespace(masked, at + 6);
    if (next == std::string_view::npos) {
      continue;
    }
    // `delete[] p` -- step over the brackets and require an operand after them.
    if (masked[next] == '[') {
      const std::size_t close = SkipWhitespace(masked, next + 1);
      if (close == std::string_view::npos || masked[close] != ']') {
        continue;
      }
      next = SkipWhitespace(masked, close + 1);
      if (next == std::string_view::npos) {
        continue;
      }
    }
    // Requiring an operand is what makes `= delete;` (a deleted function) and
    // `operator delete(...)` fall out for free rather than needing a special
    // case each.
    if (IsIdentifierChar(masked[next]) || masked[next] == '*') {
      offsets.push_back(at);
    }
  }

  std::sort(offsets.begin(), offsets.end());
  return offsets;
}

int LineAtOffset(std::string_view text, std::size_t offset) { return LineOfOffset(text, offset); }

std::string MaskCommentsAndStrings(std::string_view text) {
  std::string masked(text);
  enum class State { Code, LineComment, BlockComment, String, Char };
  State state = State::Code;

  for (std::size_t i = 0; i < masked.size(); ++i) {
    const char c = masked[i];
    const char next = (i + 1 < masked.size()) ? masked[i + 1] : '\0';

    switch (state) {
      case State::Code:
        if (c == '/' && next == '/') {
          state = State::LineComment;
          masked[i] = ' ';
          masked[i + 1] = ' ';
          ++i;
        } else if (c == '/' && next == '*') {
          state = State::BlockComment;
          masked[i] = ' ';
          masked[i + 1] = ' ';
          ++i;
        } else if (c == '"') {
          state = State::String;
        } else if (c == '\'') {
          state = State::Char;
        }
        break;

      case State::LineComment:
        if (c == '\n') {
          state = State::Code;
        } else {
          masked[i] = ' ';
        }
        break;

      case State::BlockComment:
        if (c == '*' && next == '/') {
          masked[i] = ' ';
          masked[i + 1] = ' ';
          ++i;
          state = State::Code;
        } else if (c != '\n') {
          masked[i] = ' ';
        }
        break;

      case State::String:
      case State::Char: {
        const char terminator = state == State::String ? '"' : '\'';
        if (c == '\\') {
          masked[i] = ' ';
          if (i + 1 < masked.size() && masked[i + 1] != '\n') {
            masked[i + 1] = ' ';
            ++i;
          }
        } else if (c == terminator) {
          state = State::Code;
        } else if (c != '\n') {
          masked[i] = ' ';
        }
        break;
      }
    }
  }
  return masked;
}

std::vector<IncludeDirective> ExtractIncludes(std::string_view text) {
  std::vector<IncludeDirective> includes;
  std::istringstream stream{std::string(text)};
  std::string raw_line;
  int line_number = 0;

  while (std::getline(stream, raw_line)) {
    ++line_number;
    const std::string_view line = util::TrimAscii(raw_line);
    if (!util::StartsWith(line, "#include")) {
      continue;
    }
    const std::string_view rest = util::TrimAscii(line.substr(std::string_view("#include").size()));
    if (rest.size() < 2) {
      continue;
    }
    const char opener = rest.front();
    const char closer = opener == '<' ? '>' : (opener == '"' ? '"' : '\0');
    if (closer == '\0') {
      continue;  // a macro-expanded include; not something a rule can attribute
    }
    const std::size_t end = rest.find(closer, 1);
    if (end == std::string_view::npos) {
      continue;
    }
    includes.push_back(IncludeDirective{line_number, std::string(rest.substr(1, end - 1)),
                                        opener == '<'});
  }
  return includes;
}

std::string ExternGroupFor(std::string_view include_target) {
  for (const ExternPrefix& entry : kExternPrefixes) {
    if (util::StartsWith(include_target, entry.prefix)) {
      return std::string(entry.group);
    }
  }
  return {};
}

std::vector<ClassInfo> ExtractClasses(std::string_view text) {
  const std::string masked = MaskCommentsAndStrings(text);
  std::vector<ClassInfo> classes;

  for (std::string_view keyword : {std::string_view("class"), std::string_view("struct")}) {
    std::size_t search = 0;
    while (true) {
      const std::size_t at = FindWord(masked, keyword, search);
      if (at == std::string_view::npos) {
        break;
      }
      search = at + keyword.size();

      // Name follows the keyword.
      std::size_t cursor = at + keyword.size();
      while (cursor < masked.size() && (masked[cursor] == ' ' || masked[cursor] == '\t')) {
        ++cursor;
      }
      const std::size_t name_start = cursor;
      while (cursor < masked.size() && IsIdentifierChar(masked[cursor])) {
        ++cursor;
      }
      if (cursor == name_start) {
        continue;  // anonymous struct, or `enum class` handled elsewhere
      }
      const std::string name = masked.substr(name_start, cursor - name_start);

      // Find the opening brace, refusing to cross a ';' (forward declaration)
      // or a '(' (a function parameter naming a class type).
      std::size_t brace = std::string::npos;
      for (std::size_t i = cursor; i < masked.size(); ++i) {
        const char c = masked[i];
        if (c == ';' || c == '(' || c == '=') {
          break;
        }
        if (c == '{') {
          brace = i;
          break;
        }
      }
      if (brace == std::string::npos) {
        continue;
      }

      ClassInfo info;
      info.name = name;
      info.start_line = LineOfOffset(masked, at);

      // Walk the body by brace depth, counting only at depth 1 so nested types
      // are attributed to themselves (they get their own ClassInfo from the
      // outer loop) rather than inflating the enclosing class.
      int depth = 0;
      bool is_public = keyword == "struct";
      std::size_t statement_start = brace + 1;
      std::size_t end = masked.size();

      for (std::size_t i = brace; i < masked.size(); ++i) {
        const char c = masked[i];
        if (c == '{') {
          ++depth;
          if (depth == 1) {
            statement_start = i + 1;
          }
          continue;
        }
        if (c == '}') {
          --depth;
          if (depth == 0) {
            end = i;
            break;
          }
          if (depth == 1) {
            statement_start = i + 1;
          }
          continue;
        }
        if (depth != 1) {
          continue;
        }

        if (c == ':') {
          // Possible access specifier: look back at the statement so far.
          const std::string_view pending =
              util::TrimAscii(std::string_view(masked).substr(statement_start,
                                                              i - statement_start));
          if (pending == "public") {
            is_public = true;
            statement_start = i + 1;
            continue;
          }
          if (pending == "private" || pending == "protected") {
            is_public = false;
            statement_start = i + 1;
            continue;
          }
          continue;
        }

        if (c != ';') {
          continue;
        }

        const std::string_view statement =
            util::TrimAscii(std::string_view(masked).substr(statement_start, i - statement_start));
        statement_start = i + 1;
        if (statement.empty()) {
          continue;
        }

        const bool is_declaration_only =
            statement.find("using ") == std::string_view::npos &&
            statement.find("typedef ") == std::string_view::npos &&
            statement.find("friend ") == std::string_view::npos &&
            statement.find("static_assert") == std::string_view::npos;
        if (!is_declaration_only) {
          continue;
        }

        if (statement.find('(') != std::string_view::npos) {
          // A declaration with a parameter list: a method.
          if (is_public) {
            ++info.public_methods;
          }
          continue;
        }

        // A data member, unless it is static (shared state, counted by a
        // different rule) or an enumerator list.
        if (util::StartsWith(statement, "static") || util::StartsWith(statement, "enum")) {
          continue;
        }
        ++info.members;

        // Record which modules this member's type reaches into.
        std::size_t scope = statement.find("::");
        while (scope != std::string_view::npos) {
          std::size_t begin = scope;
          while (begin > 0 && IsIdentifierChar(statement[begin - 1])) {
            --begin;
          }
          if (begin < scope) {
            std::string qualifier(statement.substr(begin, scope - begin));
            if (std::find(info.member_modules.begin(), info.member_modules.end(), qualifier) ==
                info.member_modules.end()) {
              info.member_modules.push_back(std::move(qualifier));
            }
          }
          scope = statement.find("::", scope + 2);
        }
      }

      // A body whose braces never balanced means the scanner lost track; report
      // nothing rather than a wrong number.
      if (end >= masked.size()) {
        continue;
      }

      // Methods defined inline in the body end with '}' rather than ';', so
      // they are not counted above. Count them by scanning for a depth-1 '{'
      // that follows a parameter list.
      {
        int inline_depth = 0;
        bool inline_public = keyword == "struct";
        std::size_t segment_start = brace + 1;
        for (std::size_t i = brace; i <= end; ++i) {
          const char c = masked[i];
          if (c == '{') {
            ++inline_depth;
            if (inline_depth == 2) {
              const std::string_view head = util::TrimAscii(
                  std::string_view(masked).substr(segment_start, i - segment_start));
              if (!head.empty() && head.find('(') != std::string_view::npos &&
                  head.find("class") == std::string_view::npos &&
                  head.find("struct") == std::string_view::npos &&
                  head.find("enum") == std::string_view::npos && inline_public) {
                ++info.public_methods;
              }
            }
            continue;
          }
          if (c == '}') {
            --inline_depth;
            if (inline_depth == 1) {
              segment_start = i + 1;
            }
            continue;
          }
          if (inline_depth != 1) {
            continue;
          }
          if (c == ';') {
            segment_start = i + 1;
          } else if (c == ':') {
            const std::string_view pending = util::TrimAscii(
                std::string_view(masked).substr(segment_start, i - segment_start));
            if (pending == "public") {
              inline_public = true;
              segment_start = i + 1;
            } else if (pending == "private" || pending == "protected") {
              inline_public = false;
              segment_start = i + 1;
            }
          }
        }
      }

      const int end_line = LineOfOffset(masked, end);
      info.header_lines = static_cast<std::size_t>(end_line - info.start_line + 1);
      classes.push_back(std::move(info));
    }
  }

  std::sort(classes.begin(), classes.end(),
            [](const ClassInfo& a, const ClassInfo& b) { return a.start_line < b.start_line; });
  return classes;
}

}  // namespace microbrowser::tests::architecture
