#include "support/Http2ScriptedServer.h"

#include <algorithm>
#include <cstring>

#include "net/Http2Frames.h"

namespace microbrowser::tests {

namespace {

using namespace microbrowser::net::http2;  // NOLINT(google-build-using-namespace)

std::string Frame(FrameType type, std::uint8_t flags, std::uint32_t stream,
                  std::string_view payload) {
  std::string out;
  WriteFrameHeader(type, flags, stream, payload.size(), out);
  out += payload;
  return out;
}

std::string_view ValueOf(const std::vector<net::hpack::Header>& fields, std::string_view name) {
  for (const net::hpack::Header& field : fields) {
    if (field.name == name) {
      return field.value;
    }
  }
  return {};
}

}  // namespace

Http2ScriptedTransport::~Http2ScriptedTransport() = default;

std::unique_ptr<net::Transport> Http2ScriptedTransport::Factory::Create() {
  return std::make_unique<Http2ScriptedTransport>(*this);
}

const Http2ScriptedTransport::Factory::Route* Http2ScriptedTransport::Factory::Find(
    std::string_view path) const {
  for (const Route& route : routes) {
    if (route.path == path) {
      return &route;
    }
  }
  return nullptr;
}

bool Http2ScriptedTransport::StartConnect(std::string_view, std::string_view, std::uint16_t,
                                          bool) {
  if (factory_ == nullptr) {
    return false;
  }
  ++factory_->connects;
  // The server's opening SETTINGS, which a real one sends immediately and
  // without waiting for the preface.
  to_client_ += Frame(FrameType::Settings, 0, 0, "");
  return true;
}

net::IoStatus Http2ScriptedTransport::Advance() {
  if (factory_ == nullptr || closed_) {
    return net::IoStatus::Failed;
  }
  // Still handshaking. `Blocked` is what a real socket says here for as many
  // turns of the loop as the round trips take, and it is during exactly those
  // turns that the rest of a page's requests arrive at the pool.
  if (factory_->handshake == Factory::Handshake::Held) {
    return net::IoStatus::Blocked;
  }
  return net::IoStatus::Ready;
}

std::string_view Http2ScriptedTransport::NegotiatedProtocol() const {
  if (factory_ == nullptr || !factory_->speaks_http2) {
    return {};
  }
  return "h2";
}

net::IoResult Http2ScriptedTransport::Send(std::span<const std::byte> data) {
  if (factory_ == nullptr || closed_) {
    return {net::IoStatus::Failed, 0};
  }
  from_client_.append(reinterpret_cast<const char*>(data.data()), data.size());
  ConsumeClientFrames();
  return {net::IoStatus::Ready, data.size()};
}

net::IoResult Http2ScriptedTransport::Receive(std::span<std::byte> out) {
  if (factory_ == nullptr || closed_) {
    return {net::IoStatus::Failed, 0};
  }
  if (read_at_ >= to_client_.size()) {
    return {net::IoStatus::Blocked, 0};
  }
  const std::size_t take = std::min(out.size(), to_client_.size() - read_at_);
  std::memcpy(out.data(), to_client_.data() + read_at_, take);
  read_at_ += take;
  return {net::IoStatus::Ready, take};
}

void Http2ScriptedTransport::Close() {
  closed_ = true;
}

std::optional<util::WaitDescriptor> Http2ScriptedTransport::Interest() const {
  // No descriptor, like every scripted transport here: handing the loop a
  // number that never becomes readable would be a lie that costs a wakeup.
  return std::nullopt;
}

void Http2ScriptedTransport::ConsumeClientFrames() {
  if (!saw_preface_) {
    if (from_client_.size() < kConnectionPreface.size()) {
      return;
    }
    if (from_client_.compare(0, kConnectionPreface.size(), kConnectionPreface) != 0) {
      closed_ = true;  // not HTTP/2 at all
      return;
    }
    from_client_.erase(0, kConnectionPreface.size());
    saw_preface_ = true;
  }

  std::size_t at = 0;
  while (true) {
    const std::span<const std::byte> rest(
        reinterpret_cast<const std::byte*>(from_client_.data()) + at, from_client_.size() - at);
    FrameHeader header;
    if (rest.size() < kFrameHeaderBytes || !ParseFrameHeader(rest, header) ||
        rest.size() < kFrameHeaderBytes + header.length) {
      break;
    }
    const std::span<const std::byte> payload = rest.subspan(kFrameHeaderBytes, header.length);
    at += kFrameHeaderBytes + header.length;

    switch (static_cast<FrameType>(header.type)) {
      case FrameType::Settings:
        if (!header.Has(flag::kAck)) {
          WriteSettingsAck(to_client_);
        }
        break;
      case FrameType::Ping:
        if (!header.Has(flag::kAck)) {
          WritePingAck(payload, to_client_);
        }
        break;
      case FrameType::Headers:
      case FrameType::Continuation: {
        std::span<const std::byte> block = payload;
        if (header.Is(FrameType::Headers)) {
          if (!StripPadding(header, payload, block)) {
            closed_ = true;
            return;
          }
          assembling_ = header.stream;
          block_.clear();
        }
        block_.append(reinterpret_cast<const char*>(block.data()), block.size());
        if (!header.Has(flag::kEndHeaders)) {
          break;
        }
        std::vector<net::hpack::Header> fields;
        const auto* bytes = reinterpret_cast<const std::byte*>(block_.data());
        if (!decoder_.Decode({bytes, block_.size()}, fields)) {
          closed_ = true;
          return;
        }
        Respond(assembling_, fields);
        assembling_ = 0;
        block_.clear();
        break;
      }
      default:
        // DATA, WINDOW_UPDATE, RST_STREAM, PRIORITY: read and dropped. A test
        // server does not need flow control, because it never sends enough to
        // run a window down.
        break;
    }
  }
  from_client_.erase(0, at);
}

void Http2ScriptedTransport::Respond(std::uint32_t stream,
                                     const std::vector<net::hpack::Header>& request) {
  const std::string path(ValueOf(request, ":path"));
  if (factory_ != nullptr) {
    factory_->paths.push_back(path);
  }
  const Factory::Route* route = factory_ != nullptr ? factory_->Find(path) : nullptr;

  std::vector<net::hpack::Header> fields;
  std::string body;
  if (route == nullptr) {
    fields.push_back({":status", "404"});
    body = "not found";
  } else {
    fields.push_back({":status", std::to_string(route->status)});
    if (!route->location.empty()) {
      fields.push_back({"location", route->location});
    }
    fields.push_back({"content-type", route->content_type});
    body = route->body;
  }
  fields.push_back({"content-length", std::to_string(body.size())});

  std::string block;
  net::hpack::Encode(fields, block);
  const std::uint8_t flags =
      static_cast<std::uint8_t>(flag::kEndHeaders | (body.empty() ? flag::kEndStream : 0));
  to_client_ += Frame(FrameType::Headers, flags, stream, block);
  if (!body.empty()) {
    to_client_ += Frame(FrameType::Data, flag::kEndStream, stream, body);
  }
}

}  // namespace microbrowser::tests
