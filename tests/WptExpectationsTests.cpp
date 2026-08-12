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

  AddTest(tests, "WptExpectations/APortInASubtestNameBecomesItsIndex", [] {
    // A subtest name is a string the *page* chose, and a great many pages build one
    // out of their own origin -- so an ephemeral port lands in the expectation file
    // and churns the whole file on every run. Measured: re-recording `fetch/` moved
    // 292 lines of which ~250 were nothing but a port number changing.
    const std::vector<std::uint16_t> ports{40289, 46853};
    ExpectEqString(
        wpt::NormalizePortsInName("Fetch http://localhost:40289/x with no-cors mode", ports),
        "Fetch http://localhost:{{port[0]}}/x with no-cors mode", "the first port becomes index 0");

    // **The reason it is an index and not one placeholder.** These two are two
    // *different* subtests -- one same-origin, one cross-origin -- and folding both to
    // `localhost:PORT` would collapse them onto one key and silently drop a real
    // result, which is a worse failure than the churn it fixes.
    const std::string first = wpt::NormalizePortsInName("Fetch http://localhost:40289/x", ports);
    const std::string second = wpt::NormalizePortsInName("Fetch http://localhost:46853/x", ports);
    Expect(first != second, "two origins stay two distinct names: " + first + " vs " + second);
    ExpectEqString(second, "Fetch http://localhost:{{port[1]}}/x", "the second port becomes index 1");

    // A name with no port is returned unchanged, which is almost all of them.
    ExpectEqString(wpt::NormalizePortsInName("Node.appendChild() reparents", ports),
                   "Node.appendChild() reparents", "an ordinary name is untouched");
    // And so is one whose colon is not a port.
    ExpectEqString(wpt::NormalizePortsInName("data:text/html is not fetchable", ports),
                   "data:text/html is not fetchable", "a colon that is not a port is untouched");
  });

  AddTest(tests, "WptExpectations/APortIsNotMatchedInsideALongerNumber", [] {
    // Port 8000 must not rewrite the `:80001` in a name that happens to contain one:
    // the result would be a key no run ever reproduces, so the subtest would read as
    // missing on one run and new on the next, forever.
    const std::vector<std::uint16_t> ports{8000};
    ExpectEqString(wpt::NormalizePortsInName("size :80001 bytes", ports), "size :80001 bytes",
                   "a longer number is left alone");
    ExpectEqString(wpt::NormalizePortsInName("http://localhost:8000/", ports),
                   "http://localhost:{{port[0]}}/", "while the port itself is replaced");
    // Both in one name, which is the case a naive scan gets wrong in one direction or
    // the other.
    ExpectEqString(wpt::NormalizePortsInName("http://localhost:8000/ vs :80001", ports),
                   "http://localhost:{{port[0]}}/ vs :80001", "and both together");
  });

  AddTest(tests, "WptExpectations/AnUnboundPortIsNotSubstituted", [] {
    // `ports` carries a 0 for a port that was never bound -- `ServerOptions::ports`
    // uses 0 to mean "pick a free one" and `Bind` writes the choice back, so a
    // failure leaves the zero. Substituting it would rewrite every name containing
    // `:0`, which is a plain number many tests print.
    const std::vector<std::uint16_t> ports{0, 46853};
    ExpectEqString(wpt::NormalizePortsInName("offset :0 of the buffer", ports),
                   "offset :0 of the buffer", "an unbound port substitutes nothing");
    ExpectEqString(wpt::NormalizePortsInName("http://localhost:46853/", ports),
                   "http://localhost:{{port[1]}}/", "and the bound one still works");
  });

}

}  // namespace microbrowser::tests
