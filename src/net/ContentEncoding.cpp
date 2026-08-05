#include "net/ContentEncoding.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Brotli.h"
#include "util/Inflate.h"
#include "util/PerformanceCounters.h"
#include "util/SaturatingMath.h"
#include "util/StringUtil.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

std::string Lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// The codings, outermost last, which is the order they were applied in and so
// the reverse of the order they come off in.
//
// `Content-Encoding` may appear more than once and each may be a list; the two
// forms mean the same thing, so both are flattened here rather than being two
// paths that could disagree.
std::vector<std::string> ParseCodings(const HttpResponse& response, std::size_t max_codings,
                                      bool& too_many) {
  std::vector<std::string> codings;
  too_many = false;
  for (const std::string_view field : response.headers.GetAll("content-encoding")) {
    std::string_view rest = field;
    while (true) {
      const std::size_t comma = rest.find(',');
      const std::string_view token = Trim(comma == std::string_view::npos ? rest
                                                                         : rest.substr(0, comma));
      if (!token.empty()) {
        if (codings.size() >= max_codings) {
          too_many = true;
          return codings;
        }
        codings.push_back(Lowered(token));
      }
      if (comma == std::string_view::npos) {
        break;
      }
      rest = rest.substr(comma + 1);
    }
  }
  return codings;
}

// What one step is allowed to produce. Both bounds at once, and the ratio
// applied to what actually arrived rather than to what the sender said arrived.
std::size_t AllowedOutput(std::size_t compressed, const DecodeLimits& limits) {
  const std::size_t by_ratio = util::SaturatingMul(compressed, limits.max_ratio);
  return std::min(limits.max_output, std::max(limits.min_output, by_ratio));
}

void RemoveHeader(HttpHeaders& headers, std::string_view name) {
  HttpHeaders kept;
  for (const HttpHeaders::Field& field : headers.Fields()) {
    if (!util::EqualsAsciiCaseInsensitive(field.name, name)) {
      kept.Add(field.name, field.value);
    }
  }
  headers = std::move(kept);
}

}  // namespace

DecodeStatus DecodeContentEncoding(HttpResponse& response, DecodeLimits limits) {
  bool too_many = false;
  const std::vector<std::string> codings = ParseCodings(response, limits.max_codings, too_many);
  if (too_many) {
    AddPerformanceCounter(PerfCounterId::NetContentDecodeFailures);
    return DecodeStatus::UnsupportedCoding;
  }
  if (std::all_of(codings.begin(), codings.end(),
                  [](const std::string& coding) { return coding == "identity"; })) {
    return DecodeStatus::Identity;
  }

  std::vector<std::byte> body = std::move(response.body);
  std::vector<std::byte> decoded;
  for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
    if (*coding == "identity") {
      continue;
    }
    const std::size_t allowed = AllowedOutput(body.size(), limits);
    bool ok = false;
    if (*coding == "gzip" || *coding == "x-gzip") {
      // The bomb is refused here, before a single byte is produced: a member
      // claiming more than the bound cannot be within it, and the claim is the
      // one number an attacker cannot make useful. A member that lies the other
      // way is caught by the checksum after the fact.
      const std::optional<std::uint32_t> declared = util::GzipDeclaredSize(body);
      if (declared.has_value() && static_cast<std::size_t>(*declared) > allowed) {
        AddPerformanceCounter(PerfCounterId::NetContentDecodeFailures);
        return DecodeStatus::TooLarge;
      }
      ok = util::GzipInflate(body, allowed, decoded);
    } else if (*coding == "br") {
      // Brotli, which is what the web actually serves: it is the default coding
      // for almost every static asset behind a CDN, and a browser that did not
      // advertise it received the *uncompressed* form of every one of them.
      //
      // No declared-size refusal is possible here, unlike gzip's ISIZE: a brotli
      // stream carries no output length, so the ceiling is enforced during the
      // decode and a bomb reads as `Malformed` rather than `TooLarge`. See
      // util::BrotliInflate.
      ok = util::BrotliInflate(body, allowed, decoded);
    } else if (*coding == "deflate") {
      // RFC 9110 says `deflate` is the zlib wrapper. A meaningful share of
      // servers send a raw DEFLATE stream under that name instead, and every
      // browser accepts both, so a server that sends the raw form to us and the
      // wrapped form to the browser next to us is not a difference a page can
      // be built around. The wrapper is tried first because it is checksummed.
      ok = util::ZlibInflate(body, allowed, decoded) || util::Inflate(body, allowed, decoded);
    } else {
      AddPerformanceCounter(PerfCounterId::NetContentDecodeFailures);
      return DecodeStatus::UnsupportedCoding;
    }

    if (!ok) {
      AddPerformanceCounter(PerfCounterId::NetContentDecodeFailures);
      // Anything that gets here failed for a reason the decoder cannot report
      // apart from the bound — it stops on the back reference that *would*
      // exceed the ceiling, so its output length says nothing. `TooLarge` is
      // therefore reserved for the case above, where the member declared its
      // own size. A `deflate` bomb reads as `Malformed`; the response fails
      // either way and the difference is a diagnostic, not a decision.
      return DecodeStatus::Malformed;
    }
    // Both sides of the trade, so one run answers what the coding is worth:
    // what arrived under it, and what it became.
    AddPerformanceCounter(PerfCounterId::NetBytesCoded, body.size());
    AddPerformanceCounter(PerfCounterId::NetBytesDecoded, decoded.size());
    body = std::move(decoded);
    decoded = std::vector<std::byte>{};
  }

  response.body = std::move(body);
  // Both headers described the coded form. Content-Length is now wrong by
  // exactly the amount the decoding gained, and Content-Encoding would make a
  // second pass decode a body that is already plain.
  RemoveHeader(response.headers, "content-encoding");
  RemoveHeader(response.headers, "content-length");
  return DecodeStatus::Decoded;
}

}  // namespace microbrowser::net
