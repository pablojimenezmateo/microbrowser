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
#include "url/Origin.h"
#include "url/Url.h"
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

}  // namespace

std::uint64_t Engine::StartFetch(const bindings::ScriptRequest& request) {
  // Resolved against the document, which is also the initiator and -- with no
  // frames -- the top-level site. `StartSubresource` does all of that and puts
  // the result through `privacy::PrivacyPolicy`, which is why a page's own
  // request takes exactly the same road as an image.
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return 0;
  }
  // `connect-src`, and this is the only place a page's own request can be
  // stopped: `fetch` and `XMLHttpRequest` both come through here, so the check
  // is on the request rather than on the API that made it. A refused request is
  // *not* started, and the caller sees the same zero it sees for a URL that
  // does not parse -- which rejects the promise without telling the page which
  // of the two happened.
  if (!page_.Policy().AllowsUrl(csp::Directive::Connect, request.url)) {
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

  const Loader::RequestId id = loader_.StartSubresource(
      request.url, *base, privacy::ResourceType::Xhr, NowSeconds(), options);
  script_fetches_.insert(id);
  return id;
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
