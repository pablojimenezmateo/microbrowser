#include "wpt/Summary.h"

#include <algorithm>
#include <cstdio>
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

std::vector<std::pair<std::string, std::size_t>> RankedCauses(
    const std::map<std::string, std::size_t>& counts) {
  std::vector<std::pair<std::string, std::size_t>> ranked(counts.begin(), counts.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
    return left.second != right.second ? left.second > right.second : left.first < right.first;
  });
  return ranked;
}

}  // namespace

void SummaryAccumulator::Add(const SummaryResult& result) {
  Area& area = areas_[AreaOf(result.url_path)];
  ++area.tests;
  ++area.harness_counts[result.harness];
  area.subtests_total += result.subtests_total;
  area.subtests_passed += result.subtests_passed;

  ++tests_;
  subtests_total_ += result.subtests_total;
  subtests_passed_ += result.subtests_passed;

  if (result.harness != "OK") {
    Cause& cause = harness_causes_[result.harness + ": " + Normalize(result.harness_message)];
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
    Cause& cause = subtest_causes_[key];
    ++cause.subtests;
    if (seen_here.insert(key).second) {
      ++cause.tests;
      if (cause.example.empty()) {
        cause.example = result.url_path;
      }
    }
  }
}

bool SummaryAccumulator::Write(const std::string& path, const std::string& revision,
                               std::string* error) const {
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    if (error != nullptr) {
      *error = "could not write " + path;
    }
    return false;
  }

  stream << "# The WPT baseline\n\n"
         << "**Generated**, by `microbrowser_wpt --summary " << path << "`. Do not edit it: the\n"
            "next run overwrites it, and that overwrite is the point -- the diff of this file is\n"
            "what a session moved. The argument for the instrument is `docs/adr/0040`; the work it\n"
            "sequences is `docs/wpt-plan.md`.\n\n"
         << "WPT revision: `" << revision << "`\n\n"
         << "**" << subtests_passed_ << " of " << subtests_total_ << " subtests pass ("
         << std::string() << "";
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f%%", Percent(subtests_passed_, subtests_total_));
  stream << buffer << ")** over " << tests_ << " tests.\n\n";

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
    std::snprintf(buffer, sizeof(buffer), "%.1f", Percent(area.subtests_passed, area.subtests_total));
    stream << "| `" << name << "` | " << area.tests << " | " << count("OK") << " | "
           << count("ERROR") << " | " << count("TIMEOUT") << " | " << count("CRASH") << " | "
           << area.subtests_total << " | " << area.subtests_passed << " | " << buffer << " |\n";
  }

  // Ranked by tests affected, and truncated: the tail of a cause list is one
  // test each and reading it is how a session spends an afternoon on one
  // subtest. What is worth acting on is at the top.
  constexpr std::size_t kCauseLimit = 80;

  stream << "\n## Why the harness never reported\n\n"
            "Ranked by tests affected. One line here is worth more than a page of the table\n"
            "above: a test whose harness failed reports *no* subtests, so these are invisible in\n"
            "the pass rate and are the largest block of unrealised coverage in the suite.\n\n";
  stream << "| tests | cause | example |\n|--:|---|---|\n";
  {
    std::map<std::string, std::size_t> counts;
    for (const auto& [key, cause] : harness_causes_) {
      counts[key] = cause.tests;
    }
    std::size_t written = 0;
    for (const auto& [key, tests] : RankedCauses(counts)) {
      if (written++ >= kCauseLimit) {
        break;
      }
      stream << "| " << tests << " | " << EscapeForTable(key) << " | `"
             << harness_causes_.at(key).example << "` |\n";
    }
  }

  stream << "\n## Why subtests fail\n\n"
            "Ranked by *distinct tests* affected rather than by subtests, because that is the\n"
            "number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because\n"
            "`expected \"block\" but got \"inline\"` and `expected \"Npx\" but got \"Npx\"` are\n"
            "different bugs and a bucket labelled `assert_equals` is not actionable.\n\n";
  stream << "| tests | subtests | message | example |\n|--:|--:|---|---|\n";
  {
    std::map<std::string, std::size_t> counts;
    for (const auto& [key, cause] : subtest_causes_) {
      counts[key] = cause.tests;
    }
    std::size_t written = 0;
    for (const auto& [key, tests] : RankedCauses(counts)) {
      if (written++ >= kCauseLimit) {
        break;
      }
      const Cause& cause = subtest_causes_.at(key);
      stream << "| " << tests << " | " << cause.subtests << " | " << EscapeForTable(key) << " | `"
             << cause.example << "` |\n";
    }
  }
  stream << "\n" << subtest_causes_.size() << " distinct subtest messages and "
         << harness_causes_.size() << " distinct harness messages in this run.\n";
  return static_cast<bool>(stream);
}

}  // namespace microbrowser::wpt
