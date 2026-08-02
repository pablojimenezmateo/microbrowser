#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::url {

// A URL host, which is one of four things and not merely a string.
//
// Keeping the kinds apart is a security property rather than tidiness. "Is this
// request going to a private address?" and "do these two URLs have the same
// origin?" are both questions about the *parsed* host, and both have been
// answered wrongly by code comparing strings — `http://127.0.0.1` and
// `http://2130706433` and `http://0x7f.1` are the same address written three
// ways, and a string comparison says they are three hosts.
class Host {
 public:
  enum class Kind : std::uint8_t {
    Empty,
    Domain,   // a registrable name, ASCII, lowercased
    Ipv4,     // always compared and serialized in canonical dotted form
    Ipv6,
    Opaque,   // a non-special scheme's host: no interpretation, only escaping
  };

  Host() = default;

  // Parses per the URL Standard's host parser. `is_special` selects between the
  // domain path (percent-decode, then IPv4 detection) and the opaque path.
  static std::optional<Host> Parse(std::string_view input, bool is_special);

  Kind GetKind() const { return kind_; }
  bool IsEmpty() const { return kind_ == Kind::Empty; }
  bool IsIpAddress() const { return kind_ == Kind::Ipv4 || kind_ == Kind::Ipv6; }

  // Canonical serialization: what goes back into a URL, and what two hosts are
  // compared by. IPv6 carries its brackets here.
  const std::string& Serialized() const { return serialized_; }

  // Host address in numeric form, valid only for Ipv4.
  std::uint32_t Ipv4Address() const { return ipv4_; }

  // True for a loopback, link-local, or private-range address, and for
  // "localhost". The network layer needs this before it connects: a page
  // resolving a name it controls to 127.0.0.1 is the DNS rebinding attack, and
  // the check belongs on the parsed host rather than on the name.
  bool IsPotentiallyPrivate() const;

  // True only for loopback addresses and localhost names. Secure-context
  // trust uses this narrower carve-out; private LAN addresses are not enough.
  bool IsLoopbackOrLocalhost() const;

  friend bool operator==(const Host&, const Host&) = default;

 private:
  Kind kind_ = Kind::Empty;
  std::string serialized_;
  std::uint32_t ipv4_ = 0;
};

}  // namespace microbrowser::url
