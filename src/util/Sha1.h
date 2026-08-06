#pragma once

#include <string>
#include <string_view>

namespace microbrowser::util {

// SHA-1 (FIPS 180-4), for exactly one caller.
//
// **It is here under protest and the protest is the documentation.** SHA-1 is broken
// for every purpose that depends on collision resistance, and nothing in this browser
// may use it for one. The single legitimate use is RFC 6455's `Sec-WebSocket-Accept`:
// the handshake takes SHA-1 of the client's key plus a fixed GUID and the server must
// echo it back, which is a *protocol handshake check* and not a security claim. It
// proves the peer speaks WebSocket rather than that it is trustworthy -- TLS is what
// makes it trustworthy, and a `wss://` connection has already done that work.
//
// So: no other caller. If a second one appears, the question to ask is what it thinks
// SHA-1 gives it, and the answer is almost certainly "a collision-resistant digest",
// which this is not. `util::Sha2` is next to it and is what that caller wants.
//
// Bytes in, twenty raw digest bytes out. Callers that want text encode with
// util::Base64Encode.
std::string Sha1(std::string_view data);

}  // namespace microbrowser::util
