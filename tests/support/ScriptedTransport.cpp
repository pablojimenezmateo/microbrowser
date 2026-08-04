#include "support/ScriptedTransport.h"

#include <algorithm>
#include <cstring>

namespace microbrowser::tests {

ScriptedTransport::~ScriptedTransport() { factory_.Forget(*this); }

bool ScriptedTransport::StartConnect(std::string_view host, std::uint16_t port, bool secure) {
  if (factory_.cursor >= factory_.script.size()) {
    return false;
  }
  // The slot is claimed here rather than at Close, because with requests
  // running concurrently two live connections would otherwise be reading the
  // same exchange.
  index_ = factory_.cursor++;
  const Exchange& exchange = factory_.script[index_];
  if (!exchange.expected_host.empty() && exchange.expected_host != host) {
    return false;
  }
  if (exchange.expected_port != 0 && exchange.expected_port != port) {
    return false;
  }
  if (exchange.expected_secure != secure) {
    return false;
  }
  if (factory_.log.hosts.size() <= index_) {
    factory_.log.hosts.resize(index_ + 1);
    factory_.log.secure.resize(index_ + 1);
    factory_.log.requests.resize(index_ + 1);
  }
  factory_.log.hosts[index_] = std::string(host);
  factory_.log.secure[index_] = secure;
  pending_ = exchange.response;
  released_ = factory_.delivery == Factory::Delivery::Immediate;
  factory_.Register(*this);
  return true;
}

net::IoStatus ScriptedTransport::Advance() { return net::IoStatus::Ready; }

net::IoResult ScriptedTransport::Send(std::span<const std::byte> data) {
  request_.append(reinterpret_cast<const char*>(data.data()), data.size());
  return net::IoResult{net::IoStatus::Ready, data.size()};
}

net::IoResult ScriptedTransport::Receive(std::span<std::byte> out) {
  if (!sent_) {
    factory_.log.requests[index_] = request_;
    sent_ = true;
  }
  if (!released_) {
    return net::IoResult{net::IoStatus::Blocked, 0};
  }
  if (pending_.empty()) {
    return net::IoResult{net::IoStatus::Closed, 0};
  }
  const std::size_t take = std::min(out.size(), pending_.size());
  std::memcpy(out.data(), pending_.data(), take);
  pending_.erase(0, take);
  return net::IoResult{net::IoStatus::Ready, take};
}

void ScriptedTransport::Close() { factory_.Forget(*this); }

std::optional<util::WaitDescriptor> ScriptedTransport::Interest() const {
  // Nothing to wait on: this connection has no descriptor, and claiming one
  // would make the loop block on a number that never becomes readable.
  // `RequestQueue::HasRunnableWork` is what keeps the loop turning for it.
  return std::nullopt;
}

std::unique_ptr<net::Transport> ScriptedTransport::Factory::Create() {
  return std::make_unique<ScriptedTransport>(*this);
}

void ScriptedTransport::Factory::Register(ScriptedTransport& transport) {
  if (std::find(live_.begin(), live_.end(), &transport) == live_.end()) {
    live_.push_back(&transport);
  }
}

void ScriptedTransport::Factory::Forget(ScriptedTransport& transport) {
  live_.erase(std::remove(live_.begin(), live_.end(), &transport), live_.end());
}

bool ScriptedTransport::Factory::Release(std::string_view needle) {
  for (ScriptedTransport* transport : live_) {
    if (!transport->Released() && transport->Request().find(needle) != std::string::npos) {
      transport->Release();
      return true;
    }
  }
  return false;
}

std::size_t ScriptedTransport::Factory::ReleaseAll() {
  std::size_t released = 0;
  for (ScriptedTransport* transport : live_) {
    if (!transport->Released()) {
      transport->Release();
      ++released;
    }
  }
  return released;
}

std::size_t ScriptedTransport::Factory::Held() const {
  return static_cast<std::size_t>(std::count_if(
      live_.begin(), live_.end(),
      [](const ScriptedTransport* transport) { return !transport->Released(); }));
}

std::string OkResponse(std::string_view content_type, std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

}  // namespace microbrowser::tests
