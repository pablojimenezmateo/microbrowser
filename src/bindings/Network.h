#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microbrowser::bindings {

// One header, as a page wrote it or as a server sent it.
//
// Two strings rather than `net::HttpHeaders`, which this module may not see.
// The type an answer travels in is part of the seam rather than a convenience:
// a seam that needed `net` would be one `allow:` line away from letting a page's
// bindings open a socket. Same rule as `bindings::GeometryRect`, same reason.
struct ScriptHeader {
  std::string name;
  std::string value;
};

// What `fetch` was asked for.
//
// The URL is whatever the page wrote -- relative, absolute, or nonsense. It is
// resolved against the document's base URL by the implementation, because what
// a URL means is the loader's problem and this module is not allowed to have an
// opinion about it.
struct ScriptRequest {
  std::string url;
  std::string method = "GET";
  std::vector<ScriptHeader> headers;
  std::string body;
  // The strings the specification uses: "cors", "no-cors", "same-origin", and
  // "omit"/"same-origin"/"include". Strings rather than an enum this module
  // declares, because the implementation has to turn them into `net::CorsParams`
  // and two enumerations of the same four words is one more place for them to
  // stop meaning the same thing. Anything unrecognised is the default, which is
  // what the specification says a bad `mode` is.
  std::string mode = "cors";
  std::string credentials = "same-origin";
};

// What came back, as much of it as this request is allowed to see.
//
// **Already filtered.** A `no-cors` response arrives here with `opaque` set,
// status 0 and nothing in it, because `net` discarded the rest before this
// struct existed. A `cors` response arrives with the headers the server
// exposed and no others. Nothing in this module decides any of that, and
// nothing in this module could undo it -- which is ADR 0020 §2's whole point.
struct ScriptResponse {
  // Whether the request reached a server at all. A 404 is `ok` here and not
  // `ok` in JavaScript: `Response.ok` is about the status, and this is about
  // whether there is a status.
  bool ok = false;
  // Why not, for the console. Never handed to script: a cross-origin failure
  // that explained itself would be the cross-origin read CORS exists to
  // prevent.
  std::string error;
  int status = 0;
  std::string status_text;
  std::vector<ScriptHeader> headers;
  std::string body;
  // Where the bytes came from after redirects, which is what `response.url`
  // reports and what a page resolves a relative link in a fragment against.
  std::string url;
  bool redirected = false;
  bool opaque = false;
};

// Where a request a *page* made goes.
//
// Declared here, in the module that asks, and implemented by `src/engine` --
// the same inversion `GeometrySource` uses and for the same reason: this module
// may see `util`, `js`, `dom` and `html`, and not `net`, not `url`, not
// `engine`. ADR 0008's boundary is what makes this the only path from a page's
// code to its document, and widening it by one line to reach a loader would
// delete the reason the module exists.
//
// Two rules, both load-bearing:
//
//   * **Started, never awaited.** `StartFetch` hands back an id and returns.
//     The answer arrives later through `DomBindings::DeliverFetchResponse`,
//     because a binding that blocked would block the one loop this browser has.
//   * **The policy is on the other side.** Everything about who may read what
//     is decided in `net`, at the seam the network process will be. An
//     implementation of this interface passes a request through
//     `privacy::Verdict` and CORS and hands back whatever survives.
class NetworkSource {
 public:
  virtual ~NetworkSource() = default;

  // Starts a request. The id is unique for the life of the document and is what
  // the answer will be tagged with. Zero means the request was refused before
  // it started -- an unparseable URL, a scheme this browser will not fetch --
  // and the caller rejects immediately rather than waiting for a delivery that
  // will never come.
  virtual std::uint64_t StartFetch(const ScriptRequest& request) = 0;

  // Drops a started request. No delivery follows, which is what makes an
  // aborted fetch unable to run its own `then` afterwards. Calling it for an id
  // that has already been delivered is harmless and does nothing.
  virtual void AbortFetch(std::uint64_t id) = 0;

  // A relative URL resolved against the document's base, canonicalised, or empty when it does not
  // parse. This is here rather than in a URL-shaped interface of its own because it is the same
  // question a fetch already asks -- and it must be answered by the *one* parser in `src/url`, which
  // this module may not see. A second URL parser in the binding layer is exactly the "two parsers
  // disagreeing about where the host ends" that `url/Url.h` names as the vulnerability.
  //
  // Added for `new URL(...)` (session 28), which exists because `URL.createObjectURL` has to hang off
  // something and a `URL` that was not constructible would be a stub -- a page that finds `URL` and
  // gets a TypeError from `new URL(href)` has taken the branch that assumes it works.
  virtual std::string ResolveUrl(std::string_view relative, std::string_view base) const = 0;
};

}  // namespace microbrowser::bindings
