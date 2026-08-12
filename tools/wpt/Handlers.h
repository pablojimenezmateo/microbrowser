#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::wpt {

// What a web-platform-tests `.py` handler would have answered, written in C++.
//
// **Not a Python interpreter, and ADR 0040 is right that it must not become one.** The server is
// ours, single-threaded, and forked before anything else exists; that is what makes a hang one
// TIMEOUT line rather than the end of the run, and an embedded interpreter would put a third
// party's runtime inside that process. What is here instead is a *table*: one C++ function per
// handler the suite actually leans on, each of which is a dozen lines because that is what the
// original is.
//
// The argument for building it at all is a measurement. 2,214 in-scope test files name a `.py`
// handler -- 976 in `css/`, 434 in `html/`, 203 in `xhr/`, 171 in `fetch/` -- and until this
// existed every one of them got a 501 and a test that reported nothing. 308 distinct handlers, but
// the head is short: `slow.py` is referenced 216 times, `stash.py` 199, `redirect.py` 182. A dozen
// of them is most of the traffic.
//
// **A handler that is absent is still a 501, deliberately.** Answering 200 with an empty body for
// an unknown handler would turn "this test needs a handler nobody has written" into "this test
// fails for an unknown reason", which is the same trade ADR 0012 refuses for a web API: the
// absence has to stay visible, because the ranked-cause table in `docs/wpt-baseline.md` is how the
// next handler gets chosen.

// One request, as much of it as a handler can ask about.
struct HandlerRequest {
  std::string_view method;
  // Path relative to the WPT root, e.g. `xhr/resources/status.py`. The handler is chosen by its
  // file name; the directory is kept because the stash is scoped by it, exactly as `wptserve`
  // scopes it -- two tests in different directories using the same key must not collide.
  std::string_view path;
  std::string_view query;
  // In arrival order, names as sent. `inspect-headers.py` reports the name *as the client wrote
  // it*, so this cannot be a case-folded map.
  const std::vector<std::pair<std::string, std::string>>* headers = nullptr;
  std::string_view body;
  // This server's own origin for the port the request arrived on, for a handler that has to name
  // itself in a redirect or a CORS header.
  std::string_view origin;
};

struct HandlerResponse {
  int status = 200;
  std::string status_text = "OK";
  // Empty means "do not send one", which is a state a handler needs: `status.py` with no `type`
  // parameter deliberately sends an empty `Content-Type`, and a test asserts on that.
  std::string content_type;
  bool send_content_type = false;
  // Whole lines, `Name: value`.
  std::vector<std::string> headers;
  std::string body;
};

// The per-run scratch space `stash.py` and its relatives read and write. One process and one
// thread, so a plain map is the whole of it -- and it is the server's rather than a global,
// because a global would survive between runs of the same binary in-process.
class Stash {
 public:
  void Put(const std::string& directory, const std::string& key, std::string value);
  // Reads *and removes*, which is what `stash.take` means: a value is delivered once, and a test
  // that asks twice is asserting that the second answer is null.
  bool Take(const std::string& directory, const std::string& key, std::string* out);

 private:
  std::map<std::pair<std::string, std::string>, std::string> values_;
};

// True when a native handler answered. False means no handler of that name is implemented, and the
// caller should say so rather than invent a response.
bool InvokeHandler(const HandlerRequest& request, Stash& stash, HandlerResponse* out);

// Every handler name this table implements, for a diagnostic that wants to say what is missing.
std::vector<std::string_view> ImplementedHandlers();

}  // namespace microbrowser::wpt
