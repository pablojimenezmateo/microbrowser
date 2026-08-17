#include <string>
#include <vector>

#include "TestSupport.h"
#include "wpt/Summary.h"

// The summary state file's round trip, and the guard that stops a partial run
// deleting the document.
//
// `--summary-state` is the memory of a sharded baseline: the whole suite does
// not fit in one run, so each invocation measures some areas, loads what the
// previous ones measured, and writes the union back. Everything
// `docs/wpt-baseline.md` says about an area a run did not touch comes from this
// file and from nowhere else -- which makes it the one artefact in the tool
// whose *fidelity* is the measurement. It had no test, and the cost was three
// sessions: one where the state file wrote nothing at all, and two where the
// document was regenerated down to the areas one shard had run.
//
// Written against the accumulator rather than through a suite run, for the same
// reason `Handlers.cpp` and `Reftest.cpp` are in this binary: the alternative
// way to check the state file is to take a baseline, which is hours.

namespace microbrowser::tests {

namespace {

wpt::SummaryResult Result(std::string url_path, std::string harness,
                          std::vector<std::string> failures, std::size_t total,
                          std::size_t passed) {
  wpt::SummaryResult result;
  result.url_path = std::move(url_path);
  result.harness = std::move(harness);
  result.subtests_total = total;
  result.subtests_passed = passed;
  result.failure_messages = std::move(failures);
  return result;
}

// The `## Per area` rows of a written document, in order.
std::vector<std::string> PerAreaRows(const std::filesystem::path& path) {
  std::vector<std::string> rows;
  bool in_per_area = false;
  const std::string text = ReadFile(path);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end == std::string::npos ? end : end - start);
    if (line.rfind("## ", 0) == 0) {
      in_per_area = line.rfind("## Per area", 0) == 0;
    } else if (in_per_area && line.rfind("| `", 0) == 0) {
      rows.push_back(line);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return rows;
}

}  // namespace

void RegisterWptSummaryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptSummary/AnAreaSurvivesTheStateRoundTrip", [] {
    TemporaryDirectory directory;
    const std::string state = (directory.Path() / "state.tsv").string();
    {
      wpt::SummaryAccumulator first;
      first.Add(Result("dom/nodes/a.html", "OK", {"assert_equals: expected 1 but got 2"}, 4, 3));
      first.Add(Result("dom/nodes/b.html", "TIMEOUT", {}, 0, 0));
      std::string error;
      Expect(first.SaveState(state, &error), "the first shard's state is written");
    }
    // A second shard measuring a different area must keep the first's counts.
    wpt::SummaryAccumulator second;
    second.Add(Result("url/a.html", "OK", {}, 10, 10));
    Expect(second.LoadState(state), "the state file loads");
    const std::string document = (directory.Path() / "baseline.md").string();
    std::string error;
    Expect(second.Write(document, "abc123", &error), error.empty() ? "written" : error);
    const std::vector<std::string> rows = PerAreaRows(document);
    ExpectEqInt(static_cast<long long>(rows.size()), 2, "both areas are in the table");
    // `dom/nodes` came back from the file: two tests, one OK, one TIMEOUT, 4
    // subtests of which 3 passed. Every one of those numbers is a field of the
    // state format, and a shifted field would still produce a plausible table.
    ExpectEqString(rows[0], "| `dom/nodes` | 2 | 1 | 0 | 1 | 0 | 4 | 3 | 75.0 |",
                   "the loaded area's row is intact");
    ExpectEqString(rows[1], "| `url` | 1 | 1 | 0 | 0 | 0 | 10 | 10 | 100.0 |",
                   "and this run's area is beside it");
  });

  AddTest(tests, "WptSummary/AReRunReplacesItsOwnAreaRatherThanAddingToIt", [] {
    TemporaryDirectory directory;
    const std::string state = (directory.Path() / "state.tsv").string();
    {
      wpt::SummaryAccumulator first;
      first.Add(Result("dom/nodes/a.html", "OK", {"assert_equals: expected 1 but got 2"}, 4, 1));
      std::string error;
      Expect(first.SaveState(state, &error), "the first measurement is written");
    }
    wpt::SummaryAccumulator second;
    second.Add(Result("dom/nodes/a.html", "OK", {}, 4, 4));
    Expect(second.LoadState(state), "the state file loads");
    const std::string document = (directory.Path() / "baseline.md").string();
    std::string error;
    Expect(second.Write(document, "abc123", &error), error.empty() ? "written" : error);
    const std::vector<std::string> rows = PerAreaRows(document);
    ExpectEqInt(static_cast<long long>(rows.size()), 1, "one area, measured twice");
    // 1 test and 4 of 4, not 2 tests and 5 of 8. An area is the unit of
    // replacement; a fix that made subtests pass must not leave the failures
    // behind, and the ranked cause it used to carry must be gone too.
    ExpectEqString(rows[0], "| `dom/nodes` | 1 | 1 | 0 | 0 | 0 | 4 | 4 | 100.0 |",
                   "the fresh measurement replaced the stale one");
    Expect(ReadFile(document).find("expected N but got N") == std::string::npos,
           "and the cause it used to be filed under is gone");
  });

  AddTest(tests, "WptSummary/WritingRefusesToDescribeFewerAreasThanTheDocumentDoes", [] {
    TemporaryDirectory directory;
    const std::string document = (directory.Path() / "baseline.md").string();
    {
      wpt::SummaryAccumulator full;
      full.Add(Result("dom/nodes/a.html", "OK", {}, 1, 1));
      full.Add(Result("url/a.html", "OK", {}, 1, 1));
      std::string error;
      Expect(full.Write(document, "abc123", &error), error.empty() ? "written" : error);
    }
    // One area, no state: this is the shape that silently turned a 297-row
    // table into a 61-row one, three sessions running.
    wpt::SummaryAccumulator partial;
    partial.Add(Result("url/a.html", "OK", {}, 1, 1));
    std::string error;
    Expect(!partial.Write(document, "abc123", &error), "a narrower run is refused");
    Expect(error.find("already describes 2 areas") != std::string::npos,
           "and the refusal says how many rows it would have deleted: " + error);
    ExpectEqInt(static_cast<long long>(PerAreaRows(document).size()), 2,
                "the document still has both rows");
  });

  AddTest(tests, "WptSummary/TheGuardCountsThePerAreaTableAndNotAHandMergedOne", [] {
    // `docs/wpt-baseline.md` is generated and says so, and three sessions still
    // merged a "re-measured today, trust these rows" table into its preamble --
    // which is the honest thing to do when there is no state file covering the
    // rest. Those rows begin `| ` too. Counting them made the guard refuse a
    // run that covered *more* areas than the table had rows, which is the guard
    // firing on its own document.
    TemporaryDirectory directory;
    const std::string document = (directory.Path() / "baseline.md").string();
    {
      wpt::SummaryAccumulator full;
      full.Add(Result("dom/nodes/a.html", "OK", {}, 1, 1));
      full.Add(Result("url/a.html", "OK", {}, 1, 1));
      std::string error;
      Expect(full.Write(document, "abc123", &error), error.empty() ? "written" : error);
    }
    const std::string annotated =
        "# The WPT baseline\n\n"
        "**Two areas re-measured today. Trust these over the table below.**\n\n"
        "| area | before | after |\n|---|--:|--:|\n"
        "| `url/` | 21.7% | 97.9% |\n"
        "| `dom/` | 38,589 | 38,596 |\n\n" +
        ReadFile(document);
    WriteFile(document, annotated);
    wpt::SummaryAccumulator same;
    same.Add(Result("dom/nodes/a.html", "OK", {}, 1, 1));
    same.Add(Result("url/a.html", "OK", {}, 1, 1));
    std::string error;
    Expect(same.Write(document, "abc123", &error),
           "a run covering every area is written, hand-merged rows or not: " + error);
    ExpectEqInt(static_cast<long long>(PerAreaRows(document).size()), 2,
                "and the regenerated table has its two rows");
  });
}

}  // namespace microbrowser::tests
