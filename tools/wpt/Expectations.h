#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::wpt {

// Replaces this run's port numbers in a subtest name with the *index* of the port
// they refer to: `http://localhost:40289/x` becomes `http://localhost:{{port[0]}}/x`.
//
// **A subtest name is a string the page chose, and a great many pages build one out
// of their own origin.** The server binds ephemeral ports -- it asks the kernel for
// a free one, because a fixed port makes two runs on one machine collide -- so those
// names differ on every run, and an expectation file full of them churns entirely
// for free. Measured on 2026-08-12: re-recording `fetch/` moved 292 lines of which
// **~250 were nothing but a port number changing**, which is worse than useless. The
// diff of these files is what a session delivers, and a diff nobody can read is a
// deliverable nobody can check.
//
// **By index rather than to a single placeholder, and that distinction is the whole
// of the design.** `fetch/api/basic/scheme-others.any.html` has one subtest per
// origin, so two names that differ only in their port are two *different* subtests
// -- one same-origin and one cross-origin. Folding both to `localhost:PORT` would
// collapse them onto one key and silently drop one of the two, which is a worse
// failure than the churn: it removes a real result instead of a fake change.
//
// Applied to a name arriving from the page, so that recording and comparing use the
// same key and nothing downstream has to remember. Names that contain no port are
// returned unchanged, which is almost all of them.
std::string NormalizePortsInName(std::string_view name,
                                 const std::vector<std::uint16_t>& ports);

// What a test is expected to do today.
//
// The default is PASS, everywhere, and only deviations are written down. That
// is the whole design: an expectation file shrinks as the browser improves, so
// the diff of a session that fixed something is a *deletion*, and a file that
// grows is a regression somebody has to have chosen. The alternative -- listing
// every subtest with its status -- makes a 40,000-line file whose diffs nobody
// reads.
struct TestExpectation {
  // The `#` comment lines written immediately above this test's `[path]` line,
  // verbatim and in order.
  //
  // Kept because `tests/wpt/expectations/README.md` *requires* one on every
  // deliberate refusal -- "use one whenever a line records a deliberate
  // deviation rather than a bug, and name the ADR" -- and until this field
  // existed the writer dropped every one of them. A rule the tool silently
  // undoes is not a rule: a session would write down why a test may never pass,
  // and the next `--update-expectations` anywhere in the same file would delete
  // it. They belong to the test rather than to the file so that re-recording
  // one area cannot lose another's.
  std::vector<std::string> comments;
  // Harness status: OK (default), ERROR, TIMEOUT, PRECONDITION_FAILED, CRASH.
  std::string harness = "OK";
  // Subtest name -> expected status, for subtests that are not PASS.
  std::map<std::string, std::string> subtests;
  // Set for a test that must not be run at all. A test that hangs the process
  // or takes a minute is worth excluding *loudly* -- the reason is required.
  std::string disabled_reason;
  bool disabled = false;
};

class ExpectationStore {
 public:
  // Loads every `<name>.txt` under `directory`. A missing directory is not an
  // error: it means nothing is expected to fail yet.
  void Load(const std::string& directory);

  // The expectation for a test path, or the all-PASS default.
  const TestExpectation* Find(const std::string& url_path) const;

  // Replaces (or removes, when everything passed) one test's expectation.
  void Set(const std::string& url_path, TestExpectation expectation);

  // Writes the store back, one file per top-level directory, sorted. Only
  // files whose contents changed are rewritten.
  bool Save(const std::string& directory, std::string* error) const;

  std::size_t Size() const { return tests_.size(); }

 private:
  std::map<std::string, TestExpectation> tests_;
};

}  // namespace microbrowser::wpt
