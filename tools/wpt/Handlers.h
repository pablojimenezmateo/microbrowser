#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::wpt {

// The `.py` handlers this suite actually leans on, **transcribed** rather than approximated.
//
// ADR 0040 §2 decided not to implement them, and the reason it gave is right and still stands:
// "a handler is arbitrary Python; approximating one makes a test pass for the wrong reason, which is
// worse than a failure." It also named the condition under which the trade changes -- "until somebody
// measures that those specific tests are what is blocking" -- and the measurement now exists. See the
// amendment at the end of that ADR.
//
// Two rules keep this on the right side of the objection, and both are structural rather than
// intentions:
//
//   1. **Every handler here is a transcription of a specific file in the checkout, with that file's
//      source quoted above it.** Not "something that behaves roughly like a redirect": the same
//      parameters, the same defaults, the same order of operations. A reviewer can put the two side
//      by side. Where upstream's behaviour depends on something this server does not have, the
//      handler is *absent* rather than guessed.
//   2. **The set is a closed list keyed by repo-relative path.** Anything not on it still answers
//      501, so a test that needs an unimplemented handler still fails visibly rather than silently
//      getting a plausible-looking 200. That is the property ADR 0040 was protecting.
//
// The handlers share `Stash`, which is wptserve's `request.server.stash` -- a key-value store that
// outlives one request. It is a plain map here because this server is one process and one thread,
// which is the same reason `tools/wpt` has no locks anywhere else.

// wptserve's stash: a key/value store keyed by a token a test invents, living for the run.
class Stash {
 public:
  void Put(const std::string& key, std::string value) { entries_[key] = std::move(value); }
  // Reads *and removes*, which is what `stash.take` does -- a test that took twice gets nothing the
  // second time, and several tests assert exactly that.
  std::optional<std::string> Take(const std::string& key);
  // Reads without removing. wptserve has no such method; the handlers that need it (`redirect.py`
  // counting its own redirects) do a take followed by a put, which is this plus `Put`.
  const std::string* Peek(const std::string& key) const;

 private:
  std::map<std::string, std::string> entries_;
};

// One request, as a handler sees it.
struct HandlerRequest {
  std::string method;
  // Repo-relative, e.g. `fetch/api/resources/status.py`. This is the dispatch key: three different
  // `redirect.py` files exist in the checkout with three different behaviours, so a handler named by
  // its basename would be one of them applied to all three.
  std::string path;
  // Raw and undecoded, exactly as it arrived. Handlers decode per-parameter, because wptserve does.
  std::string query;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  // `http://host:port` for the connection this arrived on, which is what a handler that builds an
  // absolute URL needs.
  std::string origin;
};

struct HandlerResponse {
  // False when nothing on the list matched, which the server turns into the 501 it always gave.
  bool handled = false;
  int status = 200;
  std::string status_text = "OK";
  // Content-Type included, like any other. A handler that sets none gets whatever the server would
  // have chosen, which for a `.py` path is `text/plain`.
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  // How long to hold the answer. **Not a sleep**: this server is single-threaded, and a sleep in it
  // would stall every other test in the run -- which with six test processes in parallel is a
  // cascade of timeouts that looks like a browser bug. The server holds the response and writes it
  // on a later turn of its poll loop.
  int delay_ms = 0;
};

// `?a=1&b=2` as pairs, percent-decoded, in order. `+` is a space, which is what a form-encoded query
// means and what wptserve's parser does.
std::vector<std::pair<std::string, std::string>> ParseQuery(std::string_view query);
// wptserve's `request.GET.first(name, default)`.
std::string QueryFirst(const std::vector<std::pair<std::string, std::string>>& query,
                       std::string_view name, std::string_view fallback = {});
bool QueryHas(const std::vector<std::pair<std::string, std::string>>& query, std::string_view name);
// A request header, case-insensitively. Empty when absent, which every handler treats as absent
// because none of them distinguishes an empty header from a missing one.
std::string HeaderValue(const HandlerRequest& request, std::string_view name);

// Runs the handler for `request.path`, or returns `handled == false`.
HandlerResponse RunHandler(const HandlerRequest& request, Stash& stash);

// wptserve's `?pipe=` chain, applied to a response the server has already built.
//
// It is the *other* half of what makes a checkout readable without Python: hundreds of tests ask for
// a status or a header on an ordinary static file with `?pipe=status(404)` or
// `?pipe=header(Content-Type,text/html)`, and without it those files are served as themselves --
// which is a wrong answer rather than a missing one, and therefore worse.
//
// `status`, `header`, `slice` and `trickle` are implemented; `sub` is a no-op here because the server
// already applies substitution to a `.sub.` file, and anything else on the chain is ignored rather
// than guessed.
void ApplyPipes(std::string_view pipe, HandlerResponse& response);

}  // namespace microbrowser::wpt
