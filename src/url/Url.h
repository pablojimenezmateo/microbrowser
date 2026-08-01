#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "url/Host.h"

namespace microbrowser::url {

// A parsed URL, per the WHATWG URL Standard.
//
// Spec-literal, and the repo's rule about spec-literal parsers applies: the
// states below are named exactly as the standard names them, so this file can
// be checked against section 4.4 line by line. Divergence from the spec text is
// a bug, not a style choice.
//
// The reason a browser cannot use a "good enough" URL parser is that a URL is a
// security boundary. Every same-origin check, every cookie scope, every CSP
// decision and every partition key is computed from the output of this file. If
// this parser and a server's parser disagree about where the host ends, that
// disagreement *is* the vulnerability — which is why the conformance suite is
// the WHATWG's own `urltestdata.json` rather than cases invented here.
class Url {
 public:
  Url() = default;

  // Absolute parse. Nullopt when the input is not a URL; there is no
  // "best effort" result, because a half-parsed URL is a URL that means one
  // thing to this code and another to whatever it is handed to.
  static std::optional<Url> Parse(std::string_view input);

  // Relative parse against a base. This is what a link on a page goes through.
  static std::optional<Url> Parse(std::string_view input, const Url& base);

  bool IsValid() const { return !scheme_.empty(); }

  const std::string& Scheme() const { return scheme_; }
  const std::string& Username() const { return username_; }
  const std::string& Password() const { return password_; }
  const Host& GetHost() const { return host_; }
  const std::string& HostSerialized() const { return host_.Serialized(); }
  // Nullopt means "the scheme's default", which is not the same as port zero.
  std::optional<std::uint16_t> Port() const { return port_; }
  // The port that will actually be connected to, default included.
  std::optional<std::uint16_t> EffectivePort() const;

  const std::vector<std::string>& PathSegments() const { return path_; }
  bool HasOpaquePath() const { return opaque_path_; }
  const std::string& OpaquePath() const { return opaque_path_value_; }
  std::string PathString() const;

  bool HasQuery() const { return query_.has_value(); }
  const std::string& Query() const { return query_ ? *query_ : empty_; }
  bool HasFragment() const { return fragment_.has_value(); }
  const std::string& Fragment() const { return fragment_ ? *fragment_ : empty_; }

  // True for http, https, ws, wss, ftp and file — the schemes the standard
  // gives special parsing rules to.
  bool IsSpecial() const;
  bool IsHttpOrHttps() const { return scheme_ == "http" || scheme_ == "https"; }

  // Serialization. `exclude_fragment` is what a referrer, a cache key and a
  // fetch all want; the fragment never leaves the client.
  std::string Serialize(bool exclude_fragment = false) const;

  // Replaces the query wholesale. Used by URL sanitization, which strips
  // tracking parameters — see guidelines/privacy.md.
  void SetQuery(std::optional<std::string> query) { query_ = std::move(query); }

  friend bool operator==(const Url&, const Url&) = default;

 private:
  friend class UrlParser;

  std::string scheme_;
  std::string username_;
  std::string password_;
  Host host_;
  std::optional<std::uint16_t> port_;
  std::vector<std::string> path_;
  std::string opaque_path_value_;
  std::optional<std::string> query_;
  std::optional<std::string> fragment_;
  bool opaque_path_ = false;
  // Returned by reference from Query()/Fragment() when absent, so callers never
  // have to hold an optional to read a string.
  static const std::string empty_;
};

// Default port for a special scheme, or nullopt.
std::optional<std::uint16_t> DefaultPortForScheme(std::string_view scheme);

}  // namespace microbrowser::url
