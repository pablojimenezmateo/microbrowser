// Served in place of web-platform-tests' own /resources/testharnessreport.js.
//
// Upstream's version talks to `wptrunner` over WebDriver. This browser has no
// WebDriver, and building one to run tests would be a second remote-control
// surface built before the first page renders. What it does have is
// `engine::Engine::EvaluateScript`, which the snapshot tool already uses -- so
// the report is left on a global and the runner reads it out of the same seam.
//
// The format is line-oriented rather than JSON on purpose: the runner parses it
// in C++, and a hand-written JSON parser in a test tool is a place for a bug to
// live that nobody would ever look for. One record per line, tab-separated,
// with \\, \t and \n escaped inside a field.
//
//   H<TAB>OK|ERROR|TIMEOUT|PRECONDITION_FAILED<TAB>message
//   T<TAB>PASS|FAIL|TIMEOUT|NOTRUN|PRECONDITION_FAILED<TAB>name<TAB>message
//
// `window.__wpt_done` is the flag the runner polls; `window.__wpt_report` is
// the string. Nothing else in this file is part of the contract.

(function () {
  "use strict";

  var HARNESS_STATUS = ["OK", "ERROR", "TIMEOUT", "PRECONDITION_FAILED"];
  var TEST_STATUS = ["PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"];

  function escapeField(value) {
    if (value === undefined || value === null) {
      return "";
    }
    return String(value)
      .replace(/\\/g, "\\\\")
      .replace(/\t/g, "\\t")
      .replace(/\r/g, "")
      .replace(/\n/g, "\\n");
  }

  // An error thrown before testharness.js finishes loading never reaches a
  // completion callback: the page simply never reports. Recording it here is
  // what turns "TIMEOUT, no output" into a message naming the line.
  self.__wpt_errors = [];
  self.addEventListener("error", function (event) {
    var message = event && event.message ? event.message : "uncaught error";
    if (event && event.filename) {
      message += " @ " + event.filename + ":" + event.lineno;
    }
    self.__wpt_errors.push(message);
  });
  self.addEventListener("unhandledrejection", function (event) {
    self.__wpt_errors.push("unhandled rejection: " + (event && event.reason));
  });

  self.__wpt_done = false;
  self.__wpt_report = "";

  if (typeof add_completion_callback !== "function") {
    // testharness.js did not load, or loaded and threw. Say so rather than
    // hanging: a runner timeout looks identical to an engine hang from outside.
    self.__wpt_report = "H\tERROR\ttestharness.js did not define add_completion_callback";
    self.__wpt_done = true;
    return;
  }

  // Rendering the results table into #log costs a DOM build and a layout per
  // test for output nobody reads -- every real runner turns it off. It also
  // has to be off for correctness here: testharness runs its completion
  // callbacks in one loop with no try/catch, so a throw inside `show_results`
  // silently eats every callback registered after it, including this file's.
  // That is not hypothetical -- it is how this harness failed on its first
  // run, because `insertAdjacentText` is not implemented and `show_results`
  // calls it. Keep this line even after that is fixed: the next missing method
  // in the output path would fail exactly the same way, and a harness that
  // reports nothing is worse than a browser that renders nothing.
  try {
    setup({ output: false });
  } catch (error) {
    self.__wpt_errors.push("setup({output:false}) threw: " + error);
  }

  add_completion_callback(function (tests, status) {
    var lines = [];
    var harness = HARNESS_STATUS[status.status] || "ERROR";
    var harnessMessage = status.message || "";
    if (harness === "OK" && self.__wpt_errors.length > 0) {
      // A page can pass every subtest and still have thrown; testharness only
      // notices an error that happens inside a test body.
      harnessMessage = self.__wpt_errors.join(" | ");
    }
    lines.push("H\t" + harness + "\t" + escapeField(harnessMessage));
    for (var index = 0; index < tests.length; index++) {
      var test = tests[index];
      lines.push(
        "T\t" +
          (TEST_STATUS[test.status] || "FAIL") +
          "\t" +
          escapeField(test.name) +
          "\t" +
          escapeField(test.message)
      );
    }
    self.__wpt_report = lines.join("\n");
    self.__wpt_done = true;
  });
})();
