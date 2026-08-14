// The engine half of `fetch`: where a request a *page* made becomes a real one.
//
// Its own translation unit for the reason EngineInput.cpp is: Engine.cpp is
// already at its cap, and a file over its cap means a missing module rather
// than a bigger file. This is the `bindings::NetworkSource` implementation and
// nothing else -- the interface is declared in `src/bindings`, which may not
// see `net`, and implemented here, which owns the loader. ADR 0015 established
// that inversion for geometry; ADR 0020 §1 uses it for the network.
//
// What this file does *not* do is decide anything. The privacy verdict is
// `StartSubresource`'s, CORS is `net`'s, and what comes back is already
// whatever the page may see. This turns four strings into `net::FetchOptions`
// and one completion back into a `bindings::ScriptResponse`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/Clock.h"
#include "engine/Engine.h"
#include "html/UrlEncoding.h"
#include "url/Origin.h"
#include "url/Url.h"
#include "util/LoadTimeline.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

// The three request modes a page can name, and anything else it names.
//
// The strings are the specification's and the enum is `net`'s; this is the one
// place they meet, which is why `bindings::ScriptRequest` carries strings. An
// unrecognised value is the default rather than an error, exactly as the
// specification says -- a page that writes `mode: "cors "` gets a cors fetch
// and not a broken one.
net::RequestMode ModeFromScript(std::string_view mode) {
  if (mode == "no-cors") {
    return net::RequestMode::NoCors;
  }
  if (mode == "same-origin") {
    return net::RequestMode::SameOrigin;
  }
  return net::RequestMode::Cors;
}

net::CredentialsMode CredentialsFromScript(std::string_view credentials) {
  if (credentials == "omit") {
    return net::CredentialsMode::Omit;
  }
  if (credentials == "include") {
    return net::CredentialsMode::Include;
  }
  return net::CredentialsMode::SameOrigin;
}

// The method, normalised the way the specification normalises it: the ones it
// names are uppercased and everything else is left alone. `fetch(url, {method:
// "post"})` has to be a POST, and `patch` is *not* PATCH -- which is the one
// case that surprises people and the one the specification is explicit about.
std::string NormaliseMethod(std::string_view method) {
  static constexpr std::string_view kNamed[] = {"DELETE", "GET",  "HEAD",
                                                "OPTIONS", "POST", "PUT"};
  for (const std::string_view named : kNamed) {
    if (util::EqualsAsciiCaseInsensitive(method, named)) {
      return std::string(named);
    }
  }
  return std::string(method);
}

// `data:` and `blob:` are answered inside `Loader::StartSubresource` without a
// socket. `connect-src` governs network connections; refusing a local URL here
// was blocking reddit's `fetch('data:text/javascript,…')` bootstrap probes while
// recording a violation the page never meant to make.
bool IsLocallyResolvedUrl(std::string_view url) {
  return util::StartsWithAsciiCaseInsensitive(url, "data:") ||
         util::StartsWithAsciiCaseInsensitive(url, "blob:");
}

}  // namespace

std::uint64_t Engine::StartFetch(const bindings::ScriptRequest& request) {
  const Loader::RequestId id = StartScriptRequest(request);
  if (id != 0) {
    script_fetches_.insert(id);
  }
  return id;
}

// The request half, shared with a *worker's* `fetch`. One function rather than two, because
// everything in it is a decision -- `connect-src`, the CORS mode, what a string body implies about
// Content-Type -- and a second copy is a second place for one of them to be made differently. The
// only thing the caller decides is which table the id goes in, which is which side settles the
// promise.
Loader::RequestId Engine::StartScriptRequest(const bindings::ScriptRequest& request) {
  // Resolved against the document, which is also the initiator and -- with no
  // frames -- the top-level site. `StartSubresource` does all of that and puts
  // the result through `privacy::PrivacyPolicy`, which is why a page's own
  // request takes exactly the same road as an image.
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return 0;
  }
  // Between Navigate and the replacement document there is no page that may
  // open sockets. AbandonForNavigation removes the interpreter; this gate
  // covers any other entry (img/beacon path that still had a binding) (TD-0048).
  if (load_.active && !load_.document_arrived) {
    util::AddPerformanceCounter(util::PerfCounterId::EngineFetchRejectedDuringNavigation);
    return 0;
  }
  // A URL that still says `[object Object]` is a DOMString coercion miss: some
  // binding took a Location/URL/host object through pure `js::ToString`. Count
  // it so a youtube load that still does this is visible without a timeline.
  if (request.url.find("[object") != std::string::npos ||
      request.url.find("%5Bobject") != std::string::npos) {
    util::AddPerformanceCounter(util::PerfCounterId::FetchObjectObjectUrl);
    if (util::LoadTimeline::Enabled()) {
      util::LoadTimeline::MarkWith("fetch.object_object_url",
                                   request.method + " " + request.url);
    }
  }
  // `connect-src`, and this is the only place a page's own request can be
  // stopped: `fetch` and `XMLHttpRequest` both come through here, so the check
  // is on the request rather than on the API that made it. A refused request is
  // *not* started, and the caller sees the same zero it sees for a URL that
  // does not parse -- which rejects the promise without telling the page which
  // of the two happened.
  if (!IsLocallyResolvedUrl(request.url) &&
      !page_.Policy().AllowsUrl(csp::Directive::Connect, request.url)) {
    return 0;
  }

  net::FetchOptions options;
  options.method = NormaliseMethod(request.method);
  for (const bindings::ScriptHeader& header : request.headers) {
    options.headers.Add(header.name, header.value);
  }
  if (!request.body.empty()) {
    options.body.reserve(request.body.size());
    for (const char byte : request.body) {
      options.body.push_back(static_cast<std::byte>(byte));
    }
    if (request.body_from_string && !options.headers.Has("content-type")) {
      // What the specification attaches to a string body, and it is not
      // cosmetic: `text/plain` is CORS-safelisted, so a POST of a string is a
      // simple request and a POST of JSON -- which the page labels itself --
      // costs a preflight. A byte source (ArrayBuffer / typed array) leaves
      // Content-Type unset, which is also safelisted-by-absence and what
      // YouTube's SABR POST needs.
      options.headers.Add("Content-Type", "text/plain;charset=UTF-8");
    }
  }
  options.cors.mode = ModeFromScript(request.mode);
  options.cors.credentials = CredentialsFromScript(request.credentials);
  options.cors.origin = url::Origin::FromUrl(*base);

  return loader_.StartSubresource(request.url, *base, privacy::ResourceType::Xhr, NowSeconds(),
                                  options);
}

// The response half, shared for the same reason. What arrives here has already been filtered by
// `net` -- an opaque response is an empty one because the bytes were discarded, not hidden.
bindings::ScriptResponse Engine::ScriptResponseFrom(Loader::Completion& completion) {
  bindings::ScriptResponse response;
  response.ok = completion.result.ok;
  response.error = completion.result.error;
  response.status = completion.result.status;
  response.status_text = completion.result.status_text;
  response.url = completion.result.final_url;
  response.body = std::move(completion.result.body);
  response.redirected = completion.result.redirected;
  response.opaque = completion.result.opaque;
  response.headers.reserve(completion.result.headers.size());
  for (const auto& [name, value] : completion.result.headers) {
    response.headers.push_back(bindings::ScriptHeader{name, value});
  }
  return response;
}

void Engine::StartWorkerFetchRequests() {
  for (const Workers::FetchAbort& abort : page_.TakeWorkerFetchAborts()) {
    // Abandoned before the answer arrived -- an `AbortController`, or the worker terminating. The
    // loader request is cancelled and the mapping dropped, so no delivery follows: an aborted fetch
    // that could still run its own `then` is the bug this is here to prevent.
    for (auto it = worker_script_fetches_.begin(); it != worker_script_fetches_.end();) {
      if (it->second.first == abort.worker_id && it->second.second == abort.request_id) {
        loader_.Cancel(it->first);
        it = worker_script_fetches_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const Workers::FetchRequest& pending : page_.TakeWorkerFetchRequests()) {
    const Loader::RequestId id = StartScriptRequest(pending.request);
    if (id == 0) {
      // Refused before it went out -- an unparseable URL, the privacy layer, `connect-src`. The
      // worker's promise has to be *rejected* rather than left, so a refusal is delivered as a
      // failed response rather than as silence.
      bindings::ScriptResponse refused;
      refused.error = "the request was refused";
      page_.DeliverWorkerFetch(pending.worker_id, pending.request_id, std::move(refused));
      continue;
    }
    worker_script_fetches_[id] = {pending.worker_id, pending.request_id};
  }
}

bool Engine::OnWorkerScriptRequestFetch(Loader::Completion completion) {
  const auto found = worker_script_fetches_.find(completion.id);
  if (found == worker_script_fetches_.end()) {
    return false;
  }
  const auto [worker_id, request_id] = found->second;
  worker_script_fetches_.erase(found);
  page_.DeliverWorkerFetch(worker_id, request_id, ScriptResponseFrom(completion));
  return true;
}

std::string Engine::ResolveDocumentUrl(std::string_view relative) const {
  // HTML's "encoding-parse a URL". Two things make it different from the call below and both are
  // the document's: the base is `<base href>` when the document has one the policy allowed, and the
  // query is encoded in the document's character set rather than in UTF-8.
  //
  // The encoder is a local rather than a member because it is three bytes of state and because a
  // cached one would have to be invalidated when a `<meta charset>` in the prescan disagrees with
  // the header -- a stale encoder is a link that sends the wrong bytes, which is the failure this
  // whole path exists to avoid.
  const html::DocumentQueryEncoder encoder(page_.Policy().Encoding());
  const std::optional<url::Url>& base = page_.Policy().Base();
  if (!base.has_value()) {
    const std::optional<url::Url> absolute = url::Url::Parse(relative, &encoder);
    return absolute.has_value() ? absolute->Serialize() : std::string();
  }
  const std::optional<url::Url> resolved = url::Url::Parse(relative, *base, &encoder);
  return resolved.has_value() ? resolved->Serialize() : std::string();
}

std::string Engine::ResolveUrl(std::string_view relative, std::string_view base) const {
  // The one parser, and the *only* thing this adds is a fallback base. `url::Url::Parse` needs a base
  // that is itself absolute; a page can pass anything as the second argument to `new URL`, so a base
  // that does not parse makes the whole call fail -- which is what the specification says.
  const std::optional<url::Url> parsed_base = url::Url::Parse(base);
  if (!parsed_base.has_value()) {
    const std::optional<url::Url> absolute = url::Url::Parse(relative);
    return absolute.has_value() ? absolute->Serialize() : std::string();
  }
  const std::optional<url::Url> resolved = url::Url::Parse(relative, *parsed_base);
  return resolved.has_value() ? resolved->Serialize() : std::string();
}

void Engine::AbortFetch(std::uint64_t id) {
  script_fetches_.erase(id);
  loader_.Cancel(id);
}

std::string Engine::RegisterBlobUrl(std::string body, std::string mime_type) {
  return page_.RegisterBlobUrl(std::move(body), std::move(mime_type));
}

void Engine::RevokeBlobUrl(const std::string& url) { page_.RevokeBlobUrl(url); }

bool Engine::OnScriptFetch(Loader::Completion completion) {
  if (script_fetches_.erase(completion.id) == 0) {
    return false;
  }
  const bindings::ScriptResponse response = ScriptResponseFrom(completion);
  if (!page_.DeliverFetchResponse(completion.id, response)) {
    return false;
  }
  // A `then` handler is a script turn like any other: the document may have
  // changed under the layout, so drop it and paint again -- and only when
  // something was actually waiting, which is what `DeliverFetchResponse`
  // answering false is for.
  LayoutAndPaint();
  return true;
}

}  // namespace microbrowser::engine
