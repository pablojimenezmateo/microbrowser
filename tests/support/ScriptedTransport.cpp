#include "support/ScriptedTransport.h"

#include <algorithm>
#include <cstring>

namespace microbrowser::tests {

bool ScriptedTransport::Connect(std::string_view host, std::uint16_t port, bool secure) {
  if (cursor_ >= script_.size()) {
    return false;
  }
  const Exchange& exchange = script_[cursor_];
  if (!exchange.expected_host.empty() && exchange.expected_host != host) {
    return false;
  }
  if (exchange.expected_port != 0 && exchange.expected_port != port) {
    return false;
  }
  if (exchange.expected_secure != secure) {
    return false;
  }
  log_.hosts.emplace_back(host);
  log_.secure.push_back(secure);
  pending_ = exchange.response;
  return true;
}

bool ScriptedTransport::Send(std::span<const std::byte> data) {
  request_.append(reinterpret_cast<const char*>(data.data()), data.size());
  return true;
}

std::optional<std::size_t> ScriptedTransport::Receive(std::span<std::byte> out) {
  if (!sent_) {
    log_.requests.push_back(request_);
    sent_ = true;
  }
  if (pending_.empty()) {
    return std::size_t{0};  // peer closed
  }
  const std::size_t take = std::min(out.size(), pending_.size());
  std::memcpy(out.data(), pending_.data(), take);
  pending_.erase(0, take);
  return take;
}

void ScriptedTransport::Close() { ++cursor_; }

std::string OkResponse(std::string_view content_type, std::string_view body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

}  // namespace microbrowser::tests
