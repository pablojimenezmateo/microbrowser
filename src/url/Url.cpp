#include "url/Url.h"

#include <algorithm>

#include "url/UrlParser.h"
#include "util/PercentEncoding.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::url {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

}  // namespace

const std::string Url::empty_;
const Host Url::empty_host_;

std::optional<std::uint16_t> DefaultPortForScheme(std::string_view scheme) {
  if (scheme == "http" || scheme == "ws") {
    return std::uint16_t{80};
  }
  if (scheme == "https" || scheme == "wss") {
    return std::uint16_t{443};
  }
  if (scheme == "ftp") {
    return std::uint16_t{21};
  }
  return std::nullopt;
}

bool Url::IsSpecial() const {
  return SchemeIsSpecial(scheme_);
}

std::optional<std::uint16_t> Url::EffectivePort() const {
  return port_.has_value() ? port_ : DefaultPortForScheme(scheme_);
}

bool Url::CanHaveCredentialsOrPort() const {
  // "A URL cannot have a username/password/port if its host is null or the
  // empty host, or its scheme is 'file'."
  return host_.has_value() && !host_->IsEmpty() && scheme_ != "file";
}

std::string Url::PathString() const {
  if (opaque_path_) {
    return opaque_path_value_;
  }
  std::string out;
  for (const std::string& segment : path_) {
    out.push_back('/');
    out += segment;
  }
  return out;
}

std::string Url::Serialize(bool exclude_fragment) const {
  std::string out = scheme_;
  out.push_back(':');
  if (host_.has_value()) {
    out += "//";
    if (!username_.empty() || !password_.empty()) {
      out += username_;
      if (!password_.empty()) {
        out.push_back(':');
        out += password_;
      }
      out.push_back('@');
    }
    out += host_->Serialized();
    if (port_.has_value()) {
      out.push_back(':');
      out += std::to_string(*port_);
    }
  } else if (!opaque_path_ && path_.size() > 1 && path_[0].empty()) {
    // Prevents `/​/foo` being re-read as a host on the next parse.
    out += "/.";
  }

  out += PathString();

  if (query_.has_value()) {
    out.push_back('?');
    out += *query_;
  }
  if (!exclude_fragment && fragment_.has_value()) {
    out.push_back('#');
    out += *fragment_;
  }
  return out;
}

std::string Url::HostPort() const {
  if (!host_.has_value()) {
    return std::string();
  }
  if (!port_.has_value()) {
    return host_->Serialized();
  }
  return host_->Serialized() + ":" + std::to_string(*port_);
}

std::string Url::PortString() const {
  return port_.has_value() ? std::to_string(*port_) : std::string();
}

std::string Url::Search() const {
  return !query_.has_value() || query_->empty() ? std::string() : "?" + *query_;
}

std::string Url::Hash() const {
  return !fragment_.has_value() || fragment_->empty() ? std::string() : "#" + *fragment_;
}

std::string Url::OriginString() const {
  if (scheme_ == "blob") {
    // The origin of a blob URL is the origin of the URL its path spells. A
    // failure here is an opaque origin rather than an error: a blob URL whose
    // path is not a URL names nothing, and "null" is what names nothing.
    const std::optional<Url> inner = Url::Parse(PathString());
    if (!inner.has_value()) {
      return "null";
    }
    if (inner->scheme_ == "http" || inner->scheme_ == "https" || inner->scheme_ == "file") {
      return inner->OriginString();
    }
    return "null";
  }
  if (scheme_ != "ftp" && scheme_ != "http" && scheme_ != "https" && scheme_ != "ws" &&
      scheme_ != "wss") {
    return "null";  // including `file`, whose origin is deliberately opaque
  }
  std::string out = scheme_ + "://" + HostSerialized();
  if (port_.has_value()) {
    out.push_back(':');
    out += std::to_string(*port_);
  }
  return out;
}

std::optional<Url> Url::Parse(std::string_view input) {
  AddPerformanceCounter(PerfCounterId::UrlParses);
  Url url;
  UrlParser parser(input, nullptr, url, std::nullopt);
  if (!parser.Run()) {
    AddPerformanceCounter(PerfCounterId::UrlParseFailures);
    return std::nullopt;
  }
  return url;
}

std::optional<Url> Url::Parse(std::string_view input, const Url& base) {
  AddPerformanceCounter(PerfCounterId::UrlParses);
  Url url;
  UrlParser parser(input, &base, url, std::nullopt);
  if (!parser.Run()) {
    AddPerformanceCounter(PerfCounterId::UrlParseFailures);
    return std::nullopt;
  }
  return url;
}

// --- Setters -----------------------------------------------------------------
//
// Each parses **in place**, which is the standard's own shape and not an
// accident of it. `url.host = "example.com:65536"` sets the host and then fails
// on the port, and the host change stands -- so a setter that parsed into a
// copy and discarded it on failure would answer differently from every other
// browser for exactly the inputs a page is least likely to have tried.

namespace {

void ParseWithOverride(Url& url, std::string_view input, UrlParser::State override_state) {
  UrlParser parser(input, nullptr, url, override_state);
  (void)parser.Run();
}

}  // namespace

void Url::SetProtocol(std::string_view value) {
  ParseWithOverride(*this, std::string(value) + ":", UrlParser::State::SchemeStart);
}

void Url::SetUsername(std::string_view value) {
  if (!CanHaveCredentialsOrPort()) {
    return;
  }
  username_ = util::PercentEncode(value, util::PercentEncodeSet::Userinfo);
}

void Url::SetPassword(std::string_view value) {
  if (!CanHaveCredentialsOrPort()) {
    return;
  }
  password_ = util::PercentEncode(value, util::PercentEncodeSet::Userinfo);
}

void Url::SetHost(std::string_view value) {
  if (opaque_path_) {
    return;
  }
  ParseWithOverride(*this, value, UrlParser::State::Host);
}

void Url::SetHostname(std::string_view value) {
  if (opaque_path_) {
    return;
  }
  ParseWithOverride(*this, value, UrlParser::State::Hostname);
}

void Url::SetPort(std::string_view value) {
  if (!CanHaveCredentialsOrPort()) {
    return;
  }
  if (value.empty()) {
    port_.reset();
    return;
  }
  ParseWithOverride(*this, value, UrlParser::State::Port);
}

void Url::SetPathname(std::string_view value) {
  if (opaque_path_) {
    return;
  }
  path_.clear();
  ParseWithOverride(*this, value, UrlParser::State::PathStart);
}

void Url::SetSearch(std::string_view value) {
  if (value.empty()) {
    query_.reset();
    return;
  }
  if (value.front() == '?') {
    value.remove_prefix(1);
  }
  query_ = std::string();
  ParseWithOverride(*this, value, UrlParser::State::Query);
}

void Url::SetHash(std::string_view value) {
  if (value.empty()) {
    fragment_.reset();
    return;
  }
  if (value.front() == '#') {
    value.remove_prefix(1);
  }
  fragment_ = std::string();
  ParseWithOverride(*this, value, UrlParser::State::Fragment);
}

bool Url::SetHref(std::string_view value) {
  const std::optional<Url> parsed = Url::Parse(value);
  if (!parsed.has_value()) {
    return false;
  }
  *this = *parsed;
  return true;
}

}  // namespace microbrowser::url
