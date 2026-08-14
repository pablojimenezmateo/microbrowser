// Served in place of web-platform-tests' own /resources/testdriver-vendor.js.
//
// Upstream's version is empty: it is the hook a vendor fills in, and `wptrunner`
// fills it with WebDriver calls. This browser has no WebDriver and will not grow
// one -- ADR 0040 §3 says why, and the reason is not laziness: a WebDriver server
// is an unauthenticated remote-control surface built into the browser, with no
// user behind it.
//
// What this browser does have is an input path. `microbrowser_snapshot -click`
// already drives it, and the runner drives the same `ipc::PointerInputMessage`
// and `ipc::KeyInputMessage` a real click and a real keystroke arrive on. This
// file is the seam between the two, and it has the same shape as the report seam
// beside it: a **queue on a global**, drained by the runner through
// `engine::Engine::EvaluateScript`. No new IPC message, no new binding, and
// nothing in `src/` knows the tests exist.
//
// **Why it has to be real input and not a synthetic event.** ADR 0017 makes an
// event a page dispatched itself untrusted by construction: a synthetic click
// runs the page's handlers and *does not* follow a link, focus an element, or
// submit a form. A `test_driver.click()` implemented with `element.click()`
// would therefore pass the half of each test that counts handler calls and fail
// the half that checks what the click *did* -- which is the half worth testing.
//
// The protocol is line-oriented for the reason the report is: the parser is in
// C++, and a hand-written JSON parser inside a test tool is a place for a bug to
// live that nobody would ever look for. One command per line, tab-separated:
//
//   <id><TAB>click<TAB><x><TAB><y>
//   <id><TAB>keys<TAB><text>
//   <id><TAB>pointerMove<TAB><x><TAB><y>
//   <id><TAB>pointerDown<TAB><button>
//   <id><TAB>pointerUp<TAB><button>
//   <id><TAB>keyDown<TAB><key>
//   <id><TAB>keyUp<TAB><key>
//   <id><TAB>pause<TAB><milliseconds>
//
// The runner takes them with `__wpt_driver_take()` and answers each with
// `__wpt_driver_settle(id, ok)`.

(function () {
  "use strict";

  var queue = [];
  var pending = Object.create(null);
  var nextId = 0;

  function command(line) {
    var id = ++nextId;
    queue.push(id + "\t" + line);
    return new Promise(function (resolve, reject) {
      pending[id] = { resolve: resolve, reject: reject };
    });
  }

  self.__wpt_driver_take = function () {
    var taken = queue.join("\n");
    queue = [];
    return taken;
  };

  self.__wpt_driver_settle = function (id, ok) {
    var entry = pending[id];
    if (!entry) {
      return;
    }
    delete pending[id];
    if (ok) {
      entry.resolve(null);
    } else {
      entry.reject(new Error("testdriver command failed"));
    }
  };

  // The number of commands waiting, so the runner can ask one cheap question per
  // poll instead of taking and re-serialising an empty queue.
  self.__wpt_driver_pending = function () {
    return queue.length;
  };

  // A keystroke's three fields, from the one character WebDriver's `send_keys`
  // deals in. `code` is the physical key and `key` is what it means; for a
  // printable character the two differ and the engine reads both.
  //
  // The WebDriver spec assigns U+E000..U+F8FF to special keys; the ones this
  // covers are the ones the suite actually sends.
  var SPECIAL = {
    "\uE003": "Backspace",
    "\uE004": "Tab",
    "\uE006": "Enter",
    "\uE007": "Enter",
    "\uE008": "Shift",
    "\uE009": "Control",
    "\uE00A": "Alt",
    "\uE00C": "Escape",
    "\uE00D": " ",
    "\uE00E": "PageUp",
    "\uE00F": "PageDown",
    "\uE010": "End",
    "\uE011": "Home",
    "\uE012": "ArrowLeft",
    "\uE013": "ArrowUp",
    "\uE014": "ArrowRight",
    "\uE015": "ArrowDown",
    "\uE017": "Delete"
  };

  function keyName(character) {
    return SPECIAL[character] || character;
  }

  self.test_driver_internal = {
    // Upstream reads this to decide whether it is being driven. Saying true is
    // what turns "not implemented by testdriver-vendor.js" into a real command.
    in_automation: true,

    click: function (element, coords) {
      return command("click\t" + coords.x + "\t" + coords.y);
    },

    send_keys: function (element, keys) {
      // Focused from the page rather than by a synthetic click, because a click
      // at the element's centre is a *different* action -- it would also select
      // text, move the caret, and fire pointer events the test did not ask for.
      // WebDriver's own `Element Send Keys` focuses first for the same reason.
      if (element && typeof element.focus === "function") {
        element.focus();
      }
      var chain = Promise.resolve();
      for (var i = 0; i < keys.length; i++) {
        (function (character) {
          chain = chain.then(function () {
            return command("keys\t" + keyName(character));
          });
        })(keys[i]);
      }
      return chain;
    },

    action_sequence: function (actions) {
      // The subset this browser can perform: a pointer that moves, presses and
      // releases, a key that goes down and up, and a pause. Anything else --
      // wheel, touch with more than one pointer, `origin: element` on a source
      // this file cannot resolve -- is **rejected** rather than approximated, so
      // a test that needs it fails visibly. That is the same rule the handler
      // list follows.
      var chain = Promise.resolve();
      var origin = { x: 0, y: 0 };
      for (var i = 0; i < actions.length; i++) {
        var source = actions[i];
        if (source.type !== "pointer" && source.type !== "key" && source.type !== "none") {
          return Promise.reject(new Error("unsupported action source: " + source.type));
        }
        var steps = source.actions || [];
        for (var j = 0; j < steps.length; j++) {
          (function (step) {
            chain = chain.then(function () {
              if (step.type === "pause") {
                return command("pause\t" + (step.duration || 0));
              }
              if (step.type === "pointerMove") {
                var x = step.x || 0;
                var y = step.y || 0;
                if (step.origin && typeof step.origin === "object" &&
                    typeof step.origin.getBoundingClientRect === "function") {
                  var rect = step.origin.getBoundingClientRect();
                  x += rect.left + rect.width / 2;
                  y += rect.top + rect.height / 2;
                } else if (step.origin === "pointer") {
                  x += origin.x;
                  y += origin.y;
                }
                origin = { x: x, y: y };
                return command("pointerMove\t" + x + "\t" + y);
              }
              if (step.type === "pointerDown") {
                return command("pointerDown\t" + (step.button || 0));
              }
              if (step.type === "pointerUp") {
                return command("pointerUp\t" + (step.button || 0));
              }
              if (step.type === "keyDown") {
                return command("keyDown\t" + keyName(step.value));
              }
              if (step.type === "keyUp") {
                return command("keyUp\t" + keyName(step.value));
              }
              return Promise.reject(new Error("unsupported action: " + step.type));
            });
          })(steps[j]);
        }
      }
      return chain;
    }
  };
})();
