#include "engine/Loader.h"

#include <utility>

#include "util/StringUtil.h"

namespace microbrowser::engine {

namespace {

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

// Percent-decoding, for the path of a data: URL. Malformed escapes are left
// alone rather than dropped: a lone `%` is a byte, and eating it would change
// the document.
std::string PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%' || i + 2 >= text.size()) {
      out.push_back(text[i]);
      continue;
    }
    const int high = HexValue(text[i + 1]);
    const int low = HexValue(text[i + 2]);
    if (high < 0 || low < 0) {
      out.push_back(text[i]);
      continue;
    }
    out.push_back(static_cast<char>(high * 16 + low));
    i += 2;
  }
  return out;
}

std::string Base64Decode(std::string_view text, bool& ok) {
  constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const std::size_t value = kAlphabet.find(c);
    if (value == std::string_view::npos) {
      ok = false;
      return {};
    }
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
    }
  }
  ok = true;
  return out;
}

std::string BodyAsString(const std::vector<std::byte>& body) {
  return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

}  // namespace

Loader::Loader() : transport_(&sockets_) {}

DataUrl DecodeDataUrl(std::string_view url) {
  DataUrl result;
  constexpr std::string_view kPrefix = "data:";
  if (url.size() < kPrefix.size() || !util::EqualsAsciiCaseInsensitive(url.substr(0, kPrefix.size()),
                                                                  kPrefix)) {
    return result;
  }
  url.remove_prefix(kPrefix.size());

  const std::size_t comma = url.find(',');
  if (comma == std::string_view::npos) {
    // No comma is not a data URL, it is a string beginning with "data:". The
    // spec says so, and accepting it would make every such string a document.
    return result;
  }
  std::string_view metadata = url.substr(0, comma);
  const std::string_view payload = url.substr(comma + 1);

  bool base64 = false;
  constexpr std::string_view kBase64 = ";base64";
  if (metadata.size() >= kBase64.size() &&
      util::EqualsAsciiCaseInsensitive(metadata.substr(metadata.size() - kBase64.size()), kBase64)) {
    base64 = true;
    metadata.remove_suffix(kBase64.size());
  }

  result.content_type = metadata.empty() ? "text/plain;charset=US-ASCII" : std::string(metadata);
  if (base64) {
    bool ok = false;
    result.body = Base64Decode(payload, ok);
    result.ok = ok;
    return result;
  }
  result.body = PercentDecode(payload);
  result.ok = true;
  return result;
}

Loader::Result Loader::Load(std::string_view url, std::int64_t now) {
  Result result;

  if (DataUrl data = DecodeDataUrl(url); data.ok) {
    result.ok = true;
    result.body = std::move(data.body);
    result.content_type = std::move(data.content_type);
    result.final_url = std::string(url);
    result.status = 200;
    return result;
  }

  const std::optional<url::Url> parsed = url::Url::Parse(url);
  if (!parsed.has_value()) {
    result.error = "that is not a URL";
    return result;
  }

  // A top-level navigation has no initiator page: the origin is opaque, and
  // that is a value rather than an absence. The top-level site is the one being
  // navigated *to*, because after this load it is the top-level site.
  privacy::Request request;
  request.url = *parsed;
  request.initiator = url::Origin{};
  request.top_level_site = url::Site::FromUrl(*parsed);
  request.container = url::ContainerId::Default();
  request.type = privacy::ResourceType::Document;
  request.is_subresource = false;

  privacy::Verdict verdict = policy_.Decide(request);
  if (!verdict.IsAllowed()) {
    blocked_reason_ = verdict.Reason();
    result.error = blocked_reason_.empty() ? "blocked" : blocked_reason_.c_str();
    return result;
  }

  net::FetchOptions options;
  options.is_top_level_navigation = true;

  const net::FetchResult fetched =
      net::Fetch(std::move(verdict), policy_, *transport_, cookies_, cache_, options, now);
  if (!fetched.ok) {
    result.error = fetched.error == nullptr ? "the load failed" : fetched.error;
    return result;
  }

  result.ok = true;
  result.status = fetched.response.status;
  result.final_url = fetched.final_url.Serialize();
  result.body = BodyAsString(fetched.response.body);
  if (const std::optional<std::string_view> type = fetched.response.headers.Get("content-type")) {
    result.content_type = std::string(*type);
  }
  return result;
}

}  // namespace microbrowser::engine
