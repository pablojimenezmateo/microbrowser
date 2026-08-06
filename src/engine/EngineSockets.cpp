// A page's WebSockets, and the one place a page can be told no.
//
// ADR 0020 §5. Its own translation unit for the reason EngineFetch.cpp is, and the seam
// is the same one: everything a *policy* decides happens here, on the engine's side of
// `bindings::SocketSource`, because `src/bindings` may not see `net`, `csp` or `url`.
//
// Three properties are the whole of this file, and each is one a page would notice:
//
//   * **A socket is user-caused and passes the same layers as everything else.**
//     `wss://` goes through `privacy::PrivacyPolicy` and `connect-src`, and a refusal
//     opens nothing rather than opening and then failing.
//   * **A socket dies with the document.** `Clear` runs on navigation, and the
//     connection's destructor closes the transport -- so nothing outlives the page that
//     asked for it, by construction rather than by a rule callers follow.
//   * **An open socket is one descriptor in the idle wait and nothing else.** No timer,
//     no poll. That is what keeps the zero-idle-CPU invariant true with a connection
//     open, and `AppendSocketDescriptors` plus `SocketsHaveWork` are the whole of how
//     this file participates in the loop.

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "csp/ContentSecurityPolicy.h"
#include "engine/Clock.h"
#include "engine/Engine.h"
#include "net/WebSocketConnection.h"
#include "privacy/PrivacyPolicy.h"
#include "url/Url.h"
#include "util/Base64.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

// The `Sec-WebSocket-Key`: sixteen bytes, base64.
//
// Not random, and the reason is the same one the masking key gives: what the key
// protects against is a *cache* answering an upgrade request, and `wss://` means there
// is no cache in between. It varies per socket so that two sockets on one page do not
// share a handshake, which is all this browser needs it for. A plaintext `ws://` path
// would need a real one, and that is written in both places rather than one.
std::string NextSocketKey(std::uint64_t counter) {
  std::string raw(16, '\0');
  for (std::size_t i = 0; i < raw.size(); ++i) {
    counter = counter * 6364136223846793005ull + 1442695040888963407ull;
    raw[i] = static_cast<char>((counter >> 33) & 0xFFu);
  }
  return util::Base64Encode(raw);
}

}  // namespace

std::uint64_t Engine::OpenSocket(std::string_view url) {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return 0;
  }
  const std::optional<url::Url> target = url::Url::Parse(url, *base);
  if (!target.has_value()) {
    return 0;
  }
  // `ws`/`wss` only, and `ws` is refused: this browser is HTTPS-only for documents and
  // a plaintext socket would be the one unencrypted thing on an encrypted page. The
  // masking key and the handshake key are both counters here, and both are safe
  // *because* the transport is TLS -- so accepting `ws://` would quietly invalidate two
  // decisions made elsewhere.
  const bool secure = target->Scheme() == "wss";
  if (!secure && target->Scheme() != "ws") {
    return 0;
  }
  if (!secure) {
    AddPerformanceCounter(util::PerfCounterId::WebSocketInsecureRefusals);
    return 0;
  }
  // `connect-src`, on the engine side for the reason `fetch`'s check is: the policy
  // lives in `src/csp`, which `src/bindings` may not see. A refused socket is not
  // opened, and script cannot tell that from a URL that did not parse.
  if (!page_.Policy().AllowsUrl(csp::Directive::Connect, url)) {
    return 0;
  }

  privacy::Request request;
  request.url = *target;
  request.initiator = url::Origin::FromUrl(*base);
  request.top_level_site = url::Site::FromUrl(*base);
  request.container = url::ContainerId::Default();
  request.type = privacy::ResourceType::Other;
  request.is_subresource = true;
  const privacy::Verdict verdict = loader_.Policy().Decide(request, &*base);
  if (!verdict.IsAllowed()) {
    return 0;
  }

  std::unique_ptr<net::Transport> transport = loader_.NewTransport();
  if (transport == nullptr) {
    return 0;
  }
  const std::uint16_t port = target->Port().value_or(443);
  std::string path = target->PathString();
  if (path.empty()) {
    path = "/";
  }
  if (target->HasQuery()) {
    path += "?" + target->Query();
  }
  const std::uint64_t id = ++next_socket_id_;
  sockets_.emplace(id, std::make_unique<net::WebSocketConnection>(
                           std::move(transport), verdict, target->HostSerialized(), port,
                           std::move(path), secure, NextSocketKey(id)));
  return id;
}

bool Engine::SendSocket(std::uint64_t id, std::string_view data, bool text) {
  const auto found = sockets_.find(id);
  return found != sockets_.end() && found->second->Send(data, text);
}

void Engine::CloseSocket(std::uint64_t id, std::uint16_t code, std::string_view reason) {
  const auto found = sockets_.find(id);
  if (found != sockets_.end()) {
    found->second->Close(code, reason);
  }
}

std::uint64_t Engine::SocketBufferedAmount(std::uint64_t id) {
  const auto found = sockets_.find(id);
  return found != sockets_.end() && found->second->HasPendingWrites() ? 1u : 0u;
}

void Engine::AppendSocketDescriptors(util::WaitDescriptorList& out) const {
  for (const auto& [id, connection] : sockets_) {
    if (const std::optional<util::WaitDescriptor> interest = connection->Interest()) {
      if (interest->IsValid()) {
        out.push_back(*interest);
      }
    }
  }
  for (const auto& [id, stream] : event_sources_) {
    if (const std::optional<util::WaitDescriptor> interest = stream->Interest()) {
      if (interest->IsValid()) {
        out.push_back(*interest);
      }
    }
  }
}

std::optional<std::uint32_t> Engine::NextEventSourceDeadlineMs(std::int64_t now_ms) const {
  // A stream *waiting* to reconnect is the one thing in this file that needs a timer, and
  // it is the reason an EventSource is not simply a socket. An open stream contributes
  // nothing, so an idle page with one still blocks indefinitely.
  std::optional<std::uint32_t> soonest;
  for (const auto& [id, stream] : event_sources_) {
    if (const std::optional<std::int64_t> at = stream->RetryAtMs()) {
      const std::int64_t delay = *at > now_ms ? *at - now_ms : 0;
      const std::uint32_t clamped = static_cast<std::uint32_t>(std::min<std::int64_t>(delay, 60000));
      soonest = soonest.has_value() ? std::min(*soonest, clamped) : clamped;
    }
  }
  return soonest;
}

bool Engine::SocketsHaveWork() const {
  // Only a socket with something *queued* is work. An open socket waiting on a server
  // is not: it is a descriptor in the wait, and answering true here would make the loop
  // spin for as long as a page held a connection -- which is exactly the failure ADR
  // 0020 §5 says this feature must not cause.
  for (const auto& [id, connection] : sockets_) {
    if (connection->HasPendingWrites()) {
      return true;
    }
  }
  return false;
}

bool Engine::AdvanceSockets() {
  bool delivered = false;
  // Collected first: delivering an event runs script, and script may close a socket or
  // open another. Iterating the map while that happens is how a container is mutated
  // under its own loop.
  std::vector<std::uint64_t> ids;
  ids.reserve(sockets_.size());
  for (const auto& [id, connection] : sockets_) {
    ids.push_back(id);
  }
  for (const std::uint64_t id : ids) {
    const auto found = sockets_.find(id);
    if (found == sockets_.end()) {
      continue;
    }
    const net::WebSocketConnection::Progress progress = found->second->Advance();
    if (progress.opened) {
      delivered = page_.DeliverSocketOpen(id) || delivered;
    }
    for (const auto& [text, data] : progress.messages) {
      delivered = page_.DeliverSocketMessage(id, data, text) || delivered;
    }
    if (progress.closed) {
      // Read before the connection is erased: the code and the reason are on it, and a
      // page's `onclose` handler reads all three.
      const auto still = sockets_.find(id);
      std::uint16_t code = 1006;
      std::string reason;
      bool clean = false;
      if (still != sockets_.end()) {
        code = still->second->CloseCode();
        reason = still->second->CloseReason();
        clean = still->second->ClosedCleanly();
      }
      delivered = page_.DeliverSocketClose(id, code, reason, clean, progress.failed) || delivered;
      sockets_.erase(id);
    }
  }
  return delivered;
}

std::uint64_t Engine::OpenEventSource(std::string_view url) {
  const std::optional<url::Url>& base = page_.BaseUrl();
  if (!base.has_value()) {
    return 0;
  }
  const std::optional<url::Url> target = url::Url::Parse(url, *base);
  // `https` only, for the reason `wss` is required: this is a long-lived connection whose
  // bytes are a page's own data, and a plaintext one on an encrypted page is the one
  // unencrypted thing on it.
  if (!target.has_value() || target->Scheme() != "https") {
    return 0;
  }
  // The same directive a `fetch` and a socket ask about, and the same reason it is asked
  // here: the policy lives in `src/csp`, which `src/bindings` may not see.
  if (!page_.Policy().AllowsUrl(csp::Directive::Connect, url)) {
    return 0;
  }
  privacy::Request request;
  request.url = *target;
  request.initiator = url::Origin::FromUrl(*base);
  request.top_level_site = url::Site::FromUrl(*base);
  request.container = url::ContainerId::Default();
  request.type = privacy::ResourceType::Other;
  request.is_subresource = true;
  const privacy::Verdict verdict = loader_.Policy().Decide(request, &*base);
  if (!verdict.IsAllowed()) {
    return 0;
  }
  std::unique_ptr<net::Transport> transport = loader_.NewTransport();
  if (transport == nullptr) {
    return 0;
  }
  std::string path = target->PathString();
  if (path.empty()) {
    path = "/";
  }
  if (target->HasQuery()) {
    path += "?" + target->Query();
  }
  const std::uint64_t id = ++next_socket_id_;
  event_sources_.emplace(id, std::make_unique<net::EventSourceConnection>(
                                 std::move(transport), verdict, target->HostSerialized(),
                                 target->Port().value_or(443), std::move(path)));
  return id;
}

void Engine::CloseEventSource(std::uint64_t id) {
  const auto found = event_sources_.find(id);
  if (found != event_sources_.end()) {
    found->second->Close();
    event_sources_.erase(found);
  }
}

bool Engine::AdvanceEventSources() {
  const std::int64_t now = NowMilliseconds();
  bool delivered = false;
  std::vector<std::uint64_t> ids;
  ids.reserve(event_sources_.size());
  for (const auto& [id, stream] : event_sources_) {
    ids.push_back(id);
  }
  for (const std::uint64_t id : ids) {
    auto found = event_sources_.find(id);
    if (found == event_sources_.end()) {
      continue;
    }
    // The reconnect, and this is the only place a transport is made for a request the user
    // did not cause. The *connection* decides when -- backoff, cap, and giving up are its
    // business -- and the engine only supplies the socket, because a connection that could
    // make its own would be one that could reconnect after its document was gone.
    if (found->second->NeedsTransport(now)) {
      found->second->Restart(loader_.NewTransport(), now);
    }
    const net::EventSourceConnection::Progress progress = found->second->Advance(now);
    if (progress.opened) {
      delivered = page_.DeliverEventSourceOpen(id) || delivered;
    }
    for (const net::ServerSentEvent& event : progress.events) {
      delivered =
          page_.DeliverEventSourceMessage(id, event.type, event.data, event.id.value_or(""))
              ? true
              : delivered;
    }
    if (progress.failed) {
      // `error` fires on every drop, including the ones that will be retried -- which is
      // what the specification says and what lets a page show "reconnecting".
      delivered = page_.DeliverEventSourceError(id, progress.closed) || delivered;
    }
    if (progress.closed) {
      event_sources_.erase(id);
    }
  }
  return delivered;
}

void Engine::CloseAllSockets() {
  // A navigation. Erasing is closing: the connection's destructor closes its transport,
  // which is what makes "no socket outlives its document" a property of the type rather
  // than a sequence someone has to remember.
  sockets_.clear();
  // And the streams, whose reconnect makes this the more important of the two: a stream
  // that outlived its document would keep asking a server for events on behalf of a page
  // that is gone.
  event_sources_.clear();
}

}  // namespace microbrowser::engine
