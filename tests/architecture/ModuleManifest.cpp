#include "architecture/ModuleManifest.h"

#include <algorithm>
#include <optional>
#include <sstream>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::tests::architecture {

namespace {

std::vector<std::string> SplitWhitespace(std::string_view text) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < text.size() && text[index] != ' ' && text[index] != '\t') {
      ++index;
    }
    if (index > start) {
      tokens.emplace_back(text.substr(start, index - start));
    }
  }
  return tokens;
}

// `key=value` where value is a non-negative integer. Returns false on anything
// else, including a missing '=' — a typo in a budget field must fail loudly
// rather than silently leave the field at zero (which would make the budget
// unsatisfiable and look like a real violation somewhere else).
bool ParseKeyValue(std::string_view token, std::string_view& key, std::size_t& value) {
  const std::size_t equals = token.find('=');
  if (equals == std::string_view::npos) {
    return false;
  }
  key = token.substr(0, equals);
  const std::optional<std::size_t> parsed = util::ParseSize(token.substr(equals + 1));
  if (!parsed.has_value()) {
    return false;
  }
  value = *parsed;
  return true;
}

bool Contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace

bool ModuleManifest::Allows(std::string_view module) const {
  return Contains(allow, module);
}

bool ModuleManifest::Exports(std::string_view header) const {
  return Contains(publics, header);
}

bool ModuleManifest::AllowsExtern(std::string_view group) const {
  return Contains(externs, group);
}

const ClassBudget* ModuleManifest::FindBudget(std::string_view class_name) const {
  for (const ClassBudget& budget : budgets) {
    if (budget.name == class_name) {
      return &budget;
    }
  }
  return nullptr;
}

ManifestParseResult ParseModuleManifest(std::string_view module_name, std::string_view text) {
  ManifestParseResult result;
  result.manifest.name = std::string(module_name);

  bool saw_max_tu_lines = false;
  std::istringstream stream{std::string(text)};
  std::string raw_line;
  int line_number = 0;

  while (std::getline(stream, raw_line)) {
    ++line_number;
    const std::string_view line = util::TrimAscii(raw_line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      result.errors.push_back("line " + std::to_string(line_number) +
                              ": expected '<field>: <value>'");
      continue;
    }

    const std::string_view field = util::TrimAscii(line.substr(0, colon));
    const std::string_view value = util::TrimAscii(line.substr(colon + 1));

    if (field == "purpose") {
      result.manifest.purpose = std::string(value);
    } else if (field == "allow") {
      result.manifest.allow = SplitWhitespace(value);
    } else if (field == "public") {
      result.manifest.publics = SplitWhitespace(value);
    } else if (field == "extern") {
      result.manifest.externs = SplitWhitespace(value);
    } else if (field == "max_tu_lines") {
      const std::optional<std::size_t> parsed = util::ParseSize(value);
      if (!parsed.has_value() || *parsed == 0) {
        result.errors.push_back("line " + std::to_string(line_number) +
                                ": max_tu_lines must be a positive integer");
      } else {
        result.manifest.max_tu_lines = *parsed;
        saw_max_tu_lines = true;
      }
    } else if (field == "budget") {
      const std::vector<std::string> tokens = SplitWhitespace(value);
      if (tokens.empty()) {
        result.errors.push_back("line " + std::to_string(line_number) +
                                ": budget needs a class name");
        continue;
      }
      ClassBudget budget;
      budget.name = tokens.front();
      bool ok = true;
      for (std::size_t i = 1; i < tokens.size(); ++i) {
        std::string_view key;
        std::size_t parsed_value = 0;
        if (!ParseKeyValue(tokens[i], key, parsed_value)) {
          result.errors.push_back("line " + std::to_string(line_number) + ": bad budget field '" +
                                  tokens[i] + "'");
          ok = false;
          continue;
        }
        if (key == "header_lines") {
          budget.header_lines = parsed_value;
        } else if (key == "public_methods") {
          budget.public_methods = parsed_value;
        } else if (key == "members") {
          budget.members = parsed_value;
        } else {
          result.errors.push_back("line " + std::to_string(line_number) +
                                  ": unknown budget field '" + std::string(key) + "'");
          ok = false;
        }
      }
      if (ok) {
        result.manifest.budgets.push_back(std::move(budget));
      }
    } else {
      result.errors.push_back("line " + std::to_string(line_number) + ": unknown field '" +
                              std::string(field) + "'");
    }
  }

  // A manifest without a purpose is a directory nobody had to justify creating,
  // which is how a codebase grows a `common/` and then a `misc/`.
  if (result.manifest.purpose.empty()) {
    result.errors.emplace_back("missing required field 'purpose'");
  }
  if (!saw_max_tu_lines) {
    result.errors.emplace_back("missing required field 'max_tu_lines'");
  }
  if (result.manifest.publics.empty()) {
    result.errors.emplace_back("missing required field 'public' (a module with no public "
                               "surface cannot be depended on)");
  }

  return result;
}

}  // namespace microbrowser::tests::architecture
