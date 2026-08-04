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
//
// Since ADR 0011 the socket is non-blocking and the connection is a state
// machine: `StartConnect` begins, `Advance` carries the TCP connect and the TLS
// handshake forward a step at a time, and `Interest` says which way the loop
// should be watching while none of that has finished.
//
// **One call still blocks: `getaddrinfo`.** There is no non-blocking form of
// it, and the two ways out are a thread (rejected in ADR 0011, with the reason
// written there) or a resolver library, which is a third-party dependency and
// so an ADR of its own. It costs one blocking call per *host* rather than per
// resource, which is why it was not worth either to land the rest.
class SocketTransportFactory : public TransportFactory {
 public:
  struct Options {
    // No timeouts here any more, and their absence is the point: a transport
    // that never blocks has nothing to time out. Giving up on a request that
    // has stopped making progress is a decision about the *request*, and it
    // moved to RequestQueue with the rest of the scheduling — see ADR 0011.
    //
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
