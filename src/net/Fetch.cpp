#include "net/Fetch.h"

#include <array>

#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

FetchResult Failure(const char* reason) {
  FetchResult result;
  result.error = reason;
  AddPerformanceCounter(PerfCounterId::NetFetchFailures);
  return result;
}

// The request line target: path and query, never the fragment. A fragment is
// client-side and sending one tells a server something it has no business
// knowing.
std::string RequestTarget(const url::Url& url) {
  std::string target = url.PathString();
  if (target.empty()) {
    target = "/";
  }
  if (url.HasQuery()) {
    target.push_back('?');
    target += url.Query();
  }
  return target;
}

// Builds the header set actually sent.
//
// Assembled here rather than taken from the caller so that no header can be
// sent because a field happened to be left set. Anti-fingerprinting lives here
// too: `Accept-Language` is `en-US` regardless of the system locale, because
// the system locale is an identifying bit the user did not choose to reveal.
HttpHeaders BuildHeaders(const url::Url& url, const FetchOptions& options,
                         const privacy::Verdict& verdict, const std::string& cookie_header) {
  HttpHeaders headers;
  std::string host = url.HostSerialized();
  if (url.Port().has_value()) {
    host.push_back(':');
    host += std::to_string(*url.Port());
  }
  headers.Add("Host", host);
  headers.Add("Accept-Language", "en-US");
  headers.Add("Accept-Encoding", "identity");
  headers.Add("Connection", "close");

  for (const HttpHeaders::Field& field : options.headers.Fields()) {
    headers.Add(field.name, field.value);
  }
  if (!verdict.Referrer().empty()) {
    headers.Add("Referer", verdict.Referrer());
  }
  if (!cookie_header.empty()) {
    headers.Add("Cookie", cookie_header);
  }
  if (!options.body.empty()) {
    headers.Add("Content-Length", std::to_string(options.body.size()));
  }
  return headers;
}

bool MayUseHttpCache(const FetchOptions& options) {
  return options.method == "GET" && options.body.empty();
}

}  // namespace

FetchResult Fetch(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                  TransportFactory& transport, CookieJar& cookies, HttpCache& cache,
                  const FetchOptions& options, std::int64_t now) {
  AddPerformanceCounter(PerfCounterId::NetFetches);

  if (!verdict.IsAllowed()) {
    // The privacy layer already refused. Reaching the socket anyway is the one
    // thing this function must never do.
    return Failure(verdict.Reason().empty() ? "refused by policy" : verdict.Reason().c_str());
  }

  FetchOptions remaining = options;
  int redirects = 0;

  while (true) {
    const url::Url& url = verdict.FinalUrl();
    if (!url.IsHttpOrHttps()) {
      return Failure("not an http(s) URL");
    }

    const bool may_use_cache = MayUseHttpCache(remaining);
    if (may_use_cache) {
      if (const HttpCache::Entry* cached = cache.Lookup(verdict.Partition(), url, now)) {
        FetchResult result;
        result.ok = true;
        result.response = cached->response;
        result.final_url = url;
        result.redirects = redirects;
        result.from_cache = true;
        return result;
      }
    }

    const bool same_site = verdict.Partition().IsFirstParty();
    const std::string cookie_header = cookies.HeaderFor(
        verdict.Partition(), url, same_site, remaining.is_top_level_navigation, now);
    const HttpHeaders headers = BuildHeaders(url, remaining, verdict, cookie_header);
    const std::string request = SerializeRequest(remaining.method, RequestTarget(url), headers);

    std::unique_ptr<Transport> connection = transport.Create();
    if (connection == nullptr) {
      return Failure("no transport");
    }
    const bool secure = url.Scheme() == "https";
    const std::uint16_t port = url.EffectivePort().value_or(secure ? 443 : 80);
    if (!connection->Connect(url.HostSerialized(), port, secure)) {
      return Failure("connect failed");
    }
    if (!connection->Send(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(request.data()), request.size()))) {
      return Failure("send failed");
    }
    if (!remaining.body.empty() && !connection->Send(remaining.body)) {
      return Failure("send body failed");
    }

    ResponseParser parser;
    std::array<std::byte, 16 * 1024> buffer{};
    while (!parser.IsComplete() && !parser.Failed()) {
      const auto read = connection->Receive(buffer);
      if (!read.has_value()) {
        return Failure("receive failed");
      }
      if (*read == 0) {
        if (!parser.Finish()) {
          return Failure(parser.Error() != nullptr ? parser.Error() : "truncated response");
        }
        break;
      }
      if (!parser.Consume(std::span<const std::byte>(buffer.data(), *read))) {
        return Failure(parser.Error() != nullptr ? parser.Error() : "malformed response");
      }
    }
    connection->Close();
    if (!parser.IsComplete()) {
      return Failure(parser.Error() != nullptr ? parser.Error() : "incomplete response");
    }

    HttpResponse response = parser.TakeResponse();

    // Cookies are stored under the partition this request was made in, which is
    // what makes a third party's Set-Cookie land in the jar for *this*
    // top-level site and nowhere else.
    for (const std::string_view field : response.headers.GetAll("set-cookie")) {
      cookies.StoreFromHeader(verdict.Partition(), url, field, now);
    }

    if (!response.IsRedirect() || !response.headers.Has("location")) {
      if (may_use_cache) {
        cache.Store(verdict.Partition(), url, response, now);
      }
      FetchResult result;
      result.ok = true;
      result.response = std::move(response);
      result.final_url = url;
      result.redirects = redirects;
      return result;
    }

    if (++redirects > remaining.max_redirects) {
      return Failure("too many redirects");
    }
    AddPerformanceCounter(PerfCounterId::NetRedirects);

    const auto location = url::Url::Parse(*response.headers.Get("location"), url);
    if (!location.has_value()) {
      return Failure("malformed redirect target");
    }

    // The redirect goes back through the policy. A server that could redirect
    // past it would be able to reach a blocked host, downgrade to http, or
    // restore the tracking parameters that were just stripped — by answering
    // with a 302.
    privacy::Request next;
    next.url = *location;
    next.initiator = url::Origin::FromUrl(url);
    next.top_level_site = verdict.Partition().TopLevelSite();
    next.container = verdict.Partition().Container();
    next.type = privacy::ResourceType::Document;
    next.is_subresource = !remaining.is_top_level_navigation;
    verdict = policy.Decide(next, &url);
    if (!verdict.IsAllowed()) {
      return Failure("redirect refused by policy");
    }

    // 303, and 301/302 in practice, turn everything into a GET without a body.
    if (response.status == 303 || ((response.status == 301 || response.status == 302) &&
                                   remaining.method != "GET" && remaining.method != "HEAD")) {
      remaining.method = "GET";
      remaining.body.clear();
    }
  }
}

}  // namespace microbrowser::net
