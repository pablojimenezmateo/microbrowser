#include "net/SocketTransport.h"

#include <cerrno>
#include <cstring>
#include <string>

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

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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
    return created;
  }();
  return context;
}

class SocketTransport : public Transport {
 public:
  explicit SocketTransport(SocketTransportFactory::Options options)
      : options_(std::move(options)) {}

  ~SocketTransport() override { Close(); }

  bool Connect(std::string_view host, std::uint16_t port, bool secure) override {
    Close();
    const std::string host_text(host);
    const std::string port_text = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (::getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &results) != 0) {
      AddPerformanceCounter(PerfCounterId::NetConnectFailures);
      return false;
    }

    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
      // SOCK_CLOEXEC on the creating call. A follow-up fcntl leaves a window in
      // which a fork inherits the descriptor, and the architecture lint rejects
      // that form outright.
      const int fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC,
                              entry->ai_protocol);
      if (fd < 0) {
        continue;
      }
      const int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
        fd_ = fd;
        break;
      }
      ::close(fd);
    }
    ::freeaddrinfo(results);

    if (fd_ < 0) {
      AddPerformanceCounter(PerfCounterId::NetConnectFailures);
      return false;
    }
    AddPerformanceCounter(PerfCounterId::NetConnections);
    return secure ? StartTls(host_text) : true;
  }

  bool Send(std::span<const std::byte> data) override {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const auto* at = reinterpret_cast<const char*>(data.data()) + sent;
      const std::size_t remaining = data.size() - sent;
      const ssize_t wrote = ssl_ != nullptr
                                ? SSL_write(ssl_, at, static_cast<int>(remaining))
                                : ::send(fd_, at, remaining, MSG_NOSIGNAL);
      if (wrote <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(wrote);
    }
    return true;
  }

  std::optional<std::size_t> Receive(std::span<std::byte> out) override {
    if (out.empty()) {
      return std::size_t{0};
    }
    if (!WaitReadable()) {
      return std::nullopt;
    }
    const ssize_t read = ssl_ != nullptr
                             ? SSL_read(ssl_, out.data(), static_cast<int>(out.size()))
                             : ::recv(fd_, out.data(), out.size(), 0);
    if (read < 0) {
      if (ssl_ != nullptr) {
        const int error = SSL_get_error(ssl_, static_cast<int>(read));
        if (error == SSL_ERROR_ZERO_RETURN) {
          return std::size_t{0};
        }
      }
      return std::nullopt;
    }
    return static_cast<std::size_t>(read);
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
  }

 private:
  bool WaitReadable() const {
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, options_.io_timeout_ms);
    return ready > 0;
  }

  bool StartTls(const std::string& host) {
    SSL_CTX* context = SharedContext(options_.ca_bundle_path);
    if (context == nullptr) {
      return false;
    }
    ssl_ = SSL_new(context);
    if (ssl_ == nullptr) {
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
    SSL_set_tlsext_host_name(ssl_, host.c_str());
#pragma GCC diagnostic pop
    SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(ssl_, host.c_str()) != 1) {
      Close();
      return false;
    }

    if (SSL_connect(ssl_) != 1) {
      AddPerformanceCounter(PerfCounterId::NetTlsFailures);
      Close();
      return false;
    }
    if (SSL_get_verify_result(ssl_) != X509_V_OK) {
      AddPerformanceCounter(PerfCounterId::NetTlsFailures);
      Close();
      return false;
    }
    AddPerformanceCounter(PerfCounterId::NetTlsHandshakes);
    return true;
  }

  SocketTransportFactory::Options options_;
  int fd_ = -1;
  SSL* ssl_ = nullptr;
};

}  // namespace

std::unique_ptr<Transport> SocketTransportFactory::Create() {
  return std::make_unique<SocketTransport>(options_);
}

bool TlsIsAvailable() {
  return SharedContext(std::string()) != nullptr;
}

}  // namespace microbrowser::net
