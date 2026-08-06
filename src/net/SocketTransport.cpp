#include "net/SocketTransport.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// A monotonic millisecond count, for the resolver cache's TTL. Steady rather
// than wall-clock: a cache entry that expired because the system clock was
// stepped backwards would be a stall nobody could reproduce.
std::int64_t NowMonotonicMs() {
  const auto since = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
}

// One TLS context for the process, built once. Loading the system trust store
// is expensive and its result does not vary; rebuilding it per connection would
// be the single largest cost of a page load.
SSL_CTX* SharedContext(const std::string& ca_bundle_path) {
  static SSL_CTX* context = [&] {
    SSL_CTX* created = SSL_CTX_new(TLS_client_method());
    if (created == nullptr) {
      return created;
    }
    // TLS 1.2 floor. 1.0 and 1.1 are deprecated and their remaining users are
    // servers nobody has touched in a decade, which is not a reason to keep
    // speaking them.
    SSL_CTX_set_min_proto_version(created, TLS1_2_VERSION);
    SSL_CTX_set_options(created, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    // Verification is not optional and there is no flag that turns it off.
    SSL_CTX_set_verify(created, SSL_VERIFY_PEER, nullptr);
    if (!ca_bundle_path.empty()) {
      SSL_CTX_load_verify_locations(created, ca_bundle_path.c_str(), nullptr);
    } else {
      SSL_CTX_set_default_verify_paths(created);
    }
    // Session tickets off. A ticket is a server-assigned identifier that
    // survives a cookie clear — ADR 0005 lists it as one of the two rows people
    // forget. Resumption returns when the ticket cache is partitioned, not
    // before.
    SSL_CTX_set_options(created, SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(created, SSL_SESS_CACHE_OFF);
    // ALPN, in the wire format the extension uses: a length byte then the
    // name, most preferred first. Offering `h2` is ADR 0010 §3 and it is one
    // line here because the negotiation is the server's to make -- everything
    // hard about HTTP/2 is what happens after it answers.
    //
    // `http/1.1` is offered beside it and is not a fallback to be removed
    // later: plenty of the web still speaks it, and a client that offered only
    // `h2` would fail to connect to those servers rather than talk to them.
    static constexpr unsigned char kAlpn[] = {2, 'h', '2', 8, 'h', 't', 't',
                                              'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(created, kAlpn, sizeof(kAlpn));
    return created;
  }();
  return context;
}

class SocketTransport : public Transport {
 public:
  SocketTransport(SocketTransportFactory::Options options, ResolverCache& resolver)
      : options_(std::move(options)), resolver_(&resolver) {}

  ~SocketTransport() override { Close(); }

  bool StartConnect(std::string_view partition, std::string_view host, std::uint16_t port,
                    bool secure) override {
    Close();
    host_ = std::string(host);
    secure_ = secure;

    const std::vector<ResolvedAddress>* addresses = Resolve(partition, host, port);
    if (addresses == nullptr) {
      AddPerformanceCounter(PerfCounterId::NetConnectFailures);
      stage_ = Stage::Failed;
      return false;
    }

    for (const ResolvedAddress& entry : *addresses) {
      // SOCK_CLOEXEC and SOCK_NONBLOCK on the creating call. A follow-up fcntl
      // leaves a window in which a fork inherits the descriptor, and the
      // architecture lint rejects that form outright.
      const int fd = ::socket(entry.family, entry.socket_type | SOCK_CLOEXEC | SOCK_NONBLOCK,
                              entry.protocol);
      if (fd < 0) {
        continue;
      }
      const int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      // On a non-blocking socket a connect that has not finished is the normal
      // answer, not a failure: EINPROGRESS means "under way", which is exactly
      // what this method promises to return.
      const auto* at = reinterpret_cast<const sockaddr*>(&entry.address);
      if (::connect(fd, at, entry.address_length) == 0) {
        fd_ = fd;
        stage_ = secure_ ? Stage::Handshaking : Stage::Open;
        break;
      }
      if (errno == EINPROGRESS) {
        fd_ = fd;
        stage_ = Stage::Connecting;
        break;
      }
      ::close(fd);
    }

    if (fd_ < 0) {
      AddPerformanceCounter(PerfCounterId::NetConnectFailures);
      stage_ = Stage::Failed;
      return false;
    }
    AddPerformanceCounter(PerfCounterId::NetConnections);
    // Wait for writability first whatever happened above: a connect that
    // completed immediately still has nothing to read.
    want_read_ = false;
    want_write_ = true;
    return true;
  }

  IoStatus Advance() override {
    switch (stage_) {
      case Stage::Idle:
      case Stage::Failed:
        return IoStatus::Failed;
      case Stage::ClosedByPeer:
        return IoStatus::Closed;
      case Stage::Connecting:
        return FinishConnect();
      case Stage::Handshaking:
        return Handshake();
      case Stage::Open:
        return IoStatus::Ready;
    }
    return IoStatus::Failed;
  }

  IoResult Send(std::span<const std::byte> data) override {
    if (const IoStatus ready = Advance(); ready != IoStatus::Ready) {
      return IoResult{ready, 0};
    }
    if (data.empty()) {
      return IoResult{IoStatus::Ready, 0};
    }
    const auto* at = reinterpret_cast<const char*>(data.data());
    const ssize_t wrote = ssl_ != nullptr ? SSL_write(ssl_, at, static_cast<int>(data.size()))
                                          : ::send(fd_, at, data.size(), MSG_NOSIGNAL);
    if (wrote > 0) {
      want_write_ = static_cast<std::size_t>(wrote) < data.size();
      return IoResult{IoStatus::Ready, static_cast<std::size_t>(wrote)};
    }
    return IoResult{ClassifyIoFailure(wrote, /*writing=*/true), 0};
  }

  IoResult Receive(std::span<std::byte> out) override {
    if (const IoStatus ready = Advance(); ready != IoStatus::Ready) {
      return IoResult{ready, 0};
    }
    if (out.empty()) {
      return IoResult{IoStatus::Ready, 0};
    }
    const ssize_t read = ssl_ != nullptr
                             ? SSL_read(ssl_, out.data(), static_cast<int>(out.size()))
                             : ::recv(fd_, out.data(), out.size(), 0);
    if (read > 0) {
      want_read_ = true;
      want_write_ = false;
      return IoResult{IoStatus::Ready, static_cast<std::size_t>(read)};
    }
    if (read == 0 && ssl_ == nullptr) {
      stage_ = Stage::ClosedByPeer;
      return IoResult{IoStatus::Closed, 0};
    }
    return IoResult{ClassifyIoFailure(read, /*writing=*/false), 0};
  }

  void Close() override {
    if (ssl_ != nullptr) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    stage_ = Stage::Idle;
    protocol_.clear();
    want_read_ = false;
    want_write_ = false;
  }

  std::string_view NegotiatedProtocol() const override { return protocol_; }

  std::optional<util::WaitDescriptor> Interest() const override {
    if (fd_ < 0 || stage_ == Stage::Idle || stage_ == Stage::Failed ||
        stage_ == Stage::ClosedByPeer) {
      return std::nullopt;
    }
    util::WaitDescriptor descriptor;
    descriptor.descriptor = fd_;
    descriptor.readable = want_read_;
    descriptor.writable = want_write_;
    if (!descriptor.readable && !descriptor.writable) {
      // An open connection with no stated want is one waiting for a response.
      // Reporting nothing here would drop it out of the loop's wait and hang
      // the load, which is the failure mode this whole design has to not have.
      descriptor.readable = true;
    }
    return descriptor;
  }

 private:
  enum class Stage {
    Idle,
    Connecting,
    Handshaking,
    Open,
    ClosedByPeer,
    Failed,
  };

  // True when the descriptor is past the point where connect() can still be in
  // flight. A zero-timeout poll is not a busy wait: it is the readiness check
  // that follows the loop's real wait, and it is what turns "the loop woke up"
  // into "this particular socket is the one that woke it".
  bool ConnectSettled() const {
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLOUT;
    return ::poll(&descriptor, 1, 0) > 0;
  }

  IoStatus FinishConnect() {
    if (!ConnectSettled()) {
      want_read_ = false;
      want_write_ = true;
      return IoStatus::Blocked;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
      AddPerformanceCounter(PerfCounterId::NetConnectFailures);
      stage_ = Stage::Failed;
      return IoStatus::Failed;
    }
    stage_ = secure_ ? Stage::Handshaking : Stage::Open;
    return stage_ == Stage::Open ? IoStatus::Ready : Handshake();
  }

  IoStatus Handshake() {
    if (ssl_ == nullptr && !StartTls()) {
      return IoStatus::Failed;
    }
    const int result = SSL_connect(ssl_);
    if (result == 1) {
      if (SSL_get_verify_result(ssl_) != X509_V_OK) {
        AddPerformanceCounter(PerfCounterId::NetTlsFailures);
        stage_ = Stage::Failed;
        return IoStatus::Failed;
      }
      AddPerformanceCounter(PerfCounterId::NetTlsHandshakes);
      // Read once, here, rather than through OpenSSL on every call: the answer
      // cannot change after the handshake, and `NegotiatedProtocol` is asked on
      // a path where a `const` method reaching into a live SSL object would be
      // a lie about what it does.
      const unsigned char* selected = nullptr;
      unsigned int selected_length = 0;
      SSL_get0_alpn_selected(ssl_, &selected, &selected_length);
      if (selected != nullptr && selected_length > 0) {
        protocol_.assign(reinterpret_cast<const char*>(selected), selected_length);
      }
      stage_ = Stage::Open;
      want_read_ = false;
      want_write_ = true;
      return IoStatus::Ready;
    }
    const int error = SSL_get_error(ssl_, result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      want_read_ = error == SSL_ERROR_WANT_READ;
      want_write_ = error == SSL_ERROR_WANT_WRITE;
      return IoStatus::Blocked;
    }
    AddPerformanceCounter(PerfCounterId::NetTlsFailures);
    stage_ = Stage::Failed;
    return IoStatus::Failed;
  }

  // Turns a non-positive return from send/recv or SSL_read/SSL_write into the
  // one of three answers the caller acts on. Kept in one place because the
  // difference between "wait" and "give up" is the difference between a load
  // that finishes and a browser that spins.
  IoStatus ClassifyIoFailure(ssize_t result, bool writing) {
    if (ssl_ != nullptr) {
      const int error = SSL_get_error(ssl_, static_cast<int>(result));
      if (error == SSL_ERROR_WANT_READ) {
        want_read_ = true;
        want_write_ = false;
        return IoStatus::Blocked;
      }
      if (error == SSL_ERROR_WANT_WRITE) {
        want_read_ = false;
        want_write_ = true;
        return IoStatus::Blocked;
      }
      if (error == SSL_ERROR_ZERO_RETURN) {
        stage_ = Stage::ClosedByPeer;
        return IoStatus::Closed;
      }
      // A server that drops the connection without a close_notify is a
      // truncated response, not a clean close. Treating it as clean is how a
      // truncation attack becomes an accepted document.
      if (error == SSL_ERROR_SYSCALL && result == 0) {
        stage_ = Stage::Failed;
        return IoStatus::Failed;
      }
      stage_ = Stage::Failed;
      return IoStatus::Failed;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      want_read_ = !writing;
      want_write_ = writing;
      return IoStatus::Blocked;
    }
    stage_ = Stage::Failed;
    return IoStatus::Failed;
  }

  // The addresses for one name, from the cache when it has them and from
  // `getaddrinfo` when it does not. Null means the name did not resolve.
  //
  // `getaddrinfo` is the one call in this file that blocks, and the reason it is
  // called out here rather than quietly left: it has no non-blocking form.
  // Removing it entirely needs either a thread (rejected in ADR 0011, and the
  // reason is written there) or a resolver library, which is a third-party
  // dependency and therefore an ADR of its own. What the cache changes is how
  // often it is reached: measured at four to six times *per host* on a single
  // page load, because a connection is opened per concurrent request and each
  // one resolved from scratch.
  //
  // Scoped and labelled with the host, because a resolve is a *main-thread
  // stall* rather than CPU: a summary that did not name it attributed the time
  // to whatever scope happened to enclose the connect.
  const std::vector<ResolvedAddress>* Resolve(std::string_view partition, std::string_view host,
                                              std::uint16_t port) {
    const std::int64_t now_ms = NowMonotonicMs();
    if (const std::vector<ResolvedAddress>* cached =
            resolver_->Lookup(partition, host, port, now_ms)) {
      return cached;
    }

    util::PerformanceTrace::ScopeLabel label("net::Resolve");
    label.Field("host", host);
    util::PerformanceTrace::Scope scope(label.View());

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int failed = ::getaddrinfo(host_.c_str(), port_text.c_str(), &hints, &results);
    AddPerformanceCounter(PerfCounterId::NetHostResolves);
    if (failed != 0) {
      return nullptr;
    }
    std::vector<ResolvedAddress> addresses;
    for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
      // Anything whose address does not fit a `sockaddr_storage` is not an
      // address this can connect to anyway, and copying it would be the buffer
      // overrun. `sockaddr_storage` is defined to be big enough for every family
      // the system supports, so this is a guard rather than a filter.
      if (entry->ai_addrlen > sizeof(sockaddr_storage)) {
        continue;
      }
      ResolvedAddress address;
      address.family = entry->ai_family;
      address.socket_type = entry->ai_socktype;
      address.protocol = entry->ai_protocol;
      address.address_length = entry->ai_addrlen;
      std::memcpy(&address.address, entry->ai_addr, entry->ai_addrlen);
      addresses.push_back(address);
    }
    ::freeaddrinfo(results);
    if (addresses.empty()) {
      return nullptr;
    }
    resolver_->Store(partition, host, port, std::move(addresses), now_ms);
    return resolver_->Lookup(partition, host, port, now_ms);
  }

  bool StartTls() {
    SSL_CTX* context = SharedContext(options_.ca_bundle_path);
    if (context == nullptr) {
      stage_ = Stage::Failed;
      return false;
    }
    ssl_ = SSL_new(context);
    if (ssl_ == nullptr) {
      stage_ = Stage::Failed;
      return false;
    }
    SSL_set_fd(ssl_, fd_);

    // SNI, and — the part that is actually load-bearing — hostname
    // verification. `SSL_VERIFY_PEER` alone checks that the certificate chains
    // to a trusted root; it does *not* check that the certificate is for the
    // host being talked to. Without this call, any valid certificate for any
    // domain authenticates any server, which is the classic way to hold TLS
    // wrongly.
    // OpenSSL spells SNI as a macro that expands to an old-style cast, so the
    // warning is silenced for exactly this call rather than for the file.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    SSL_set_tlsext_host_name(ssl_, host_.c_str());
#pragma GCC diagnostic pop
    SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(ssl_, host_.c_str()) != 1) {
      stage_ = Stage::Failed;
      return false;
    }
    return true;
  }

  SocketTransportFactory::Options options_;
  // Borrowed from the factory, which outlives every transport it made.
  ResolverCache* resolver_;
  std::string host_;
  // What ALPN settled on, captured the moment the handshake finished. Cleared
  // by `Close`, because a transport that has been closed and reconnected has
  // negotiated again.
  std::string protocol_;
  int fd_ = -1;
  SSL* ssl_ = nullptr;
  Stage stage_ = Stage::Idle;
  bool secure_ = false;
  bool want_read_ = false;
  bool want_write_ = false;
};

}  // namespace

std::unique_ptr<Transport> SocketTransportFactory::Create() {
  return std::make_unique<SocketTransport>(options_, resolver_);
}

bool TlsIsAvailable() {
  return SharedContext(std::string()) != nullptr;
}

}  // namespace microbrowser::net
