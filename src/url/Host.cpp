#include "url/Host.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "util/PercentEncoding.h"

namespace microbrowser::url {

namespace {

// Forbidden host code points, from the URL Standard. A host containing one of
// these is a parse failure rather than something to escape: these are the
// characters that would change which part of the URL the parser thought it was
// reading, so accepting them is how a URL comes to mean two things.
bool IsForbiddenHostCodePoint(char c) {
  switch (c) {
    case '\0':
    case '\t':
    case '\n':
    case '\r':
    case ' ':
    case '#':
    case '/':
    case ':':
    case '<':
    case '>':
    case '?':
    case '@':
    case '[':
    case '\\':
    case ']':
    case '^':
    case '|':
      return true;
    default:
      return false;
  }
}

bool IsForbiddenDomainCodePoint(char c) {
  return IsForbiddenHostCodePoint(c) || static_cast<unsigned char>(c) <= 0x1F || c == '%' ||
         static_cast<unsigned char>(c) == 0x7F;
}

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// One dotted part of an IPv4 address. The three radixes are not a quirk to be
// tolerated but part of the format: `0x7f.1` and `2130706433` and `127.0.0.1`
// are the same address, and any code that decides whether an address is private
// has to agree with the code that connects to it about which one it is.
struct Ipv4Number {
  std::uint64_t value = 0;
  bool valid = false;
};

Ipv4Number ParseIpv4Number(std::string_view input) {
  Ipv4Number result;
  if (input.empty()) {
    return result;
  }
  int radix = 10;
  if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
    input.remove_prefix(2);
    radix = 16;
  } else if (input.size() >= 2 && input[0] == '0') {
    input.remove_prefix(1);
    radix = 8;
  }
  if (input.empty()) {
    result.valid = true;  // "0", "0x" and "00" are all zero
    return result;
  }

  std::uint64_t value = 0;
  for (const char c : input) {
    const int digit = HexValue(c);
    if (digit < 0 || digit >= radix) {
      return result;
    }
    value = value * static_cast<std::uint64_t>(radix) + static_cast<std::uint64_t>(digit);
    // Bounded as it accumulates rather than after. A host of five hundred
    // digits must not spend five hundred multiplications wrapping around.
    if (value > 0xFFFFFFFFull) {
      return result;
    }
  }
  result.value = value;
  result.valid = true;
  return result;
}

// Whether a domain "ends in a number", which is the standard's test for
// treating it as IPv4 rather than as a name.
bool EndsInNumber(std::string_view input) {
  if (input.empty()) {
    return false;
  }
  if (input.back() == '.') {
    input.remove_suffix(1);
  }
  const std::size_t dot = input.rfind('.');
  const std::string_view last = dot == std::string_view::npos ? input : input.substr(dot + 1);
  if (last.empty()) {
    return false;
  }
  if (std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return true;
  }
  return ParseIpv4Number(last).valid;
}

std::optional<std::uint32_t> ParseIpv4(std::string_view input) {
  if (!input.empty() && input.back() == '.') {
    input.remove_suffix(1);
  }
  std::array<std::uint64_t, 4> parts{};
  std::size_t count = 0;
  std::size_t start = 0;
  while (true) {
    const std::size_t dot = input.find('.', start);
    const std::string_view part =
        dot == std::string_view::npos ? input.substr(start) : input.substr(start, dot - start);
    if (count >= 4) {
      return std::nullopt;
    }
    const Ipv4Number number = ParseIpv4Number(part);
    if (!number.valid) {
      return std::nullopt;
    }
    parts[count++] = number.value;
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }

  // Every part but the last must fit in a byte; the last absorbs the rest, so
  // `127.1` is 127.0.0.1 and `2130706433` is the whole address.
  for (std::size_t i = 0; i + 1 < count; ++i) {
    if (parts[i] > 255) {
      return std::nullopt;
    }
  }
  const std::uint64_t last = parts[count - 1];
  if (last >= (1ull << (8 * (5 - count)))) {
    return std::nullopt;
  }

  std::uint32_t address = static_cast<std::uint32_t>(last);
  for (std::size_t i = 0; i + 1 < count; ++i) {
    address |= static_cast<std::uint32_t>(parts[i]) << (8 * (3 - i));
  }
  return address;
}

std::string SerializeIpv4(std::uint32_t address) {
  std::string out;
  for (int i = 3; i >= 0; --i) {
    out += std::to_string((address >> (8 * i)) & 0xFFu);
    if (i > 0) {
      out.push_back('.');
    }
  }
  return out;
}

bool IsLoopbackIpv4(std::uint32_t address) {
  return ((address >> 24) & 0xFFu) == 127;
}

bool IsPotentiallyPrivateIpv4(std::uint32_t address) {
  const std::uint32_t a = (address >> 24) & 0xFFu;
  const std::uint32_t b = (address >> 16) & 0xFFu;
  if (IsLoopbackIpv4(address) || a == 10 || a == 0) {
    return true;
  }
  if (a == 192 && b == 168) {
    return true;
  }
  if (a == 169 && b == 254) {
    return true;
  }
  return a == 172 && b >= 16 && b <= 31;
}

bool IsLocalhostName(std::string_view host) {
  if (host.size() > 1 && host.back() == '.') {
    host.remove_suffix(1);
  }
  if (host == "localhost") {
    return true;
  }
  constexpr std::string_view kSuffix = ".localhost";
  return host.size() > kSuffix.size() &&
         host.compare(host.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

std::optional<std::array<std::uint16_t, 8>> ParseIpv6(std::string_view input) {
  std::array<std::uint16_t, 8> pieces{};
  std::size_t piece_index = 0;
  std::optional<std::size_t> compress;
  std::size_t position = 0;

  const auto remaining = [&] { return position < input.size(); };

  if (remaining() && input[position] == ':') {
    if (position + 1 >= input.size() || input[position + 1] != ':') {
      return std::nullopt;
    }
    position += 2;
    compress = ++piece_index - 1;
  }

  while (remaining()) {
    if (piece_index == 8) {
      return std::nullopt;
    }
    if (input[position] == ':') {
      if (compress.has_value()) {
        return std::nullopt;
      }
      ++position;
      compress = piece_index;
      continue;
    }

    std::uint32_t value = 0;
    int length = 0;
    while (length < 4 && remaining() && HexValue(input[position]) >= 0) {
      value = value * 16 + static_cast<std::uint32_t>(HexValue(input[position]));
      ++position;
      ++length;
    }

    if (remaining() && input[position] == '.') {
      // An embedded IPv4 tail, which occupies the last two pieces.
      if (length == 0 || piece_index > 6) {
        return std::nullopt;
      }
      position -= static_cast<std::size_t>(length);
      int numbers = 0;
      while (remaining()) {
        std::uint32_t ipv4_piece = 0;
        bool have_digit = false;
        if (numbers > 0) {
          if (input[position] != '.' || numbers >= 4) {
            return std::nullopt;
          }
          ++position;
        }
        if (!remaining() || input[position] < '0' || input[position] > '9') {
          return std::nullopt;
        }
        while (remaining() && input[position] >= '0' && input[position] <= '9') {
          const std::uint32_t digit = static_cast<std::uint32_t>(input[position] - '0');
          if (have_digit && ipv4_piece == 0) {
            return std::nullopt;  // a leading zero is not allowed here
          }
          ipv4_piece = ipv4_piece * 10 + digit;
          if (ipv4_piece > 255) {
            return std::nullopt;
          }
          have_digit = true;
          ++position;
        }
        pieces[piece_index] = static_cast<std::uint16_t>(pieces[piece_index] * 0x100 + ipv4_piece);
        ++numbers;
        if (numbers == 2 || numbers == 4) {
          ++piece_index;
        }
        if (numbers == 4) {
          break;
        }
      }
      if (numbers != 4) {
        return std::nullopt;
      }
      break;
    }

    if (remaining() && input[position] == ':') {
      ++position;
      if (!remaining()) {
        return std::nullopt;
      }
    } else if (remaining()) {
      return std::nullopt;
    }
    if (length == 0) {
      return std::nullopt;
    }
    pieces[piece_index++] = static_cast<std::uint16_t>(value);
  }

  if (compress.has_value()) {
    const std::size_t swaps = piece_index - *compress;
    piece_index = 7;
    for (std::size_t i = 0; i < swaps; ++i) {
      std::swap(pieces[piece_index], pieces[*compress + swaps - 1 - i]);
      --piece_index;
    }
  } else if (piece_index != 8) {
    return std::nullopt;
  }
  return pieces;
}

// The standard's serializer, including the "compress the longest run of zeros"
// rule. Canonical output matters because two spellings of one address must
// compare equal, and they are compared as strings after this.
std::string SerializeIpv6(const std::array<std::uint16_t, 8>& pieces) {
  std::size_t best_start = 0;
  std::size_t best_length = 0;
  std::size_t run_start = 0;
  std::size_t run_length = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    if (pieces[i] == 0) {
      if (run_length == 0) {
        run_start = i;
      }
      ++run_length;
      if (run_length > best_length) {
        best_length = run_length;
        best_start = run_start;
      }
    } else {
      run_length = 0;
    }
  }
  // A single zero is written out; only a run of two or more is compressed.
  const bool compress = best_length > 1;

  std::string out = "[";
  bool ignore_zero = false;
  for (std::size_t i = 0; i < 8; ++i) {
    if (ignore_zero && pieces[i] == 0) {
      continue;
    }
    ignore_zero = false;
    if (compress && i == best_start) {
      out += i == 0 ? "::" : ":";
      ignore_zero = true;
      continue;
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%x", pieces[i]);
    out += buffer;
    if (i != 7) {
      out.push_back(':');
    }
  }
  if (out.back() == ':' && out.size() >= 2 && out[out.size() - 2] != ':') {
    out.pop_back();
  }
  out.push_back(']');
  return out;
}

std::optional<std::uint32_t> Ipv4MappedAddress(const std::array<std::uint16_t, 8>& pieces) {
  if (pieces[0] != 0 || pieces[1] != 0 || pieces[2] != 0 || pieces[3] != 0 || pieces[4] != 0 ||
      pieces[5] != 0xFFFFu) {
    return std::nullopt;
  }
  return (static_cast<std::uint32_t>(pieces[6]) << 16u) |
         static_cast<std::uint32_t>(pieces[7]);
}

}  // namespace

std::optional<Host> Host::Parse(std::string_view input, bool is_special) {
  Host host;

  if (!input.empty() && input.front() == '[') {
    if (input.back() != ']') {
      return std::nullopt;
    }
    const auto pieces = ParseIpv6(input.substr(1, input.size() - 2));
    if (!pieces.has_value()) {
      return std::nullopt;
    }
    host.kind_ = Kind::Ipv6;
    host.serialized_ = SerializeIpv6(*pieces);
    return host;
  }

  if (!is_special) {
    // An opaque host is not interpreted at all. It is checked for the code
    // points that would re-partition the URL, and otherwise escaped as-is.
    for (const char c : input) {
      if (IsForbiddenHostCodePoint(c)) {
        return std::nullopt;
      }
    }
    host.kind_ = input.empty() ? Kind::Empty : Kind::Opaque;
    host.serialized_ = util::PercentEncode(input, util::PercentEncodeSet::C0Control);
    return host;
  }

  if (input.empty()) {
    return std::nullopt;  // a special scheme requires a host
  }

  const std::string decoded = util::PercentDecode(input);
  std::string ascii;
  ascii.reserve(decoded.size());
  for (const char c : decoded) {
    // No IDNA. A non-ASCII domain is rejected rather than passed through,
    // because passing it through would mean the host that gets connected to is
    // not the host that was checked — which is the shape of every homograph
    // and every origin-confusion bug. Unicode domains need Punycode plus UTS
    // #46 mapping, and that lands with the rest of the Unicode tables.
    if (static_cast<unsigned char>(c) > 0x7F) {
      return std::nullopt;
    }
    if (IsForbiddenDomainCodePoint(c)) {
      return std::nullopt;
    }
    ascii.push_back(ToLower(c));
  }
  if (ascii.empty()) {
    return std::nullopt;
  }

  if (EndsInNumber(ascii)) {
    const auto address = ParseIpv4(ascii);
    if (!address.has_value()) {
      return std::nullopt;
    }
    host.kind_ = Kind::Ipv4;
    host.ipv4_ = *address;
    host.serialized_ = SerializeIpv4(*address);
    return host;
  }

  host.kind_ = Kind::Domain;
  host.serialized_ = std::move(ascii);
  return host;
}

bool Host::IsPotentiallyPrivate() const {
  switch (kind_) {
    case Kind::Ipv4:
      return IsPotentiallyPrivateIpv4(ipv4_);
    case Kind::Ipv6: {
      const auto pieces = ParseIpv6(std::string_view(serialized_).substr(1, serialized_.size() - 2));
      if (pieces.has_value()) {
        const std::optional<std::uint32_t> mapped_address = Ipv4MappedAddress(*pieces);
        if (mapped_address.has_value()) {
          return IsPotentiallyPrivateIpv4(*mapped_address);
        }
      }
      // ::1 and the unique-local and link-local ranges.
      return serialized_ == "[::1]" || serialized_.rfind("[fc", 0) == 0 ||
             serialized_.rfind("[fd", 0) == 0 || serialized_.rfind("[fe8", 0) == 0 ||
             serialized_.rfind("[fe9", 0) == 0 || serialized_.rfind("[fea", 0) == 0 ||
             serialized_.rfind("[feb", 0) == 0;
    }
    case Kind::Domain:
      return IsLocalhostName(serialized_);
    case Kind::Empty:
    case Kind::Opaque:
      return false;
  }
  return false;
}

bool Host::IsLoopbackOrLocalhost() const {
  switch (kind_) {
    case Kind::Ipv4:
      return IsLoopbackIpv4(ipv4_);
    case Kind::Ipv6: {
      if (serialized_ == "[::1]") {
        return true;
      }
      const auto pieces = ParseIpv6(std::string_view(serialized_).substr(1, serialized_.size() - 2));
      if (!pieces.has_value()) {
        return false;
      }
      const std::optional<std::uint32_t> mapped_address = Ipv4MappedAddress(*pieces);
      return mapped_address.has_value() && IsLoopbackIpv4(*mapped_address);
    }
    case Kind::Domain:
      return IsLocalhostName(serialized_);
    case Kind::Empty:
    case Kind::Opaque:
      return false;
  }
  return false;
}

}  // namespace microbrowser::url
