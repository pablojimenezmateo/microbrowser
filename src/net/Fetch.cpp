#include "net/Fetch.h"

#include <array>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool HeaderNameIs(std::string_view name, std::string_view expected) {
  return util::EqualsAsciiCaseInsensitive(name, expected);
}

bool IsRequestFramingHeader(std::string_view name) {
  return HeaderNameIs(name, "content-length") || HeaderNameIs(name, "transfer-encoding");
}

bool IsFetchOwnedHeader(std::string_view name) {
  return IsRequestFramingHeader(name) || HeaderNameIs(name, "host") ||
         HeaderNameIs(name, "connection") || HeaderNameIs(name, "proxy-connection") ||
         HeaderNameIs(name, "accept-language") || HeaderNameIs(name, "accept-encoding") ||
         HeaderNameIs(name, "cookie") || HeaderNameIs(name, "referer") ||
         HeaderNameIs(name, "user-agent") || HeaderNameIs(name, "te") ||
         HeaderNameIs(name, "trailer") || HeaderNameIs(name, "upgrade");
}

bool IsBodyHeader(std::string_view name) {
  return IsRequestFramingHeader(name) || HeaderNameIs(name, "content-type");
}

void DropBodyHeaders(FetchOptions& options) {
  HttpHeaders kept;
  for (const HttpHeaders::Field& field : options.headers.Fields()) {
    if (!IsBodyHeader(field.name)) {
      kept.Add(field.name, field.value);
    }
  }
  options.headers = std::move(kept);
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
    if (IsFetchOwnedHeader(field.name)) {
      continue;
    }
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

bool MayUseHttpCache(const FetchOptions& options,
                     std::string_view cookie_header,
                     std::string_view referrer) {
  return options.method == "GET" && options.body.empty() && options.headers.Fields().empty() &&
         cookie_header.empty() && referrer.empty();
}

}  // namespace

FetchRequest::FetchRequest(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                           TransportFactory& transport, CookieJar& cookies, HttpCache& cache,
                           FetchOptions options, std::int64_t now)
    : verdict_(std::move(verdict)),
      policy_(policy),
      transport_(transport),
      cookies_(cookies),
      cache_(cache),
      remaining_(std::move(options)),
      now_(now) {
  AddPerformanceCounter(PerfCounterId::NetFetches);
  if (!verdict_.IsAllowed()) {
    // The privacy layer already refused. Reaching the socket anyway is the one
    // thing this class must never do, and doing the check in the constructor
    // means there is no state in which it has not been done.
    Fail(verdict_.Reason().empty() ? std::string_view("refused by policy")
                                  : std::string_view(verdict_.Reason()));
  }
}

FetchRequest::~FetchRequest() {
  if (connection_ != nullptr) {
    connection_->Close();
  }
}

void FetchRequest::Fail(std::string_view reason) {
  if (complete_) {
    return;
  }
  if (connection_ != nullptr) {
    connection_->Close();
    connection_.reset();
  }
  result_ = FetchResult{};
  result_.error = std::string(reason);
  result_.redirects = redirects_;
  complete_ = true;
  stage_ = Stage::Done;
  AddPerformanceCounter(PerfCounterId::NetFetchFailures);
}

void FetchRequest::Complete(HttpResponse response, const url::Url& url) {
  if (connection_ != nullptr) {
    connection_->Close();
    connection_.reset();
  }
  result_ = FetchResult{};
  result_.ok = true;
  result_.response = std::move(response);
  result_.final_url = url;
  result_.redirects = redirects_;
  complete_ = true;
  stage_ = Stage::Done;
}

bool FetchRequest::BeginExchange() {
  const url::Url& url = verdict_.FinalUrl();
  if (!url.IsHttpOrHttps()) {
    Fail("not an http(s) URL");
    return false;
  }

  const bool same_site = verdict_.Partition().IsFirstParty();
  const std::string cookie_header = cookies_.HeaderFor(
      verdict_.Partition(), url, same_site, remaining_.is_top_level_navigation, now_);
  may_use_cache_ = MayUseHttpCache(remaining_, cookie_header, verdict_.Referrer());
  if (may_use_cache_ && !remaining_.bypass_cache) {
    if (const HttpCache::Entry* cached = cache_.Lookup(verdict_.Partition(), url, now_)) {
      HttpResponse response = cached->response;
      Complete(std::move(response), url);
      result_.from_cache = true;
      return false;
    }
  }

  const HttpHeaders headers = BuildHeaders(url, remaining_, verdict_, cookie_header);
  outgoing_ = SerializeRequest(remaining_.method, RequestTarget(url), headers);
  outgoing_.append(reinterpret_cast<const char*>(remaining_.body.data()),
                   remaining_.body.size());
  sent_ = 0;
  parser_ = ResponseParser{};

  connection_ = transport_.Create();
  if (connection_ == nullptr) {
    Fail("no transport");
    return false;
  }
  const bool secure = url.Scheme() == "https";
  const std::uint16_t port = url.EffectivePort().value_or(secure ? 443 : 80);
  if (!connection_->StartConnect(url.HostSerialized(), port, secure)) {
    Fail("connect failed");
    return false;
  }
  stage_ = Stage::Connecting;
  return true;
}

void FetchRequest::FinishResponse() {
  const url::Url url = verdict_.FinalUrl();
  HttpResponse response = parser_.TakeResponse();

  // Cookies are stored under the partition this request was made in, which is
  // what makes a third party's Set-Cookie land in the jar for *this* top-level
  // site and nowhere else.
  for (const std::string_view field : response.headers.GetAll("set-cookie")) {
    cookies_.StoreFromHeader(verdict_.Partition(), url, field, now_);
  }

  if (!response.IsRedirect() || !response.headers.Has("location")) {
    if (may_use_cache_) {
      cache_.Store(verdict_.Partition(), url, response, now_);
    }
    Complete(std::move(response), url);
    return;
  }

  if (++redirects_ > remaining_.max_redirects) {
    Fail("too many redirects");
    return;
  }
  AddPerformanceCounter(PerfCounterId::NetRedirects);

  const auto location = url::Url::Parse(*response.headers.Get("location"), url);
  if (!location.has_value()) {
    Fail("malformed redirect target");
    return;
  }

  // The redirect goes back through the policy. A server that could redirect
  // past it would be able to reach a blocked host, downgrade to http, or
  // restore the tracking parameters that were just stripped — by answering with
  // a 302.
  privacy::Request next;
  next.url = *location;
  next.initiator = url::Origin::FromUrl(url);
  next.top_level_site = verdict_.Partition().TopLevelSite();
  next.container = verdict_.Partition().Container();
  next.type = verdict_.Type();
  next.is_subresource = verdict_.IsSubresource();
  verdict_ = policy_.Decide(next, &url);
  if (!verdict_.IsAllowed()) {
    Fail("redirect refused by policy");
    return;
  }

  // 303, and 301/302 in practice, turn everything into a GET without a body.
  if (response.status == 303 || ((response.status == 301 || response.status == 302) &&
                                 remaining_.method != "GET" && remaining_.method != "HEAD")) {
    remaining_.method = "GET";
    remaining_.body.clear();
    DropBodyHeaders(remaining_);
  }

  // The old connection is finished with. Reuse across a redirect would need the
  // partition-keyed pool from ADR 0010, which is a separate change.
  if (connection_ != nullptr) {
    connection_->Close();
    connection_.reset();
  }
  stage_ = Stage::Begin;
}

bool FetchRequest::Advance() {
  bool progress = false;
  blocked_ = false;
  std::array<std::byte, 16 * 1024> buffer{};

  while (true) {
    switch (stage_) {
      case Stage::Done:
        return progress;

      case Stage::Begin:
        if (!BeginExchange()) {
          return true;  // served from cache, or failed: either way it moved
        }
        progress = true;
        break;

      case Stage::Connecting: {
        const IoStatus status = connection_->Advance();
        if (status == IoStatus::Blocked) {
          blocked_ = true;
          return progress;
        }
        if (status != IoStatus::Ready) {
          Fail("connect failed");
          return true;
        }
        stage_ = Stage::Sending;
        progress = true;
        break;
      }

      case Stage::Sending: {
        while (sent_ < outgoing_.size()) {
          const std::span<const std::byte> rest(
              reinterpret_cast<const std::byte*>(outgoing_.data()) + sent_,
              outgoing_.size() - sent_);
          const IoResult wrote = connection_->Send(rest);
          if (wrote.status == IoStatus::Blocked) {
            blocked_ = true;
            return progress;
          }
          if (wrote.status != IoStatus::Ready || wrote.bytes == 0) {
            Fail("send failed");
            return true;
          }
          sent_ += wrote.bytes;
          progress = true;
        }
        stage_ = Stage::Receiving;
        break;
      }

      case Stage::Receiving: {
        while (!parser_.IsComplete() && !parser_.Failed()) {
          const IoResult read = connection_->Receive(buffer);
          if (read.status == IoStatus::Blocked) {
            blocked_ = true;
            return progress;
          }
          if (read.status == IoStatus::Closed) {
            // A body delimited by the connection closing is complete here and
            // nowhere else, which is why Closed and Failed are separate answers.
            if (!parser_.Finish()) {
              Fail(parser_.Error() != nullptr ? parser_.Error() : "truncated response");
              return true;
            }
            break;
          }
          if (read.status != IoStatus::Ready) {
            Fail("receive failed");
            return true;
          }
          progress = true;
          if (!parser_.Consume(std::span<const std::byte>(buffer.data(), read.bytes))) {
            Fail(parser_.Error() != nullptr ? parser_.Error() : "malformed response");
            return true;
          }
        }
        if (!parser_.IsComplete()) {
          Fail(parser_.Error() != nullptr ? parser_.Error() : "incomplete response");
          return true;
        }
        FinishResponse();
        return true;
      }
    }
  }
}

std::optional<util::WaitDescriptor> FetchRequest::Interest() const {
  if (complete_ || connection_ == nullptr) {
    return std::nullopt;
  }
  return connection_->Interest();
}

std::unique_ptr<FetchRequest> Fetch(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                                    TransportFactory& transport, CookieJar& cookies,
                                    HttpCache& cache, const FetchOptions& options,
                                    std::int64_t now) {
  return std::make_unique<FetchRequest>(std::move(verdict), policy, transport, cookies, cache,
                                        options, now);
}

}  // namespace microbrowser::net
