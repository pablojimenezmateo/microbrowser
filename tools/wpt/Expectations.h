#pragma once

#include <map>
#include <string>
#include <vector>

namespace microbrowser::wpt {

// What a test is expected to do today.
//
// The default is PASS, everywhere, and only deviations are written down. That
// is the whole design: an expectation file shrinks as the browser improves, so
// the diff of a session that fixed something is a *deletion*, and a file that
// grows is a regression somebody has to have chosen. The alternative -- listing
// every subtest with its status -- makes a 40,000-line file whose diffs nobody
// reads.
struct TestExpectation {
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
