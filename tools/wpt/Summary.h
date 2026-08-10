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

  // Writes the markdown report. `revision` is the pinned WPT commit, so a
  // number in this file can be traced to the tests that produced it.
  bool Write(const std::string& path, const std::string& revision, std::string* error) const;

 private:
  struct Area {
    std::size_t tests = 0;
    std::map<std::string, std::size_t> harness_counts;
    std::size_t subtests_total = 0;
    std::size_t subtests_passed = 0;
  };
  struct Cause {
    std::size_t tests = 0;
    std::size_t subtests = 0;
    std::string example;
  };

  std::map<std::string, Area> areas_;
  std::map<std::string, Cause> subtest_causes_;
  std::map<std::string, Cause> harness_causes_;
  std::size_t tests_ = 0;
  std::size_t subtests_total_ = 0;
  std::size_t subtests_passed_ = 0;
};

}  // namespace microbrowser::wpt
