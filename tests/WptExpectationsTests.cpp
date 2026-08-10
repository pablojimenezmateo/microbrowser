#include <string>
#include <vector>

#include "TestSupport.h"
#include "wpt/Expectations.h"

// The expectation file's round trip.
//
// This exists because it did not, and the cost was a session: a subtest name
// that could not survive being written and read back produced
// `FAIL (expected PASS)` beside `MISSING (expected FAIL)` for the *same*
// subtest, against the same binary that recorded it. That reads exactly like a
// regression, and ADR 0040 §5 exists to stop expectation files disagreeing with
// themselves.
//
// A name is a string the page chose. The three that matter are a trailing
// space -- which `test(function(){...})` with no name gets from a `<title>`
// written with spaces inside its tags -- an embedded newline, and a literal
// backslash, which testharness puts in names itself when it formats a control
// character.

namespace microbrowser::tests {

namespace {

// Writes one expectation, reads it back, and answers whether the name survived.
bool RoundTrips(const std::string& name) {
  TemporaryDirectory directory;
  wpt::TestExpectation expectation;
  expectation.subtests[name] = "FAIL";
  {
    wpt::ExpectationStore store;
    store.Set("area/test.html", expectation);
    std::string error;
    if (!store.Save(directory.Path().string(), &error)) {
      return false;
    }
  }
  wpt::ExpectationStore reloaded;
  reloaded.Load(directory.Path().string());
  const wpt::TestExpectation* found = reloaded.Find("area/test.html");
  if (found == nullptr || found->subtests.size() != 1) {
    return false;
  }
  return found->subtests.begin()->first == name && found->subtests.begin()->second == "FAIL";
}

}  // namespace

void RegisterWptExpectationsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptExpectations/SubtestNamesSurviveTheRoundTrip", [] {
    Expect(RoundTrips("an ordinary name"), "an ordinary name");
    Expect(RoundTrips(" Calling stopPropagation() prior to dispatchEvent() "),
           "a name with a leading and a trailing space");
    Expect(RoundTrips("Invalid attribute name: x\n (source: xml-dom)"),
           "a name with a newline in it");
    Expect(RoundTrips("Blob with type \"\\timage/gif\\t\""),
           "a name holding the literal backslashes testharness writes");
    Expect(RoundTrips("ends with a tab\t"), "a name ending in a tab");
    Expect(RoundTrips("\"quoted\""), "a name that begins and ends with a quote");
  });

  AddTest(tests, "WptExpectations/OnlyNamesThatNeedItAreEscaped", [] {
    // The escape marker is on the *key*, and it appears only when the raw form
    // would not survive. That is what keeps a format change from rewriting
    // thousands of already-recorded names -- which would invalidate every
    // expectation file not re-recorded on the same commit.
    TemporaryDirectory directory;
    wpt::TestExpectation expectation;
    expectation.subtests["plain name"] = "FAIL";
    expectation.subtests["trailing space "] = "FAIL";
    wpt::ExpectationStore store;
    store.Set("area/test.html", expectation);
    std::string error;
    Expect(store.Save(directory.Path().string(), &error), "the store saved");
    const std::string written = ReadFile(directory.Path() / "area.txt");
    Expect(written.find("FAIL=plain name\n") != std::string::npos,
           "a name that needs nothing is written exactly as before");
    Expect(written.find("FAIL:esc=trailing space \n") != std::string::npos,
           "a name that needs escaping is marked on the key");
  });
}

}  // namespace microbrowser::tests
