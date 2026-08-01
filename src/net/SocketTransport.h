#pragma once

#include <memory>
#include <string>

#include "net/Transport.h"

namespace microbrowser::net {

// The real transport: a TCP socket, with TLS on top when the scheme asked for
// it.
//
// The only file in the project that opens a socket, and the only one that names
// OpenSSL. ADR 0001 sanctions OpenSSL for the TLS record layer *only* — we own
// HTTP entirely — and `extern: openssl` in this module's manifest is what keeps
// that true rather than aspirational.
//
// Everything above `Transport` was written and tested before this existed, and
// that ordering was on purpose: redirect handling, cookie round trips, cache
// behavior and policy re-entry are all logic, and logic that can only be
// exercised by talking to a real server is logic that does not get exercised.
class SocketTransportFactory : public TransportFactory {
 public:
  struct Options {
    int connect_timeout_ms = 15000;
    int io_timeout_ms = 30000;
    // Certificate verification. There is no setting that disables it — a
    // "skip verification" flag is a flag somebody eventually ships enabled,
    // and an unverified TLS connection is a plaintext connection that looks
    // encrypted. A caller who needs a private CA supplies its path instead.
    std::string ca_bundle_path;
  };

  SocketTransportFactory() = default;
  explicit SocketTransportFactory(Options options) : options_(std::move(options)) {}

  std::unique_ptr<Transport> Create() override;

 private:
  Options options_;
};

// True when this build has TLS compiled in. A test asserts it, so a build that
// silently lost OpenSSL cannot pass while quietly refusing every https URL.
bool TlsIsAvailable();

}  // namespace microbrowser::net
