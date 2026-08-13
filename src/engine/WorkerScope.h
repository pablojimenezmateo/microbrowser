#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "js/StructuredClone.h"
#include "js/Value.h"

namespace microbrowser::js {
class Interpreter;
}

namespace microbrowser::engine {

// A dedicated worker's global scope: the half of ADR 0022 §1 that runs *inside* the thread.
//
// `engine::Workers` owns the thread, the heap and the two queues; this owns what a script standing in
// that heap can see. The split is the one `PageScript` and `Page` already have -- one class for the
// machinery, one for the surface -- and it is what keeps `Workers.cpp` about synchronisation.
//
// **The global is not the window and does not pretend to be.** There is no `document`, no `Element`,
// no `getComputedStyle` and no path to any of them, which is ADR 0022's "it borrows nothing" made
// structural rather than promised: this file cannot reach `src/dom` because the surface it installs is
// built out of `src/js` and the host interface below, and nothing else.
//
// What the host does for it is exactly the set of things a thread with no borrows cannot do for
// itself: put bytes on the page's queue, report an uncaught error, forward a console line, and fetch.
class WorkerScopeHost {
 public:
  virtual ~WorkerScopeHost() = default;

  // `self.postMessage`. The value is already serialised, in this thread's heap, by the caller.
  virtual void PostToPage(js::SerializedValue message) = 0;

  // An uncaught error, as text. Text rather than a serialised exception: an error object crossing a
  // heap boundary would be an object graph copied for a diagnostic.
  virtual void ReportError(std::string message) = 0;

  // A `console` line, forwarded to the page's console so that a worker is debuggable at all. Without
  // it a worker's diagnostics land in an interpreter nobody ever reads.
  virtual void ReportConsole(std::string line) = 0;

  // `importScripts`, which the specification defines as **synchronous**. This blocks the worker
  // thread while the main loop performs the fetch and hands the bytes back -- which is precisely what
  // a worker is *for*: the one thread that may block on a resource is the one that is not drawing.
  // False when the fetch failed or the worker was terminated while it waited, and the caller turns
  // that into the specification's `NetworkError`.
  virtual bool FetchSync(const std::string& url, std::string* body_out) = 0;

  // `close()`. The loop stops after the current turn rather than inside it, because a script that
  // calls `close()` and then keeps running is defined to keep running to the end of its task.
  virtual void RequestClose() = 0;
};

// The components of the worker's own URL, computed on the main thread.
//
// Strings rather than a `url::Url`, and that is deliberate: the URL parser has lazily-initialised
// tables behind it (IDNA, the public suffix list), so parsing on a second thread is a data race
// waiting to be found. The main thread parses once, at `new Worker`, and the worker holds the answers.
struct WorkerLocation {
  std::string href;
  std::string origin;
  std::string protocol;
  std::string host;
  std::string hostname;
  std::string port;
  std::string pathname;
  std::string search;
  std::string hash;
};

class WorkerScope {
 public:
  WorkerScope(js::Interpreter& interpreter, WorkerScopeHost& host, std::string name,
              WorkerLocation location);

  WorkerScope(const WorkerScope&) = delete;
  WorkerScope& operator=(const WorkerScope&) = delete;

  // Builds the global surface. Called once, before the worker's script runs.
  void Install();

  // The worker's own script, or one `importScripts` brought in. An uncaught throw is reported to the
  // page as an `error` event and the worker stays alive, which is what the specification says.
  void RunScript(const std::string& source, const std::string& url);

  // One message from the page, deserialised into this heap and dispatched as a `message` event.
  void DeliverMessage(const js::SerializedValue& message);

  // Milliseconds until the earliest timer is due, or -1 when there are none. Negative-but-not-minus-one
  // is impossible: an overdue timer answers zero, so the caller never sleeps on one.
  double NextDelayMs() const;
  bool HasTimers() const { return !timers_.empty(); }

  // Runs every timer whose deadline has passed, in deadline order.
  void RunDueTimers();

  // The end of a turn: the microtask queue is drained and anything the script logged is forwarded.
  // Called after the script, after each batch of messages, and after each batch of timers -- which is
  // the same "once per task" rule the page's loop follows.
  void EndTurn();

 private:
  // A pending `setTimeout`/`setInterval`. The *callback* is not here: it lives in a table hung off the
  // global, because a `js::Value` in a C++ container is invisible to the collector and a timer whose
  // function was collected is a crash on the next tick.
  struct Timer {
    std::uint64_t id = 0;
    double due_ms = 0.0;
    double interval_ms = 0.0;
    bool repeating = false;
  };

  void InstallGlobalScopeTypes();
  void InstallEvents();
  void InstallTimers();
  void InstallImportScripts();
  void InstallLocation();
  void InstallNavigator();

  // Declares `value` both as a property of the global object and as a binding in the global scope, so
  // that `addEventListener(...)` unqualified and `self.addEventListener(...)` are the same function.
  // testharness.js calls it both ways in the same file.
  void Define(const char* name, const js::Value& value);

  // The listener table for `type`, created on demand. Hung off the global so the collector walks it.
  js::Value ListenersFor(const std::string& type, bool create);

  // Dispatches `event` to the `on<type>` handler and then to every registered listener, which is the
  // order `RunListenersOn` uses on the page's side. An uncaught throw becomes an `error` report rather
  // than stopping the dispatch: one bad listener must not silence the rest.
  void Dispatch(const js::Value& event);

  js::Value MakeEvent(const char* type);

  // Reports an uncaught error the way the specification does: as an `error` event on the global first,
  // and to the page only if nothing handled it. testharness.js installs exactly such a listener and
  // turns the error into a failed test rather than a silent timeout.
  void ReportUncaught(const js::Value& error, const std::string& url);

  js::Interpreter& interpreter_;
  WorkerScopeHost& host_;
  std::string name_;
  WorkerLocation location_;
  std::vector<Timer> timers_;
  std::uint64_t next_timer_id_ = 0;
  // How far into the interpreter's console log has already been forwarded.
  std::size_t console_cursor_ = 0;
};

}  // namespace microbrowser::engine
