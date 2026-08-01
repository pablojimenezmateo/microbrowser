#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "architecture/ModuleManifest.h"
#include "architecture/SourceScan.h"
#include "util/StringUtil.h"

// The architecture lint.
//
// Its job is to make the design decisions in guidelines/architecture.md
// mechanical, so that a year from now they are still true. Three sources
// informed the shape:
//
//   Chromium   per-directory DEPS include_rules, checked by presubmit.
//   Gecko      moz.build EXPORTS: a module's public surface is declared, and
//              anything unlisted is unreachable from outside it.
//   Ladybird   one library per subsystem with explicit link dependencies.
//
// The per-class budgets are this project's own, and are the direct answer to
// "how do classes not grow unbounded": growth past a declared number is a test
// failure, and raising the number is an edit to MODULE.deps that shows up in
// the diff.
//
// Every rule below is run twice: once over the real tree (must pass) and once
// over a fixture that is known to violate it (must fail). A lint nobody has
// watched fail is not a lint.

namespace microbrowser::tests {

namespace {

using architecture::ClassInfo;
using architecture::IncludeDirective;
using architecture::ModuleManifest;
using architecture::ModuleManifests;
using architecture::Rule;
using architecture::SourceFile;
using architecture::SourceSet;
using architecture::Violation;

// A class body smaller than this is not worth a manifest entry; anything at or
// above it must declare a budget, so a new god object cannot appear unbudgeted.
constexpr std::size_t kBudgetRequiredAtHeaderLines = 25;

// A class whose data members reach into more modules than this is coordinating
// too much. Four is enough for a legitimate coordinator (its own module plus
// three collaborators) and short of the eight-plus that characterizes a shell
// object.
constexpr std::size_t kMaxMemberModules = 4;

Violation At(const SourceFile& file, int line, std::string message) {
  return Violation{file.path, line, std::move(message)};
}

// --- Rules -------------------------------------------------------------------

// Chromium's DEPS include_rules, plus Gecko's EXPORTS in one check: a project
// include must name an allowed module, and must name a header that module
// actually publishes.
std::vector<Violation> CheckModuleIncludeRules(const SourceSet& files,
                                               const ModuleManifests& manifests) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    const std::string module = architecture::ModuleOf(file.path);
    if (module.empty()) {
      continue;
    }
    const auto manifest = manifests.find(module);
    if (manifest == manifests.end()) {
      continue;  // reported by the manifest-exists rule
    }

    for (const IncludeDirective& include : architecture::ExtractIncludes(file.text)) {
      if (include.angled) {
        continue;  // handled by the extern rule
      }
      const std::string target_module = architecture::ModuleOf("src/" + include.target);
      if (target_module.empty() || target_module == module) {
        continue;  // own module, or not a module-qualified project header
      }
      const std::string header = architecture::FileNameOf(include.target);

      if (!manifest->second.Allows(target_module)) {
        violations.push_back(At(file, include.line,
                                "module '" + module + "' includes '" + include.target +
                                    "' but does not list '" + target_module +
                                    "' in its MODULE.deps allow:"));
        continue;
      }
      const auto target = manifests.find(target_module);
      if (target != manifests.end() && !target->second.Exports(header)) {
        violations.push_back(At(file, include.line,
                                "'" + include.target + "' is private to module '" +
                                    target_module + "'; add it to that module's public: list "
                                    "if it is meant to be part of its surface"));
      }
    }
  }
  return violations;
}

// A third-party header may only appear in a module whose manifest names its
// group. This is what enforces "gfx must not include SDL", "SDL lives only in
// platform", and "OpenSSL only in net" without a rule per library.
std::vector<Violation> CheckExternIncludesAreDeclared(const SourceSet& files,
                                                      const ModuleManifests& manifests) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    const std::string module = architecture::ModuleOf(file.path);
    const auto manifest = manifests.find(module);
    if (module.empty() || manifest == manifests.end()) {
      continue;
    }
    for (const IncludeDirective& include : architecture::ExtractIncludes(file.text)) {
      const std::string group = architecture::ExternGroupFor(include.target);
      if (group.empty() || manifest->second.AllowsExtern(group)) {
        continue;
      }
      violations.push_back(At(file, include.line,
                              "module '" + module + "' includes third-party header '" +
                                  include.target + "' (group '" + group +
                                  "') without declaring it in MODULE.deps extern:"));
    }
  }
  return violations;
}

std::vector<Violation> CheckTranslationUnitLineCaps(const SourceSet& files,
                                                    const ModuleManifests& manifests) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    const std::string module = architecture::ModuleOf(file.path);
    const auto manifest = manifests.find(module);
    if (module.empty() || manifest == manifests.end() || manifest->second.max_tu_lines == 0) {
      continue;
    }
    const std::size_t lines =
        static_cast<std::size_t>(std::count(file.text.begin(), file.text.end(), '\n')) + 1;
    if (lines > manifest->second.max_tu_lines) {
      violations.push_back(At(file, 0,
                              "file is " + std::to_string(lines) + " lines, over the module cap of " +
                                  std::to_string(manifest->second.max_tu_lines) +
                                  "; a file over its cap means a missing module, not a bigger file"));
    }
  }
  return violations;
}

// The anti-growth rule. Two halves, and the second matters more than the first:
// exceeding a budget fails, and *not having* a budget fails too.
std::vector<Violation> CheckClassBudgets(const SourceSet& files,
                                         const ModuleManifests& manifests) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (!file.IsHeader()) {
      continue;  // budgets describe declared surface, which lives in headers
    }
    const std::string module = architecture::ModuleOf(file.path);
    const auto manifest = manifests.find(module);
    if (module.empty() || manifest == manifests.end()) {
      continue;
    }

    for (const ClassInfo& info : architecture::ExtractClasses(file.text)) {
      const architecture::ClassBudget* budget = manifest->second.FindBudget(info.name);
      if (budget == nullptr) {
        if (info.header_lines >= kBudgetRequiredAtHeaderLines) {
          violations.push_back(At(file, info.start_line,
                                  "class '" + info.name + "' is " +
                                      std::to_string(info.header_lines) +
                                      " header lines but has no budget in src/" + module +
                                      "/MODULE.deps; declare one so later growth is visible"));
        }
        continue;
      }
      if (info.header_lines > budget->header_lines) {
        violations.push_back(At(file, info.start_line,
                                "class '" + info.name + "' is " +
                                    std::to_string(info.header_lines) + " header lines, over its "
                                    "budget of " + std::to_string(budget->header_lines)));
      }
      if (info.public_methods > budget->public_methods) {
        violations.push_back(At(file, info.start_line,
                                "class '" + info.name + "' has " +
                                    std::to_string(info.public_methods) +
                                    " public methods, over its budget of " +
                                    std::to_string(budget->public_methods)));
      }
      if (info.members > budget->members) {
        violations.push_back(At(file, info.start_line,
                                "class '" + info.name + "' has " + std::to_string(info.members) +
                                    " data members, over its budget of " +
                                    std::to_string(budget->members)));
      }
    }
  }
  return violations;
}

// God-object detector. A class whose data members reach into many modules at
// once is coordinating too much, whatever its line count says.
std::vector<Violation> CheckClassFanOut(const SourceSet& files,
                                        const ModuleManifests& manifests) {
  std::vector<Violation> violations;
  std::set<std::string> module_names;
  for (const auto& [name, manifest] : manifests) {
    module_names.insert(name);
  }

  for (const SourceFile& file : files) {
    if (!file.IsHeader()) {
      continue;
    }
    for (const ClassInfo& info : architecture::ExtractClasses(file.text)) {
      std::set<std::string> reached;
      for (const std::string& qualifier : info.member_modules) {
        if (module_names.count(qualifier) != 0) {
          reached.insert(qualifier);
        }
      }
      if (reached.size() > kMaxMemberModules) {
        std::ostringstream names;
        for (const std::string& name : reached) {
          names << (names.tellp() == 0 ? "" : ", ") << name;
        }
        violations.push_back(At(file, info.start_line,
                                "class '" + info.name + "' holds members from " +
                                    std::to_string(reached.size()) + " modules (" + names.str() +
                                    "); split the coordination rather than widening the class"));
      }
    }
  }
  return violations;
}

// Ported from microide, where it exists because a locale-dependent, throwing
// parse of a persisted number is a real bug that shipped.
std::vector<Violation> CheckNoThrowingNumericParse(const SourceSet& files,
                                                   const ModuleManifests&) {
  static constexpr std::string_view kBanned[] = {"std::stoi", "std::stol", "std::stoll",
                                                 "std::stoul", "std::stoull", "std::stof",
                                                 "std::stod"};
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    std::istringstream stream(masked);
    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
      ++line_number;
      for (const std::string_view banned : kBanned) {
        if (line.find(banned) != std::string::npos) {
          violations.push_back(At(file, line_number,
                                  std::string(banned) +
                                      " throws and reads the decimal separator from the process "
                                      "locale; use the util/Parse.h helpers"));
        }
      }
    }
  }
  return violations;
}

// A browser's entire input surface is hostile, so the C functions that cannot
// be called safely on attacker-influenced data are not available at all.
//
// The point of banning them by name rather than by review is that each one has
// a safe-looking call site. `strcpy` into a buffer that is "obviously" big
// enough is the single most common memory-corruption bug in the history of
// browsers, and it is obviously big enough right up until a length field says
// otherwise. See guidelines/security.md.
std::vector<Violation> CheckNoBannedCFunctions(const SourceSet& files, const ModuleManifests&) {
  struct Banned {
    std::string_view name;
    std::string_view reason;
  };
  static constexpr Banned kBanned[] = {
      {"strcpy", "writes until a NUL the input controls; use std::string or a sized copy"},
      {"strcat", "writes until a NUL the input controls; use std::string"},
      {"stpcpy", "writes until a NUL the input controls; use std::string"},
      {"sprintf", "has no output bound at all; use std::snprintf or std::format"},
      {"vsprintf", "has no output bound at all; use std::vsnprintf"},
      {"gets", "cannot be called safely under any circumstances"},
      {"strncpy", "does not terminate on truncation, so the next read runs off the end"},
      {"strncat", "takes the remaining space, not the buffer size; the off-by-one is the default"},
      {"alloca", "puts an input-influenced length on the stack, past every heap guard"},
      {"strtok", "mutates its input and keeps hidden static state shared across callers"},
      {"atoi", "returns 0 for unparsable input and is undefined on overflow; use util/Parse.h"},
      {"atol", "returns 0 for unparsable input and is undefined on overflow; use util/Parse.h"},
      {"atoll", "returns 0 for unparsable input and is undefined on overflow; use util/Parse.h"},
      {"atof", "returns 0 for unparsable input and reads the locale; use util/Parse.h"},
      {"rand", "is not a CSPRNG; anything a page can observe needs unpredictable bytes"},
      {"srand", "is not a CSPRNG; anything a page can observe needs unpredictable bytes"},
      {"tmpnam", "names a file without creating it, which is a TOCTOU by construction"},
      {"mktemp", "names a file without creating it, which is a TOCTOU by construction"},
      {"system", "runs a shell; process spawning belongs behind the broker, never inline"},
      {"popen", "runs a shell; process spawning belongs behind the broker, never inline"},
  };

  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    for (const Banned& banned : kBanned) {
      for (const std::size_t offset : architecture::FindCallSites(masked, banned.name)) {
        violations.push_back(At(file, architecture::LineAtOffset(file.text, offset),
                                std::string(banned.name) + " is banned: " +
                                    std::string(banned.reason)));
      }
    }
  }
  return violations;
}

// Ownership is RAII or it is not ownership. A raw owning pointer is a leak, a
// double free, or a use-after-free waiting for an early return to be added
// above it — and in a browser, a use-after-free is a remote code execution
// primitive, not a crash.
//
// This rule covers the owning forms only. Placement new, `operator new`, and
// `= delete` are all deliberately outside it; see FindManualHeapExpressions.
std::vector<Violation> CheckNoManualHeapOwnership(const SourceSet& files, const ModuleManifests&) {
  static constexpr std::string_view kBannedAllocators[] = {"malloc", "calloc", "realloc", "free"};

  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);

    for (const std::size_t offset : architecture::FindManualHeapExpressions(masked)) {
      violations.push_back(At(file, architecture::LineAtOffset(file.text, offset),
                              "manual new/delete: heap lifetime belongs to a container, a "
                              "unique_ptr, or a value member. A raw owning pointer becomes a "
                              "use-after-free the first time an early return is added above it"));
    }
    for (const std::string_view allocator : kBannedAllocators) {
      for (const std::size_t offset : architecture::FindCallSites(masked, allocator)) {
        violations.push_back(At(file, architecture::LineAtOffset(file.text, offset),
                                std::string(allocator) +
                                    " is C memory management with no destructor behind it; use a "
                                    "container or unique_ptr"));
      }
    }
  }
  return violations;
}

// The environment is input, and it is input that survives into every child
// process. Code that reads it ad hoc has grown a configuration surface nobody
// reviewed and nobody can enumerate.
//
// Keeping every read in one translation unit means the whole surface is a
// single file to read, and it is also how three separate "is this flag on"
// implementations stopped agreeing by coincidence and started agreeing by
// construction.
std::vector<Violation> CheckEnvironmentReadsAreCentralized(const SourceSet& files,
                                                           const ModuleManifests&) {
  static constexpr std::string_view kOwner = "src/util/Env.cpp";
  static constexpr std::string_view kReaders[] = {"getenv", "secure_getenv", "environ"};

  std::vector<Violation> violations;
  bool owner_present = false;

  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    const bool is_owner = file.path == kOwner;
    owner_present = owner_present || is_owner;
    if (is_owner) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    for (const std::string_view reader : kReaders) {
      for (const std::size_t offset : architecture::FindCallSites(masked, reader)) {
        violations.push_back(At(file, architecture::LineAtOffset(file.text, offset),
                                std::string(reader) + " outside " + std::string(kOwner) +
                                    ": every environment read goes through util::EnvValue or "
                                    "util::EnvFlagEnabled, so the process's whole configuration "
                                    "surface stays enumerable"));
      }
    }
  }

  // Without this the rule goes vacuous the day Env.cpp is renamed: the owner
  // would no longer be found, nothing would be exempt, and nothing would be
  // flagged either, because there would be no reads left anywhere to flag. A
  // rule that passes because its subject vanished is the failure mode the
  // control fixtures exist to prevent, so it is checked rather than assumed.
  if (!owner_present) {
    violations.push_back(Violation{std::string(kOwner), 0,
                                   "the file that owns environment reads does not exist; this "
                                   "rule has nothing to exempt and is no longer checking anything"});
  }
  return violations;
}

// Mutable state at namespace scope is invisible coupling: two modules that
// never mention each other can still interfere. Function-local statics are
// fine — they are initialized on first use and cannot be reached without
// calling the function that owns them.
std::vector<Violation> CheckNoNamespaceScopeMutableState(const SourceSet& files,
                                                         const ModuleManifests&) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    std::istringstream stream(masked);
    std::string raw_line;
    int line_number = 0;
    int brace_depth = 0;

    while (std::getline(stream, raw_line)) {
      ++line_number;
      const std::string_view line = util::TrimAscii(raw_line);
      const int depth_at_line_start = brace_depth;
      brace_depth += static_cast<int>(std::count(raw_line.begin(), raw_line.end(), '{'));
      brace_depth -= static_cast<int>(std::count(raw_line.begin(), raw_line.end(), '}'));

      // Only namespace scope. A namespace opening brace does not increase the
      // depth we care about, so track it by requiring the line to be a plain
      // declaration ending in ';' at depth 0.
      if (depth_at_line_start != 0 || line.empty() || line.back() != ';') {
        continue;
      }
      if (!util::StartsWith(line, "static ") && !util::StartsWith(line, "inline ")) {
        continue;
      }
      const bool is_const = line.find("const ") != std::string_view::npos ||
                            line.find("constexpr ") != std::string_view::npos;
      const bool is_function = line.find('(') != std::string_view::npos;
      if (is_const || is_function) {
        continue;
      }
      violations.push_back(At(file, line_number,
                              "mutable state at namespace scope creates coupling no include "
                              "graph can show; pass ownership explicitly or use a "
                              "function-local static"));
    }
  }
  return violations;
}

std::vector<Violation> CheckHeadersUsePragmaOnce(const SourceSet& files, const ModuleManifests&) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    if (!file.IsHeader() || architecture::ModuleOf(file.path).empty()) {
      continue;
    }
    if (file.text.find("#pragma once") == std::string::npos) {
      violations.push_back(At(file, 1, "header is missing '#pragma once'"));
    }
  }
  return violations;
}

// Types allocated once per DOM node, per layout box, per display command, or
// per JS value have their size multiplied by the document. A static_assert
// makes growth a compile error instead of a memory-profile mystery six months
// later; this rule checks that the assert still exists.
std::vector<Violation> CheckObjectSizeBudgetsArePresent(const SourceSet& files,
                                                        const ModuleManifests&) {
  struct SizeBudget {
    std::string_view file;
    std::string_view type;
  };
  static constexpr SizeBudget kBudgets[] = {
      {"src/gfx/Color.h", "Color"},
      {"src/gfx/Geometry.h", "IntRect"},
      {"src/gfx/DisplayList.h", "DisplayCommand"},
  };

  std::vector<Violation> violations;
  for (const SizeBudget& budget : kBudgets) {
    const auto found = std::find_if(files.begin(), files.end(), [&](const SourceFile& file) {
      return file.path == budget.file;
    });
    if (found == files.end()) {
      violations.push_back(Violation{std::string(budget.file), 0,
                                     "file listed in the object-size budget table does not "
                                     "exist; update the table or restore the file"});
      continue;
    }
    const std::string needle = "sizeof(" + std::string(budget.type) + ")";
    const std::string masked = architecture::MaskCommentsAndStrings(found->text);
    if (masked.find("static_assert") == std::string::npos ||
        masked.find(needle) == std::string::npos) {
      violations.push_back(Violation{std::string(budget.file), 0,
                                     "type '" + std::string(budget.type) +
                                         "' has lost its static_assert(sizeof(...)) size budget"});
    }
  }
  return violations;
}


// --- M2 rules ----------------------------------------------------------------
//
// AGENTS.md scheduled these three for M2 and said plainly why they were not
// written earlier: "with zero call sites it would pass while checking nothing,
// which is the exact failure the control fixtures exist to prevent." The call
// sites now exist.

// Every network request carries a privacy::Verdict.
//
// The rule is not "somebody remembered to call the privacy layer" — it is that
// there is no way not to. `net::Fetch` takes a Verdict by value, and a second
// entry point that did not would silently become the one people used.
std::vector<Violation> CheckFetchRequiresAVerdict(const SourceSet& files,
                                                  const ModuleManifests&) {
  std::vector<Violation> violations;
  bool found_declaration = false;

  for (const SourceFile& file : files) {
    if (architecture::ModuleOf(file.path) != "net") {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    for (const std::size_t at : architecture::FindCallSites(masked, "Fetch")) {
      // Only declarations, which in this codebase are the lines that name a
      // return type before the identifier.
      const std::size_t line_start = masked.rfind('\n', at);
      const std::string_view line(masked.data() + (line_start == std::string::npos ? 0 : line_start + 1),
                                  0);
      (void)line;
      const std::size_t open = masked.find('(', at);
      if (open == std::string::npos) {
        continue;
      }
      const std::size_t close = masked.find(')', open);
      const std::string parameters =
          masked.substr(open + 1, close == std::string::npos ? 0 : close - open - 1);
      if (parameters.find("Verdict") == std::string::npos) {
        continue;
      }
      found_declaration = true;
      // A Verdict passed by const reference is a Verdict a caller can keep and
      // reuse for a different request. By value, it is consumed.
      if (parameters.find("const privacy::Verdict&") != std::string::npos ||
          parameters.find("const Verdict&") != std::string::npos) {
        violations.push_back(Violation{
            file.path, architecture::LineAtOffset(file.text, at),
            "net::Fetch must take privacy::Verdict by value, not by const reference: a "
            "borrowed verdict can be reused for a request it was not issued for"});
      }
    }
  }

  const bool net_module_exists =
      std::any_of(files.begin(), files.end(),
                  [](const SourceFile& file) { return architecture::ModuleOf(file.path) == "net"; });
  if (net_module_exists && !found_declaration) {
    violations.push_back(Violation{
        "src/net", 0,
        "src/net exists but declares no Fetch taking a privacy::Verdict; the rule that every "
        "request passes the privacy layer is enforced by that signature and nothing else"});
  }
  return violations;
}

// Every storage-like lookup takes a PartitionKey.
//
// The cookie jar, the HTTP cache, and everything per-site that follows. A
// lookup keyed on a URL alone is one that shares state across partitions, which
// is the failure Total Cookie Protection exists to prevent.
std::vector<Violation> CheckStorageLookupsArePartitioned(const SourceSet& files,
                                                         const ModuleManifests&) {
  std::vector<Violation> violations;
  static constexpr std::string_view kPartitionedTypes[] = {"CookieJar", "HttpCache"};

  for (const SourceFile& file : files) {
    if (!file.IsHeader()) {
      continue;
    }
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);
    for (const ClassInfo& info : architecture::ExtractClasses(file.text)) {
      const bool partitioned =
          std::any_of(std::begin(kPartitionedTypes), std::end(kPartitionedTypes),
                      [&info](std::string_view name) { return info.name == name; });
      if (!partitioned) {
        continue;
      }
      // Every public method that looks like a lookup or a store must name a
      // PartitionKey in its parameter list.
      static constexpr std::string_view kLookupNames[] = {"Lookup", "Store", "CookiesFor",
                                                          "HeaderFor", "StoreFromHeader"};
      for (const std::string_view name : kLookupNames) {
        for (const std::size_t at : architecture::FindCallSites(masked, name)) {
          if (architecture::LineAtOffset(file.text, at) < info.start_line) {
            continue;
          }
          const std::size_t open = masked.find('(', at);
          const std::size_t close = open == std::string::npos ? std::string::npos
                                                              : masked.find(')', open);
          if (open == std::string::npos || close == std::string::npos) {
            continue;
          }
          const std::string parameters = masked.substr(open + 1, close - open - 1);
          if (parameters.find("PartitionKey") == std::string::npos) {
            violations.push_back(Violation{
                file.path, architecture::LineAtOffset(file.text, at),
                std::string(info.name) + "::" + std::string(name) +
                    " must take a url::PartitionKey; a lookup that does not is state shared "
                    "across partitions"});
          }
        }
      }
    }
  }
  return violations;
}

// Descriptor creation is close-on-exec on the creating call.
//
// A browser spawns helper processes. A descriptor without O_CLOEXEC or
// SOCK_CLOEXEC is inherited by all of them, and a follow-up fcntl leaves a
// window between the two calls in which a fork inherits it anyway.
std::vector<Violation> CheckDescriptorsAreCloseOnExec(const SourceSet& files,
                                                      const ModuleManifests&) {
  std::vector<Violation> violations;
  for (const SourceFile& file : files) {
    const std::string masked = architecture::MaskCommentsAndStrings(file.text);

    for (const std::size_t at : architecture::FindCallSites(masked, "socket")) {
      const std::size_t close = masked.find(')', at);
      const std::string call = masked.substr(at, close == std::string::npos ? 0 : close - at);
      if (call.find("SOCK_CLOEXEC") == std::string::npos) {
        violations.push_back(Violation{file.path, architecture::LineAtOffset(file.text, at),
                                       "socket() must pass SOCK_CLOEXEC on the creating call"});
      }
    }
    for (const std::string_view name : {"open", "openat"}) {
      for (const std::size_t at : architecture::FindCallSites(masked, name)) {
        const std::size_t close = masked.find(')', at);
        const std::string call = masked.substr(at, close == std::string::npos ? 0 : close - at);
        if (call.find("O_CLOEXEC") == std::string::npos) {
          violations.push_back(Violation{file.path, architecture::LineAtOffset(file.text, at),
                                         "open() must pass O_CLOEXEC on the creating call"});
        }
      }
    }
    // The follow-up form, which is the bug this rule is really about.
    for (const std::size_t at : architecture::FindCallSites(masked, "fcntl")) {
      const std::size_t close = masked.find(')', at);
      const std::string call = masked.substr(at, close == std::string::npos ? 0 : close - at);
      if (call.find("FD_CLOEXEC") != std::string::npos) {
        violations.push_back(Violation{
            file.path, architecture::LineAtOffset(file.text, at),
            "close-on-exec must be set on the creating call, not by a follow-up fcntl: "
            "between the two, a fork inherits the descriptor"});
      }
    }
  }
  return violations;
}

const Rule kRules[] = {
    {"ModuleIncludeRules", CheckModuleIncludeRules},
    {"ExternIncludesAreDeclared", CheckExternIncludesAreDeclared},
    {"TranslationUnitLineCaps", CheckTranslationUnitLineCaps},
    {"ClassBudgets", CheckClassBudgets},
    {"ClassFanOut", CheckClassFanOut},
    {"NoThrowingNumericParse", CheckNoThrowingNumericParse},
    {"NoBannedCFunctions", CheckNoBannedCFunctions},
    {"NoManualHeapOwnership", CheckNoManualHeapOwnership},
    {"EnvironmentReadsAreCentralized", CheckEnvironmentReadsAreCentralized},
    {"NoNamespaceScopeMutableState", CheckNoNamespaceScopeMutableState},
    {"HeadersUsePragmaOnce", CheckHeadersUsePragmaOnce},
    {"ObjectSizeBudgetsArePresent", CheckObjectSizeBudgetsArePresent},
    {"FetchRequiresAVerdict", CheckFetchRequiresAVerdict},
    {"StorageLookupsArePartitioned", CheckStorageLookupsArePartitioned},
    {"DescriptorsAreCloseOnExec", CheckDescriptorsAreCloseOnExec},
};

// --- Fixtures ----------------------------------------------------------------
//
// Each rule gets a clean fixture (must produce no violations) and a dirty one
// (must produce at least one). The dirty fixture is what proves the rule's
// pattern actually matches something — the failure mode a silent lint has.

ModuleManifests FixtureManifests() {
  ModuleManifests manifests;

  ModuleManifest util;
  util.name = "util";
  util.purpose = "fixture";
  util.publics = {"Parse.h"};
  util.max_tu_lines = 10;
  manifests.emplace("util", util);

  ModuleManifest gfx;
  gfx.name = "gfx";
  gfx.purpose = "fixture";
  gfx.allow = {"util"};
  gfx.publics = {"Canvas.h"};
  gfx.max_tu_lines = 10;
  gfx.budgets.push_back(architecture::ClassBudget{"Small", 30, 2, 2});
  manifests.emplace("gfx", gfx);

  // ipc and engine exist in the fixture set only so the fan-out rule has more
  // than kMaxMemberModules modules available to reach into. Without them its
  // dirty fixture cannot trip the rule — which is precisely what the control
  // check caught the first time this ran.
  ModuleManifest ipc;
  ipc.name = "ipc";
  ipc.purpose = "fixture";
  ipc.publics = {"Message.h"};
  ipc.max_tu_lines = 10;
  manifests.emplace("ipc", ipc);

  ModuleManifest engine;
  engine.name = "engine";
  engine.purpose = "fixture";
  engine.publics = {"Engine.h"};
  engine.max_tu_lines = 10;
  manifests.emplace("engine", engine);

  ModuleManifest platform;
  platform.name = "platform";
  platform.purpose = "fixture";
  platform.allow = {"gfx"};
  platform.publics = {"Window.h"};
  platform.externs = {"SDL3"};
  platform.max_tu_lines = 10;
  manifests.emplace("platform", platform);

  return manifests;
}

SourceSet Fixture(std::string path, std::string text) {
  return SourceSet{SourceFile{std::move(path), std::move(text)}};
}

struct RuleFixture {
  std::string_view rule;
  SourceSet clean;
  SourceSet dirty;
};

std::vector<RuleFixture> BuildFixtures() {
  std::vector<RuleFixture> fixtures;

  fixtures.push_back(RuleFixture{
      "ModuleIncludeRules",
      // gfx may include util, and Parse.h is util's published surface.
      Fixture("src/gfx/Canvas.cpp", "#include \"util/Parse.h\"\n"),
      // gfx may not include platform at all.
      Fixture("src/gfx/Canvas.cpp", "#include \"platform/Window.h\"\n")});

  fixtures.push_back(RuleFixture{
      "ModuleIncludeRules",
      Fixture("src/platform/Window.cpp", "#include \"gfx/Canvas.h\"\n"),
      // Allowed module, but the header is not in gfx's public: list.
      Fixture("src/platform/Window.cpp", "#include \"gfx/Internal.h\"\n")});

  fixtures.push_back(RuleFixture{
      "ExternIncludesAreDeclared",
      Fixture("src/platform/Window.cpp", "#include <SDL3/SDL.h>\n"),
      Fixture("src/gfx/Canvas.cpp", "#include <SDL3/SDL.h>\n")});

  fixtures.push_back(RuleFixture{
      "TranslationUnitLineCaps",
      Fixture("src/gfx/Canvas.cpp", "// three\n// short\n// lines\n"),
      Fixture("src/gfx/Canvas.cpp", std::string(40, '\n'))});

  fixtures.push_back(RuleFixture{
      "ClassBudgets",
      Fixture("src/gfx/Canvas.h",
              "#pragma once\nclass Small {\n public:\n  void A();\n  void B();\n"
              " private:\n  int x_;\n  int y_;\n};\n"),
      // Four public methods against a budget of two.
      Fixture("src/gfx/Canvas.h",
              "#pragma once\nclass Small {\n public:\n  void A();\n  void B();\n  void C();\n"
              "  void D();\n private:\n  int x_;\n};\n")});

  fixtures.push_back(RuleFixture{
      "ClassBudgets",
      Fixture("src/gfx/Canvas.h", "#pragma once\nstruct Tiny {\n  int a;\n};\n"),
      // Large enough to require a budget, and has none.
      Fixture("src/gfx/Canvas.h",
              "#pragma once\nclass Unbudgeted {\n public:\n" + std::string(30, '\n') + "};\n")});

  fixtures.push_back(RuleFixture{
      "ClassFanOut",
      Fixture("src/gfx/Canvas.h",
              "#pragma once\nstruct Ok {\n  util::A a_;\n  gfx::B b_;\n};\n"),
      Fixture("src/gfx/Canvas.h",
              "#pragma once\nstruct TooWide {\n  util::A a_;\n  gfx::B b_;\n  ipc::C c_;\n"
              "  engine::D d_;\n  platform::E e_;\n};\n")});

  fixtures.push_back(RuleFixture{
      "NoThrowingNumericParse",
      Fixture("src/gfx/Canvas.cpp", "int v = util::ParseInt(text).value_or(0);\n"),
      Fixture("src/gfx/Canvas.cpp", "int v = std::stoi(text);\n")});

  fixtures.push_back(RuleFixture{
      "NoThrowingNumericParse",
      // A banned name inside a comment must not trip the rule.
      Fixture("src/gfx/Canvas.cpp", "// never call std::stoi here\n"),
      Fixture("src/gfx/Canvas.cpp", "long v = std::stoll(text);\n")});

  fixtures.push_back(RuleFixture{
      "NoBannedCFunctions",
      // The near misses that make substring-matching versions of this rule
      // useless: snprintf contains no banned name, srand must not be found
      // inside it by a `rand` search, and a method of ours may be called
      // anything at all.
      Fixture("src/gfx/Canvas.cpp",
              "std::snprintf(buffer, sizeof(buffer), \"%d\", n);\n"
              "generator.rand();\n"
              "channel_->system();\n"
              "int v = util::ParseInt(text).value_or(0);\n"),
      Fixture("src/gfx/Canvas.cpp", "std::strcpy(buffer, header_value);\n")});

  fixtures.push_back(RuleFixture{
      "NoBannedCFunctions",
      Fixture("src/gfx/Canvas.cpp", "double v = std::strtod(text, &end);\n"),
      // Qualification must not launder a banned call.
      Fixture("src/gfx/Canvas.cpp", "int v = std::atoi(header_value);\n")});

  fixtures.push_back(RuleFixture{
      "NoManualHeapOwnership",
      Fixture("src/gfx/Canvas.cpp",
              "auto p = std::make_unique<Impl>();\n"
              "Canvas(const Canvas&) = delete;\n"
              "void* operator new(std::size_t bytes);\n"
              "void operator delete(void* p) noexcept;\n"
              "auto* q = new (storage) Impl();\n"
              "int new_size = 0;\n"),
      Fixture("src/gfx/Canvas.cpp", "impl_ = new Impl();\n")});

  fixtures.push_back(RuleFixture{
      "NoManualHeapOwnership",
      Fixture("src/gfx/Canvas.cpp", "buffer_.resize(count);\n"),
      Fixture("src/gfx/Canvas.cpp", "delete[] rows_;\n")});

  fixtures.push_back(RuleFixture{
      "NoManualHeapOwnership",
      Fixture("src/gfx/Canvas.cpp", "pool_.Free(block);\n"),
      Fixture("src/gfx/Canvas.cpp", "void* p = std::malloc(length);\n")});

  fixtures.push_back(RuleFixture{
      "FetchRequiresAVerdict",
      SourceSet{SourceFile{"src/net/Fetch.h",
                           "FetchResult Fetch(privacy::Verdict verdict, CookieJar& jar);\n"}},
      // By const reference, which lets a caller keep a verdict and reuse it for
      // a request it was never issued for.
      SourceSet{SourceFile{"src/net/Fetch.h",
                           "FetchResult Fetch(const privacy::Verdict& v, CookieJar& jar);\n"}}});

  fixtures.push_back(RuleFixture{
      "FetchRequiresAVerdict",
      SourceSet{SourceFile{"src/net/Fetch.h",
                           "FetchResult Fetch(privacy::Verdict verdict, CookieJar& jar);\n"}},
      // A net module with no verdict-taking Fetch at all. Without this fixture
      // the rule would pass on a tree where somebody deleted the parameter.
      SourceSet{SourceFile{"src/net/Fetch.h", "FetchResult Fetch(const Url& url);\n"}}});

  fixtures.push_back(RuleFixture{
      "StorageLookupsArePartitioned",
      SourceSet{SourceFile{"src/net/CookieJar.h",
                           "class CookieJar {\n"
                           " public:\n"
                           "  std::string HeaderFor(const url::PartitionKey& key,\n"
                           "                        const url::Url& url) const;\n"
                           "};\n"}},
      SourceSet{SourceFile{"src/net/CookieJar.h",
                           "class CookieJar {\n"
                           " public:\n"
                           "  std::string HeaderFor(const url::Url& url) const;\n"
                           "};\n"}}});

  fixtures.push_back(RuleFixture{
      "StorageLookupsArePartitioned",
      SourceSet{SourceFile{"src/net/HttpCache.h",
                           "class HttpCache {\n"
                           " public:\n"
                           "  const Entry* Lookup(const url::PartitionKey& key,\n"
                           "                      const url::Url& url) const;\n"
                           "};\n"}},
      SourceSet{SourceFile{"src/net/HttpCache.h",
                           "class HttpCache {\n"
                           " public:\n"
                           "  const Entry* Lookup(const url::Url& url) const;\n"
                           "};\n"}}});

  fixtures.push_back(RuleFixture{
      "DescriptorsAreCloseOnExec",
      Fixture("src/net/Socket.cpp", "fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);\n"),
      Fixture("src/net/Socket.cpp", "fd_ = ::socket(AF_INET, SOCK_STREAM, 0);\n")});

  fixtures.push_back(RuleFixture{
      "DescriptorsAreCloseOnExec",
      Fixture("src/platform/AppDirectories.cpp", "int fd = ::open(path, O_RDONLY | O_CLOEXEC);\n"),
      // The follow-up form: correct-looking, and it leaves a window in which a
      // fork inherits the descriptor.
      Fixture("src/net/Socket.cpp", "::fcntl(fd, F_SETFD, FD_CLOEXEC);\n")});

  fixtures.push_back(RuleFixture{
      "EnvironmentReadsAreCentralized",
      SourceSet{SourceFile{"src/util/Env.cpp", "return std::getenv(name);\n"},
                SourceFile{"src/gfx/Canvas.cpp", "const bool on = util::EnvFlagEnabled(kName);\n"}},
      SourceSet{SourceFile{"src/util/Env.cpp", "return std::getenv(name);\n"},
                SourceFile{"src/gfx/Canvas.cpp", "const char* v = std::getenv(\"HOME\");\n"}}});

  fixtures.push_back(RuleFixture{
      "EnvironmentReadsAreCentralized",
      SourceSet{SourceFile{"src/util/Env.cpp", "return std::getenv(name);\n"}},
      // The owner is gone. Nothing reads the environment, so a rule without the
      // vacuity check would report success while checking nothing.
      SourceSet{SourceFile{"src/gfx/Canvas.cpp", "// nothing reads the environment here\n"}}});

  fixtures.push_back(RuleFixture{
      "NoNamespaceScopeMutableState",
      Fixture("src/gfx/Canvas.cpp", "static constexpr int kLimit = 4;\n"),
      Fixture("src/gfx/Canvas.cpp", "static int g_counter;\n")});

  fixtures.push_back(RuleFixture{
      "HeadersUsePragmaOnce",
      Fixture("src/gfx/Canvas.h", "#pragma once\n"),
      Fixture("src/gfx/Canvas.h", "#ifndef CANVAS_H\n#define CANVAS_H\n#endif\n")});

  fixtures.push_back(RuleFixture{
      "ObjectSizeBudgetsArePresent",
      SourceSet{SourceFile{"src/gfx/Color.h", "static_assert(sizeof(Color) == 4, \"\");\n"},
                SourceFile{"src/gfx/Geometry.h", "static_assert(sizeof(IntRect) == 16, \"\");\n"},
                SourceFile{"src/gfx/DisplayList.h",
                           "static_assert(sizeof(DisplayCommand) <= 24, \"\");\n"}},
      SourceSet{SourceFile{"src/gfx/Color.h", "// budget removed\n"},
                SourceFile{"src/gfx/Geometry.h", "static_assert(sizeof(IntRect) == 16, \"\");\n"},
                SourceFile{"src/gfx/DisplayList.h",
                           "static_assert(sizeof(DisplayCommand) <= 24, \"\");\n"}}});

  return fixtures;
}

std::string Describe(const std::vector<Violation>& violations) {
  std::ostringstream out;
  for (const Violation& violation : violations) {
    out << "\n  " << violation.file;
    if (violation.line > 0) {
      out << ':' << violation.line;
    }
    out << ": " << violation.message;
  }
  return out.str();
}

const architecture::Rule* FindRule(std::string_view name) {
  for (const architecture::Rule& rule : kRules) {
    if (rule.name == name) {
      return &rule;
    }
  }
  return nullptr;
}

}  // namespace

void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests) {
  // Every module has a well-formed manifest. Runs first because every other
  // rule reads them.
  AddTest(tests, "ArchitectureInvariants/ModuleManifestsAreWellFormed", [] {
    std::vector<Violation> errors;
    const ModuleManifests manifests = architecture::LoadModuleManifests(SourceRoot(), errors);
    Expect(errors.empty(), "manifest problems:" + Describe(errors));
    Expect(!manifests.empty(), "no modules found under src/");
  });

  // One ctest case per rule, so the suite shards across cores and a failure
  // names the rule that failed rather than "the architecture test".
  for (const architecture::Rule& rule : kRules) {
    const std::string name = "ArchitectureInvariants/" + rule.name;
    AddTest(tests, name, [&rule] {
      std::vector<Violation> manifest_errors;
      const ModuleManifests manifests =
          architecture::LoadModuleManifests(SourceRoot(), manifest_errors);
      const architecture::SourceSet files = architecture::LoadSourceTree(SourceRoot());
      Expect(!files.empty(), "no source files found under src/");

      const std::vector<Violation> violations = rule.check(files, manifests);
      Expect(violations.empty(), rule.name + " violations:" + Describe(violations));
    });
  }

  // The controls. Without these, a rule whose pattern matches nothing passes
  // forever and reports success while checking nothing at all.
  AddTest(tests, "ArchitectureInvariants/RuleControlFixtures", [] {
    const ModuleManifests manifests = FixtureManifests();
    const std::vector<RuleFixture> fixtures = BuildFixtures();
    Expect(!fixtures.empty(), "no control fixtures defined");

    std::set<std::string> covered;
    for (const RuleFixture& fixture : fixtures) {
      const architecture::Rule* rule = FindRule(fixture.rule);
      Expect(rule != nullptr, "fixture names unknown rule '" + std::string(fixture.rule) + "'");
      covered.insert(std::string(fixture.rule));

      const std::vector<Violation> clean = rule->check(fixture.clean, manifests);
      Expect(clean.empty(),
             std::string(fixture.rule) + " flagged its clean fixture:" + Describe(clean));

      const std::vector<Violation> dirty = rule->check(fixture.dirty, manifests);
      Expect(!dirty.empty(), std::string(fixture.rule) +
                                 " did not flag its dirty fixture; the rule cannot fail, which "
                                 "means it is not checking anything");
    }

    // Every rule must have controls, or a new rule can be added without ever
    // being shown to work.
    for (const architecture::Rule& rule : kRules) {
      Expect(covered.count(rule.name) != 0,
             "rule '" + rule.name + "' has no control fixture; add a clean and a dirty case");
    }
  });
}

}  // namespace microbrowser::tests
