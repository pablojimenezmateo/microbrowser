#include "wpt/Summary.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

namespace microbrowser::wpt {
namespace {

// The area a test is counted under: two path segments where there are two.
// `css` alone is 30,000 tests and says nothing; `css/css-flexbox` is a task in
// docs/wpt-plan.md. `url/a.html` has only one segment and stays `url`.
std::string AreaOf(const std::string& url_path) {
  const std::size_t first = url_path.find('/');
  if (first == std::string::npos) {
    return url_path;
  }
  const std::size_t second = url_path.find('/', first + 1);
  if (second == std::string::npos) {
    return url_path.substr(0, first);
  }
  return url_path.substr(0, second);
}

// Two failures are the same cause when they say the same thing about different
// values. Digits carry the values -- an index, a pixel, a length -- so a run of
// them becomes `N`; whitespace is collapsed because a message built from a
// multi-line source carries its indentation.
//
// This deliberately does *not* strip quoted strings. `expected "block" but got
// "inline"` and `expected "1px" but got "0px"` are different bugs, and merging
// them produces a bucket labelled `assert_equals` that nobody can act on.
std::string Normalize(const std::string& message) {
  std::string out;
  out.reserve(message.size());
  bool in_space = false;
  bool in_digits = false;
  for (const char c : message) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      in_digits = false;
      if (!in_space && !out.empty()) {
        out.push_back(' ');
        in_space = true;
      }
      continue;
    }
    in_space = false;
    if (c >= '0' && c <= '9') {
      if (!in_digits) {
        out.push_back('N');
        in_digits = true;
      }
      continue;
    }
    in_digits = false;
    out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  constexpr std::size_t kMaxLength = 140;
  if (out.size() > kMaxLength) {
    out.resize(kMaxLength);
    out += "...";
  }
  return out;
}

std::string EscapeForTable(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '|') {
      out += "\\|";
    } else if (c == '`') {
      out += "'";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

double Percent(std::size_t part, std::size_t whole) {
  return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    fields.push_back(line.substr(
        start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
    if (tab == std::string_view::npos) {
      return fields;
    }
    start = tab + 1;
  }
}

std::size_t ParseCount(std::string_view field) {
  return static_cast<std::size_t>(std::strtoull(std::string(field).c_str(), nullptr, 10));
}

}  // namespace

void SummaryAccumulator::Add(const SummaryResult& result) {
  const std::string area_name = AreaOf(result.url_path);
  // The first result for an area in this run discards whatever a loaded state
  // said about it: a re-run replaces an area, it does not add to it.
  if (!fresh_[area_name]) {
    fresh_[area_name] = true;
    areas_[area_name] = Area{};
  }
  Area& area = areas_[area_name];
  ++area.tests;
  ++area.harness_counts[result.harness];
  area.subtests_total += result.subtests_total;
  area.subtests_passed += result.subtests_passed;

  if (result.harness != "OK") {
    Cause& cause = area.harness_causes[result.harness + ": " + Normalize(result.harness_message)];
    ++cause.tests;
    if (cause.example.empty()) {
      cause.example = result.url_path;
    }
  }
  // A test is counted once per distinct cause, so one test failing forty
  // subtests on one missing method adds one to that cause's `tests` and forty
  // to its `subtests`. The ranking is on the first.
  std::set<std::string> seen_here;
  for (const std::string& message : result.failure_messages) {
    const std::string key = Normalize(message);
    Cause& cause = area.subtest_causes[key];
    ++cause.subtests;
    if (seen_here.insert(key).second) {
      ++cause.tests;
      if (cause.example.empty()) {
        cause.example = result.url_path;
      }
    }
  }
}

bool SummaryAccumulator::LoadState(const std::string& path) {
  std::ifstream stream(path);
  if (!stream) {
    return false;
  }
  std::string line;
  std::string current;
  while (std::getline(stream, line)) {
    const std::vector<std::string_view> fields = SplitTabs(line);
    if (fields.size() < 2) {
      continue;
    }
    if (fields[0] == "A" && fields.size() >= 9) {
      current = std::string(fields[1]);
      if (fresh_[current]) {
        current.clear();  // this run measured it; the file is stale for it
        continue;
      }
      Area& area = areas_[current];
      area.tests = ParseCount(fields[2]);
      area.harness_counts["OK"] = ParseCount(fields[3]);
      area.harness_counts["ERROR"] = ParseCount(fields[4]);
      area.harness_counts["TIMEOUT"] = ParseCount(fields[5]);
      area.harness_counts["CRASH"] = ParseCount(fields[6]);
      area.subtests_total = ParseCount(fields[7]);
      area.subtests_passed = ParseCount(fields[8]);
    } else if ((fields[0] == "S" || fields[0] == "H") && fields.size() >= 5 && !current.empty()) {
      Cause cause;
      cause.tests = ParseCount(fields[1]);
      cause.subtests = ParseCount(fields[2]);
      cause.example = std::string(fields[3]);
      const std::string message(fields[4]);
      if (fields[0] == "S") {
        areas_[current].subtest_causes[message] = cause;
      } else {
        areas_[current].harness_causes[message] = cause;
      }
    }
  }
  return true;
}

bool SummaryAccumulator::SaveState(const std::string& path, std::string* error) const {
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    if (error != nullptr) {
      *error = "could not write " + path;
    }
    return false;
  }
  stream << "# microbrowser_wpt --summary-state. Generated; a run cache, not an artefact.\n"
            "# A<TAB>area<TAB>tests<TAB>ok<TAB>error<TAB>timeout<TAB>crash<TAB>subtests<TAB>passed\n"
            "# S|H<TAB>tests<TAB>subtests<TAB>example<TAB>message\n";
  for (const auto& [name, area] : areas_) {
    const auto count = [&](const char* key) {
      const auto found = area.harness_counts.find(key);
      return found == area.harness_counts.end() ? std::size_t{0} : found->second;
    };
    stream << "A\t" << name << '\t' << area.tests << '\t' << count("OK") << '\t' << count("ERROR")
           << '\t' << count("TIMEOUT") << '\t' << count("CRASH") << '\t' << area.subtests_total
           << '\t' << area.subtests_passed << '\n';
    for (const auto& [message, cause] : area.subtest_causes) {
      stream << "S\t" << cause.tests << '\t' << cause.subtests << '\t' << cause.example << '\t'
             << message << '\n';
    }
    for (const auto& [message, cause] : area.harness_causes) {
      stream << "H\t" << cause.tests << '\t' << cause.subtests << '\t' << cause.example << '\t'
             << message << '\n';
    }
  }
  return static_cast<bool>(stream);
}

bool SummaryAccumulator::Write(const std::string& path, const std::string& revision,
                               std::string* error) const {
  // **Refuse to describe less than the document already does.**
  //
  // This file is written from the accumulator alone, so a run whose
  // `--summary-state` does not already cover every area produces a document
  // about only the areas it ran -- complete-looking, correctly formatted, and
  // missing two hundred rows. That has now happened three times, twice caught
  // by a reader noticing and once not, and every time the state file was in
  // `/tmp` and had been left behind by a session that measured a handful of
  // areas. Counting the rows already there is the cheapest possible guard, and
  // it turns a silent truncation into a refusal that says what to do.
  if (std::ifstream existing(path); existing) {
    std::size_t rows = 0;
    std::string line;
    while (std::getline(existing, line)) {
      if (line.rfind("| `", 0) == 0) {
        ++rows;
      }
    }
    if (rows > areas_.size()) {
      if (error != nullptr) {
        *error = path + " already describes " + std::to_string(rows) + " areas and this run has " +
                 std::to_string(areas_.size()) +
                 "; pass --summary-state pointing at a state file that covers the rest, or "
                 "re-measure everything. Writing would delete the areas this run did not touch.";
      }
      return false;
    }
  }

  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    if (error != nullptr) {
      *error = "could not write " + path;
    }
    return false;
  }

  std::size_t tests = 0;
  std::size_t subtests_total = 0;
  std::size_t subtests_passed = 0;
  std::map<std::string, Cause> subtest_causes;
  std::map<std::string, Cause> harness_causes;
  const auto merge = [](std::map<std::string, Cause>& into,
                        const std::map<std::string, Cause>& from) {
    for (const auto& [message, cause] : from) {
      Cause& target = into[message];
      target.tests += cause.tests;
      target.subtests += cause.subtests;
      if (target.example.empty()) {
        target.example = cause.example;
      }
    }
  };
  const Area* largest = nullptr;
  std::string largest_name;
  for (const auto& [name, area] : areas_) {
    tests += area.tests;
    subtests_total += area.subtests_total;
    subtests_passed += area.subtests_passed;
    merge(subtest_causes, area.subtest_causes);
    merge(harness_causes, area.harness_causes);
    if (largest == nullptr || area.subtests_total > largest->subtests_total) {
      largest = &area;
      largest_name = name;
    }
  }

  char buffer[64];
  stream << "# The WPT baseline\n\n"
         << "**Generated**, by `microbrowser_wpt --summary " << path << "`. Do not edit it: the\n"
            "next run overwrites it, and that overwrite is the point -- the diff of this file is\n"
            "what a session moved. The argument for the instrument is `docs/adr/0040`; the work it\n"
            "sequences is `docs/wpt-plan.md`.\n\n"
         << "This file is written from `--summary-state` alone. **A run whose state file does\n"
            "not already describe every area would produce a document about only the areas it\n"
            "ran** -- complete-looking and wrong -- so the writer now refuses when the document\n"
            "already has more rows than the run does, and says so. A per-area re-measurement is\n"
            "therefore a hand-merge of its rows into this table until one full run has written a\n"
            "state file that covers everything (plan task B6).\n\n"
         << "WPT revision: `" << revision << "`\n\n";

  std::snprintf(buffer, sizeof(buffer), "%.1f%%", Percent(subtests_passed, subtests_total));
  stream << subtests_passed << " of " << subtests_total << " subtests pass (" << buffer << ") over "
         << tests << " tests.\n\n";

  if (largest != nullptr && subtests_total > 0) {
    std::snprintf(buffer, sizeof(buffer), "%.0f%%",
                  Percent(largest->subtests_total, subtests_total));
    stream << "**Do not quote that number.** Subtests are not comparable across areas: `"
           << largest_name << "` alone is " << buffer << " of every subtest here.\n"
           << "A suite that tests one index table entry per code point counts differently from\n"
              "one that tests an algorithm. The per-area column is the measurement; the aggregate\n"
              "is an artefact of how the suite is written.\n\n";
  }

  stream << "A test with no subtests at all -- a reftest, or a testharness page whose harness\n"
            "died before it ran anything -- contributes nothing to that percentage, so the\n"
            "harness columns below are the ones to read first. A `TIMEOUT` is not a slow test;\n"
            "it is a page that never reported, which almost always means something threw before\n"
            "`done()`.\n\n";

  stream << "## Per area\n\n";
  stream << "| area | tests | ok | error | timeout | crash | subtests | passed | % |\n";
  stream << "|---|--:|--:|--:|--:|--:|--:|--:|--:|\n";
  for (const auto& [name, area] : areas_) {
    const auto count = [&](const char* key) {
      const auto found = area.harness_counts.find(key);
      return found == area.harness_counts.end() ? std::size_t{0} : found->second;
    };
    std::snprintf(buffer, sizeof(buffer), "%.1f",
                  Percent(area.subtests_passed, area.subtests_total));
    stream << "| `" << name << "` | " << area.tests << " | " << count("OK") << " | "
           << count("ERROR") << " | " << count("TIMEOUT") << " | " << count("CRASH") << " | "
           << area.subtests_total << " | " << area.subtests_passed << " | " << buffer << " |\n";
  }

  // Ranked by tests affected, and truncated: the tail of a cause list is one
  // test each and reading it is how a session spends an afternoon on one
  // subtest. What is worth acting on is at the top.
  constexpr std::size_t kCauseLimit = 100;
  const auto ranked = [](const std::map<std::string, Cause>& causes) {
    std::vector<std::pair<std::string, Cause>> out(causes.begin(), causes.end());
    std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
      if (left.second.tests != right.second.tests) {
        return left.second.tests > right.second.tests;
      }
      return left.first < right.first;
    });
    return out;
  };

  stream << "\n## Why the harness never reported\n\n"
            "Ranked by tests affected. One line here is worth more than a page of the table\n"
            "above: a test whose harness failed reports *no* subtests, so these are invisible in\n"
            "the pass rate and are the largest block of unrealised coverage in the suite.\n\n";
  stream << "| tests | cause | example |\n|--:|---|---|\n";
  {
    std::size_t written = 0;
    for (const auto& [message, cause] : ranked(harness_causes)) {
      if (written++ >= kCauseLimit) {
        break;
      }
      stream << "| " << cause.tests << " | " << EscapeForTable(message) << " | `" << cause.example
             << "` |\n";
    }
  }

  stream << "\n## Why subtests fail\n\n"
            "Ranked by *distinct tests* affected rather than by subtests, because that is the\n"
            "number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because\n"
            "`expected \"block\" but got \"inline\"` and `expected \"Npx\" but got \"Npx\"` are\n"
            "different bugs and a bucket labelled `assert_equals` is not actionable.\n\n";
  stream << "| tests | subtests | message | example |\n|--:|--:|---|---|\n";
  {
    std::size_t written = 0;
    for (const auto& [message, cause] : ranked(subtest_causes)) {
      if (written++ >= kCauseLimit) {
        break;
      }
      stream << "| " << cause.tests << " | " << cause.subtests << " | " << EscapeForTable(message)
             << " | `" << cause.example << "` |\n";
    }
  }
  stream << "\n" << subtest_causes.size() << " distinct subtest messages and "
         << harness_causes.size() << " distinct harness messages behind these numbers.\n";
  return static_cast<bool>(stream);
}

}  // namespace microbrowser::wpt
