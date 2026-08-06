#include "net/Fetch.h"

#include <array>
#include <utility>

#include "net/ContentEncoding.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"
#include "util/UserAgent.h"

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

// `Host` on HTTP/1.1 and `:authority` on HTTP/2 -- the same string, built once,
// because two spellings of "the server this is addressed to" is one more place
// for the two protocols to disagree about where a request is going.
std::string AuthorityFor(const url::Url& url) {
  std::string authority = url.HostSerialized();
  if (url.Port().has_value()) {
    authority.push_back(':');
    authority += std::to_string(*url.Port());
  }
  return authority;
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

// Whether this request sends the user's cookies.
//
// The `Browser` answer is unconditional and is what every load before ADR 0020
// did: which cookies travel is already decided inside the jar, by the partition
// key and by same-site. The three fetch modes narrow that, and `SameOrigin` --
// the default -- is the one that matters: a page's `fetch` to a third party
// carries nothing unless it asked for `credentials: "include"` *and* the server
// agreed, which is checked on the way back.
bool SendsCredentials(const CorsParams& cors, const url::Url& url) {
  switch (cors.credentials) {
    case CredentialsMode::Omit:
      return false;
    case CredentialsMode::Include:
      return true;
    case CredentialsMode::SameOrigin:
      break;
  }
  return cors.mode == RequestMode::Browser || IsSameOrigin(cors.origin, url);
}

// Whether an `Origin` header goes out.
//
// Every non-`Browser` request that is not a plain read gets one, and every
// `cors` request gets one whatever its method: that header is what a server
// answers `Access-Control-Allow-Origin` against, so omitting it would make
// every cross-origin fetch fail for a reason the page could not see.
bool SendsOrigin(const CorsParams& cors, std::string_view method) {
  if (cors.mode == RequestMode::Browser) {
    return false;
  }
  return cors.mode == RequestMode::Cors || (method != "GET" && method != "HEAD");
}

// Builds the header set actually sent.
//
// Assembled here rather than taken from the caller so that no header can be
// sent because a field happened to be left set. Anti-fingerprinting lives here
// too: `Accept-Language` is `en-US` regardless of the system locale, because
// the system locale is an identifying bit the user did not choose to reveal,
// and `User-Agent` is one constant naming the browser and nothing about the
// machine — see util/UserAgent.h, which is also what script is told.
HttpHeaders BuildHeaders(const url::Url& url, const FetchOptions& options,
                         const privacy::Verdict& verdict, const std::string& cookie_header) {
  HttpHeaders headers;
  headers.Add("Host", AuthorityFor(url));
  headers.Add("User-Agent", util::kUserAgent);
  headers.Add("Accept-Language", "en-US");
  // Exactly the set net::DecodeContentEncoding can undo. Advertising more than
  // that turns every response using the difference into a failed load.
  headers.Add("Accept-Encoding", kAcceptedContentEncodings);
  // No `Connection` header at all. HTTP/1.1 is persistent by default, so
  // `keep-alive` would be a byte on every request saying what the version
  // already says, and one more bit for a fingerprinter to measure. The header
  // stays in IsHeaderOwnedByFetch so a caller still cannot set it: whether a
  // connection is kept is a decision the pool makes, not one a page influences.

  if (SendsOrigin(options.cors, options.method)) {
    // The serialization, which is "null" for an opaque origin and for one a
    // cross-origin redirect tainted. Both must send the same thing: a request
    // that revealed its real origin after being bounced through a third party
    // would let that third party read a response meant for the first.
    headers.Add("Origin", options.cors.origin.Serialize());
  }
  if (!options.cors.preflight_method.empty()) {
    headers.Add("Access-Control-Request-Method", options.cors.preflight_method);
    if (!options.cors.preflight_headers.empty()) {
      headers.Add("Access-Control-Request-Headers", options.cors.preflight_headers);
    }
  }

  for (const HttpHeaders::Field& field : options.headers.Fields()) {
    if (IsHeaderOwnedByFetch(field.name)) {
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
                           ConnectionPool& pool, CookieJar& cookies, HttpCache& cache,
                           FetchOptions options, std::int64_t now)
    : verdict_(std::move(verdict)),
      policy_(policy),
      pool_(pool),
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
  ReleaseEverything();
}

void FetchRequest::ReleaseEverything() {
  if (connection_ != nullptr) {
    connection_->Close();
    connection_.reset();
  }
  if (session_ != nullptr) {
    if (stream_ != 0) {
      // Tells the server to stop sending. Without it an abandoned image goes on
      // arriving on a connection nineteen other requests are sharing.
      session_->CloseStream(stream_);
      stream_ = 0;
    }
    // Dropped, not closed: the session belongs to the pool and to whoever else
    // is using it.
    session_.reset();
  }
  if (owns_connect_) {
    // The claim on "somebody is connecting to this origin". Every path out of
    // this class comes through here, because a claim left behind parks every
    // other request for the same origin until the stall deadline.
    pool_.AbandonConnect(connect_key_);
    owns_connect_ = false;
  }
}

void FetchRequest::Fail(std::string_view reason) {
  if (complete_) {
    return;
  }
  ReleaseEverything();
  result_ = FetchResult{};
  result_.error = std::string(reason);
  result_.redirects = redirects_;
  complete_ = true;
  stage_ = Stage::Done;
  AddPerformanceCounter(PerfCounterId::NetFetchFailures);
}

void FetchRequest::Complete(HttpResponse response, const url::Url& url) {
  ReleaseEverything();

  // **The CORS decision, at the last point inside `net` and before a result
  // exists.** Every way a response can arrive -- the wire, the cache, a
  // redirect chain that ended here -- comes through this function, which is why
  // the check is here rather than at each of them. A refusal calls `Fail`,
  // which throws the response away: the caller gets an error and no bytes, not
  // bytes it is trusted not to look at. See ADR 0020 §2 and Cors.h.
  //
  // A preflight is the exception, and only because its response never leaves
  // this module: `RequestQueue` reads `Access-Control-Allow-Methods` off it and
  // throws it away. Filtering those headers out here -- which is what the
  // `cors` branch below would do -- would leave the queue with a response that
  // granted nothing.
  const bool is_preflight = !remaining_.cors.preflight_method.empty();
  const CorsResult decision =
      is_preflight ? CorsResult{true, {}} : CheckResponse(remaining_.cors, url, response);
  if (!decision.allowed) {
    Fail(decision.error);
    return;
  }
  const bool opaque = !is_preflight &&
      remaining_.cors.mode == RequestMode::NoCors && !IsSameOrigin(remaining_.cors.origin, url);
  if (opaque) {
    // Not a flag over readable bytes: the bytes are gone. Status 0, no headers,
    // no body -- which is what `Response.type === "opaque"` means, and the only
    // form of it that survives somebody forgetting to check a flag.
    response = HttpResponse{};
    response.status = 0;
    AddPerformanceCounter(PerfCounterId::NetCorsOpaque);
  } else if (!is_preflight && remaining_.cors.mode == RequestMode::Cors &&
             !IsSameOrigin(remaining_.cors.origin, url)) {
    FilterExposedHeaders(response);
  }

  result_ = FetchResult{};
  result_.ok = true;
  result_.opaque = opaque;
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
  const std::string cookie_header =
      SendsCredentials(remaining_.cors, url)
          ? cookies_.HeaderFor(verdict_.Partition(), url, same_site,
                               remaining_.is_top_level_navigation, now_)
          : std::string();
  may_use_cache_ = MayUseHttpCache(remaining_, cookie_header, verdict_.Referrer());
  if (may_use_cache_ && !remaining_.bypass_cache) {
    if (const HttpCache::Entry* cached = cache_.Lookup(verdict_.Partition(), url, now_)) {
      HttpResponse response = cached->response;
      Complete(std::move(response), url);
      result_.from_cache = true;
      return false;
    }
  }

  // The header *list*, not a serialized request: HTTP/1.1 and HTTP/2 send the
  // same fields and differ only in how they are framed, and the framing cannot
  // be chosen until ALPN has answered. Building this once here is what keeps
  // the two protocols from ever disagreeing about what was sent.
  request_headers_ = BuildHeaders(url, remaining_, verdict_, cookie_header);
  sent_ = 0;
  parser_ = ResponseParser{};

  const bool secure = url.Scheme() == "https";
  const std::uint16_t port = url.EffectivePort().value_or(secure ? 443 : 80);
  ConnectionPool::Lease lease =
      pool_.Acquire(verdict_.Partition().Serialize(), url.HostSerialized(), port, secure,
                    /*allow_reuse=*/!retried_);
  connect_key_ = std::move(lease.key);
  owns_connect_ = lease.owns_connect;
  reused_ = lease.reused;

  if (lease.wait_for_protocol) {
    // Somebody else is opening the connection whose ALPN decides whether this
    // origin speaks HTTP/2. Opening a second one would be the burst that
    // protocol exists to remove -- see ConnectionPool::Acquire.
    stage_ = Stage::AwaitingProtocol;
    return true;
  }
  if (lease.session != nullptr) {
    // An origin already known to speak HTTP/2, with a session already open.
    // Nothing is connected and nothing is handshaked: this request is a stream.
    session_ = std::move(lease.session);
    if (!StartStream()) {
      Fail("the connection could not take another request");
      return false;
    }
    return true;
  }

  connection_ = std::move(lease.connection);
  if (connection_ == nullptr) {
    Fail("no transport");
    return false;
  }
  // A connection out of the pool is already connected and already through its
  // handshake; one that is not has to be started. Both then go through
  // `Stage::Connecting`, because an open transport answers `Ready` to
  // `Advance()` and a second state machine for the reused case would be a
  // second place for this to be wrong.
  if (!reused_ && !connection_->StartConnect(verdict_.Partition().Serialize(),
                                             url.HostSerialized(), port, secure)) {
    Fail("connect failed");
    return false;
  }
  stage_ = Stage::Connecting;
  return true;
}

void FetchRequest::ChooseProtocol() {
  // **The one place the two protocols diverge.** The verdict, the cookies, the
  // cache, CORS, the redirect chain and the retry rule are all above this line
  // or below it, and none of them is written twice.
  if (connection_->NegotiatedProtocol() == "h2") {
    session_ = pool_.AdoptHttp2(connect_key_, std::move(connection_));
    connection_.reset();
    owns_connect_ = false;  // AdoptHttp2 released it
    if (session_ == nullptr || !StartStream()) {
      Fail("the connection could not take the request");
    }
    return;
  }
  if (owns_connect_) {
    // Recorded, so the next six requests to this origin do not queue behind
    // each other: a bound that is right while the protocol is unknown is wrong
    // the moment it is known.
    pool_.FinishedHttp1(connect_key_);
    owns_connect_ = false;
  }
  const url::Url& url = verdict_.FinalUrl();
  outgoing_ = SerializeRequest(remaining_.method, RequestTarget(url), request_headers_);
  outgoing_.append(reinterpret_cast<const char*>(remaining_.body.data()),
                   remaining_.body.size());
  sent_ = 0;
  stage_ = Stage::Sending;
}

bool FetchRequest::StartStream() {
  const url::Url& url = verdict_.FinalUrl();
  Http2Session::Request request;
  request.method = remaining_.method;
  request.scheme = url.Scheme();
  request.authority = AuthorityFor(url);
  request.target = RequestTarget(url);
  request.headers = &request_headers_;
  request.body = std::span<const std::byte>(remaining_.body);
  const std::optional<Http2Session::StreamId> id = session_->StartRequest(request);
  if (!id.has_value()) {
    return false;
  }
  stream_ = *id;
  stage_ = Stage::Streaming;
  return true;
}

void FetchRequest::ReleaseConnection(const HttpResponse& response, std::int64_t now_ms) {
  if (connection_ == nullptr) {
    return;
  }
  const url::Url& url = verdict_.FinalUrl();
  const bool secure = url.Scheme() == "https";
  const std::uint16_t port = url.EffectivePort().value_or(secure ? 443 : 80);

  // Four things have to hold at once, and every one of them is the difference
  // between a connection that can carry another request and one that cannot:
  //
  //  - the message said how long it was, rather than ending when the socket
  //    did. A close-delimited body's only terminator is the close.
  //  - nothing arrived after it. A byte past the end is either a response
  //    nobody asked for or a second framing of the same bytes, and the next
  //    request would start reading at an unknown offset.
  //  - the server did not say `Connection: close`, which is a promise it is
  //    about to close and a socket we would write into as it goes away.
  //  - the version is persistent by default, or said `keep-alive` if it is not.
  const std::optional<std::string_view> connection = response.headers.Get("connection");
  const bool says_close =
      connection.has_value() && util::EqualsAsciiCaseInsensitive(*connection, "close");
  const bool says_keep_alive =
      connection.has_value() && util::EqualsAsciiCaseInsensitive(*connection, "keep-alive");
  const bool persistent_by_default = response.version_minor >= 1;
  const bool keep = parser_.BodyWasSelfDelimiting() && parser_.Leftover() == 0 && !says_close &&
                    (persistent_by_default || says_keep_alive);

  if (!keep) {
    connection_->Close();
    connection_.reset();
    return;
  }
  pool_.Release(verdict_.Partition().Serialize(), url.HostSerialized(), port, secure,
                std::move(connection_), now_ms);
  connection_.reset();
}

void FetchRequest::DeliverResponse(HttpResponse response) {
  const url::Url url = verdict_.FinalUrl();

  // Before anything reads the body, and before the cache is offered it: what is
  // stored, matched against a redirect, or handed to a parser is the decoded
  // form, and there is no state in which a caller holds a body still under a
  // coding it would have to know about.
  switch (DecodeContentEncoding(response)) {
    case DecodeStatus::Identity:
    case DecodeStatus::Decoded:
      break;
    case DecodeStatus::UnsupportedCoding:
      Fail("response uses a content coding we did not ask for");
      return;
    case DecodeStatus::TooLarge:
      Fail("decompressed response exceeds its bound");
      return;
    case DecodeStatus::Malformed:
      Fail("malformed compressed response");
      return;
  }

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

  // A redirect is a response too, and a cross-origin one has to have said so
  // before it is allowed to send this request somewhere else. Without this the
  // check at the end of the chain would be the only one, and a server could
  // bounce a cross-origin request through itself to a URL that *does* permit
  // the origin -- reading the first response by the URL it chose to send us to.
  const CorsResult hop = CheckResponse(remaining_.cors, url, response);
  if (!hop.allowed) {
    Fail(hop.error);
    return;
  }
  if (remaining_.cors.preflighted) {
    // The permission a preflight bought names one URL. Spending it on another
    // is what the specification forbids, and following the redirect after a
    // fresh preflight is work with no target site asking for it.
    Fail("redirect after a CORS preflight");
    return;
  }

  const auto location = url::Url::Parse(*response.headers.Get("location"), url);
  if (!location.has_value()) {
    Fail("malformed redirect target");
    return;
  }
  if (remaining_.cors.mode != RequestMode::Browser &&
      !IsSameOrigin(remaining_.cors.origin, *location)) {
    if (remaining_.cors.mode == RequestMode::SameOrigin) {
      Fail("same-origin request redirected to a different origin");
      return;
    }
    // The origin is *tainted* from here on: every later hop sends `Origin:
    // null` and is checked against it. A request that kept its real origin
    // across a third party's redirect would let that third party spend it.
    remaining_.cors.origin = url::Origin();
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

  // The connection has already gone back to the pool if it could. If the
  // redirect stays on the same host in the same partition, the next exchange
  // takes it straight back out — which is the case ADR 0010 was written for and
  // the one that used to cost a whole handshake. On HTTP/2 there is nothing to
  // take back out and nothing to close: the session is the pool's, this
  // request's stream is already finished with, and the next hop asks for
  // another one on the same connection.
  ReleaseEverything();
  parser_ = ResponseParser{};
  reused_ = false;
  retried_ = false;
  stage_ = Stage::Begin;
}

bool FetchRequest::Advance(std::int64_t now_ms) {
  bool progress = false;
  blocked_ = false;
  std::array<std::byte, 16 * 1024> buffer{};

  // A pooled connection the server closed while it was idle fails on the send
  // or on the first read, and it looks exactly like a server that went away.
  // The difference is that this one was never asked anything, so asking again
  // on a fresh socket repeats nothing.
  const auto retry_or_fail = [this](std::string_view reason, bool nothing_was_processed) {
    if (!MayRetry(nothing_was_processed)) {
      Fail(reason);
      return;
    }
    ReleaseEverything();
    retried_ = true;
    reused_ = false;
    sent_ = 0;
    parser_ = ResponseParser{};
    stage_ = Stage::Begin;
  };

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
        // The handshake is over, so ALPN has an answer and this request finds
        // out which protocol it is speaking.
        ChooseProtocol();
        progress = true;
        break;
      }

      case Stage::AwaitingProtocol:
        // Another request is opening the connection whose ALPN will say whether
        // this origin speaks HTTP/2. Going round again re-asks the pool, which
        // by then has either a session to join or an origin known to speak
        // HTTP/1.1. Blocked rather than runnable, so this does not spin: the
        // connecting request's socket is in the loop's wait, and if that request
        // has gone away `RequestQueue::HasRunnableWork` is what says so.
        stage_ = Stage::Begin;
        if (!BeginExchange()) {
          return true;
        }
        if (stage_ == Stage::AwaitingProtocol) {
          blocked_ = true;
          return progress;
        }
        progress = true;
        break;

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
            retry_or_fail("send failed", reused_ && parser_.NothingReceived());
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
              retry_or_fail(parser_.Error() != nullptr ? parser_.Error() : "truncated response",
                            reused_ && parser_.NothingReceived());
              return true;
            }
            break;
          }
          if (read.status != IoStatus::Ready) {
            retry_or_fail("receive failed", reused_ && parser_.NothingReceived());
            return true;
          }
          progress = true;
          AddPerformanceCounter(PerfCounterId::NetBytesReceived, read.bytes);
          if (!parser_.Consume(std::span<const std::byte>(buffer.data(), read.bytes))) {
            Fail(parser_.Error() != nullptr ? parser_.Error() : "malformed response");
            return true;
          }
        }
        if (!parser_.IsComplete()) {
          Fail(parser_.Error() != nullptr ? parser_.Error() : "incomplete response");
          return true;
        }
        HttpResponse response = parser_.TakeResponse();
        // Released first, while the parser still knows how the body was framed
        // and while the verdict still names the URL this exchange was with.
        // After a redirect rewrites either, the connection would be filed under
        // the wrong key -- and a connection under the wrong partition key is
        // the cross-site linkage ADR 0005 exists to prevent.
        ReleaseConnection(response, now_ms);
        DeliverResponse(std::move(response));
        return true;
      }

      case Stage::Streaming: {
        // Advancing the *session* moves every request sharing this connection,
        // not just this one. That is the whole of multiplexing and it is why a
        // request no longer owns the socket it is reading from.
        progress |= session_->Advance();
        switch (session_->StateOf(stream_)) {
          case Http2Session::StreamState::Open:
            blocked_ = session_->IsBlocked();
            return progress;
          case Http2Session::StreamState::Complete: {
            HttpResponse response = session_->TakeResponse(stream_);
            stream_ = 0;
            // Nothing to release: the session stays in the pool with whatever
            // other requests are still on it. `DeliverResponse` drops this
            // request's reference when it completes or redirects.
            DeliverResponse(std::move(response));
            return true;
          }
          case Http2Session::StreamState::Refused:
          case Http2Session::StreamState::Failed: {
            const char* reason = session_->ErrorOf(stream_);
            const bool refused =
                session_->StateOf(stream_) == Http2Session::StreamState::Refused;
            retry_or_fail(reason != nullptr ? reason : "the stream failed", refused);
            return true;
          }
          case Http2Session::StreamState::Unknown:
            Fail("the stream went away");
            return true;
        }
        return progress;
      }
    }
  }
}

std::optional<util::WaitDescriptor> FetchRequest::Interest() const {
  if (complete_) {
    return std::nullopt;
  }
  // On HTTP/2 this is the *shared* connection's descriptor, so several requests
  // report the same one. That is correct and is why `RequestQueue` deduplicates
  // before handing the list to the wait: one socket, watched once.
  if (session_ != nullptr) {
    return session_->Interest();
  }
  if (connection_ == nullptr) {
    return std::nullopt;
  }
  return connection_->Interest();
}

std::unique_ptr<FetchRequest> Fetch(privacy::Verdict verdict, const privacy::PrivacyPolicy& policy,
                                    ConnectionPool& pool, CookieJar& cookies, HttpCache& cache,
                                    const FetchOptions& options, std::int64_t now) {
  return std::make_unique<FetchRequest>(std::move(verdict), policy, pool, cookies, cache, options,
                                        now);
}

}  // namespace microbrowser::net
