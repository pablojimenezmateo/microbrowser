#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace microbrowser::wpt {

// The baseline report: where this browser is, per area, and *why*.
//
// docs/wpt-plan.md tasks B2 and B4 ask for two documents -- a per-area pass
// rate and a ranked list of causes. Both are written by the run that produced
// them rather than by hand, because a hand-written table is out of date the
// moment somebody fixes anything, and a hand-grouped cause list is one agent's
// reading of a log rather than a count.
//
// The ranking metric is **distinct tests affected**, not subtests. A cause that
// breaks one test in forty different ways is one session's work; a cause that
// breaks forty tests once each is forty tests unblocked. The second is what the
// plan means by "rank by tests-unblocked-per-fix".
//
// **It accumulates across runs, because the whole suite does not fit in one.**
// 21,265 testharness tests on this machine is most of a day, and the runner
// only writes its expectations when it finishes -- so a baseline has to be
// taken an area at a time or it is lost to the first interruption. `--summary
// FILE --summary-state FILE.tsv` loads the previous state, replaces every area
// this run covered, and re-renders the whole document. An area is the unit of
// replacement, which is why the causes are counted per area rather than
// globally: a global count could not be un-counted when its area is re-run.
struct SummaryResult {
  std::string url_path;
  // OK / ERROR / TIMEOUT / CRASH / PRECONDITION_FAILED.
  std::string harness;
  std::string harness_message;
  std::size_t subtests_total = 0;
  std::size_t subtests_passed = 0;
  // One entry per subtest that did not pass. Empty when the harness itself
  // failed, which is what the harness table below is for.
  std::vector<std::string> failure_messages;
};

class SummaryAccumulator {
 public:
  void Add(const SummaryResult& result);

  // Merges `path` into this accumulator, keeping only the areas this run has
  // not touched. A missing file is not an error: it means this is the first
  // shard. The format is the tab-separated one the harness report uses, for the
  // reason ADR 0040 §3 gives -- a hand-written JSON parser in a test tool is a
  // place for a bug to live that nobody would look for.
  bool LoadState(const std::string& path);
  bool SaveState(const std::string& path, std::string* error) const;

  // Writes the markdown report. `revision` is the pinned WPT commit, so a
  // number in this file can be traced to the tests that produced it.
  bool Write(const std::string& path, const std::string& revision, std::string* error) const;

 private:
  struct Cause {
    std::size_t tests = 0;
    std::size_t subtests = 0;
    std::string example;
  };
  struct Area {
    std::size_t tests = 0;
    std::map<std::string, std::size_t> harness_counts;
    std::size_t subtests_total = 0;
    std::size_t subtests_passed = 0;
    std::map<std::string, Cause> subtest_causes;
    std::map<std::string, Cause> harness_causes;
  };

  std::map<std::string, Area> areas_;
  // Areas this run touched, so a loaded state does not double-count them.
  std::map<std::string, bool> fresh_;
};

}  // namespace microbrowser::wpt
