#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "url/Host.h"

namespace microbrowser::url {

// The query is the one part of a URL that is not UTF-8.
//
// A document in Shift_JIS puts Shift_JIS bytes in the query of every link on
// it, because that is what the server on the other end expects to read back;
// the URL Standard calls this "percent-encode after encoding" and it is the
// only place in URL parsing where a *document's* encoding is an input. Getting
// it wrong is not cosmetic — a search box on a Japanese page sends bytes the
// server decodes as different words.
//
// This module may see only `util`, which is what keeps the bottom of the web
// stack at the bottom: every same-origin check, cookie scope and partition key
// is computed here, and a dependency on the encoding tables would put a
// character-set question underneath all of them. So the encoder is handed in.
// `src/html` implements it (it owns the indexes) and `src/engine` — the one
// module that sees both — is where the two are joined.
//
// Nothing is a bare "encode this string": the standard's error escape must be
// appended already percent-encoded whatever the percent-encode set says (`&`
// and `;` are not in it, and `&#1234;` written literally would look like two
// more query parameters to whatever reads it). So the encoding and the
// percent-encoding happen in one call, and cannot be composed in the wrong
// order by a caller.
class QueryEncoder {
 public:
  virtual ~QueryEncoder() = default;

  // Appends the encoded, percent-encoded form of `input` — a UTF-8 string — to
  // `out`. `needs_escape` is this module's percent-encode set, applied to each
  // byte the encoding produced; a code point the encoding cannot represent
  // appends `%26%23<n>%3B` and is not passed through it.
  virtual void EncodeQuery(std::string_view input, bool (*needs_escape)(unsigned char),
                           std::string& out) const = 0;
};

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
// `tools/urlconf` runs those vectors against this class directly.
//
// **A host is nullable, and that is not a detail.** "No host at all"
// (`mailto:x@example.com`) and "the empty host" (`file:///tmp`) are different
// states with different serializations: the second prints `//` and the first
// does not. Folding them into one — an empty string standing for both — is how
// `sc://x` and `sc:x` come to serialize alike, which is a parser that disagrees
// with every other one about what a URL means.
class Url {
 public:
  Url() = default;

  // Absolute parse. Nullopt when the input is not a URL; there is no
  // "best effort" result, because a half-parsed URL is a URL that means one
  // thing to this code and another to whatever it is handed to.
  static std::optional<Url> Parse(std::string_view input);

  // Relative parse against a base. This is what a link on a page goes through.
  static std::optional<Url> Parse(std::string_view input, const Url& base);

  // The same two, with a document's encoding for the query. `encoder` may be
  // null, which is UTF-8 and is what every caller that is not a document wants
  // — `new URL(…)` is defined to be UTF-8 whatever page it was called from,
  // and so is every URL this browser builds for itself.
  //
  // The encoder is ignored for a URL that is not special and for `ws`/`wss`,
  // which the standard forces back to UTF-8: those schemes have no legacy of
  // servers reading a document's charset out of a query, and honouring one
  // would let a document's `<meta charset>` change the bytes of a WebSocket
  // handshake.
  static std::optional<Url> Parse(std::string_view input, const QueryEncoder* encoder);
  static std::optional<Url> Parse(std::string_view input, const Url& base,
                                  const QueryEncoder* encoder);

  bool IsValid() const { return !scheme_.empty(); }

  const std::string& Scheme() const { return scheme_; }
  const std::string& Username() const { return username_; }
  const std::string& Password() const { return password_; }
  bool HasHost() const { return host_.has_value(); }
  const Host& GetHost() const { return host_ ? *host_ : empty_host_; }
  const std::string& HostSerialized() const {
    return host_ ? host_->Serialized() : empty_;
  }
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

  // --- The `URL` object's own getters (§6.1) --------------------------------
  //
  // These are here rather than in the binding layer because they are the
  // standard's own definitions, and a second copy of "what does `.host` mean"
  // written against the components below is a second answer. `src/bindings`
  // spells `url.host` as one call into this.
  std::string Href() const { return Serialize(); }
  std::string Protocol() const { return scheme_ + ":"; }
  std::string HostPort() const;
  std::string Hostname() const { return HostSerialized(); }
  std::string PortString() const;
  std::string Pathname() const { return PathString(); }
  std::string Search() const;
  std::string Hash() const;
  // The origin, serialized — "null" for every scheme that does not have a
  // tuple origin, which is the same answer `location.origin` gives.
  std::string OriginString() const;

  // --- The `URL` object's own setters (§6.1) --------------------------------
  //
  // Each is the standard's setter verbatim, which is why several of them are
  // documented no-ops: `url.pathname = "x"` on an opaque path does nothing at
  // all, and a setter that "helpfully" did something instead would be a URL
  // this browser understands and no server does. None can throw; `SetHref` is
  // the only one that reports, because it is the only one the standard makes
  // fail loudly.
  void SetProtocol(std::string_view value);
  void SetUsername(std::string_view value);
  void SetPassword(std::string_view value);
  void SetHost(std::string_view value);
  void SetHostname(std::string_view value);
  void SetPort(std::string_view value);
  void SetPathname(std::string_view value);
  void SetSearch(std::string_view value);
  void SetHash(std::string_view value);
  [[nodiscard]] bool SetHref(std::string_view value);

  friend bool operator==(const Url&, const Url&) = default;

 private:
  friend class UrlParser;

  // Whether the standard lets this URL carry credentials or a port. A null or
  // empty host cannot: there would be nothing for them to qualify.
  bool CanHaveCredentialsOrPort() const;

  std::string scheme_;
  std::string username_;
  std::string password_;
  std::optional<Host> host_;
  std::optional<std::uint16_t> port_;
  std::vector<std::string> path_;
  std::string opaque_path_value_;
  std::optional<std::string> query_;
  std::optional<std::string> fragment_;
  bool opaque_path_ = false;
  // Returned by reference from Query()/Fragment() when absent, so callers never
  // have to hold an optional to read a string.
  static const std::string empty_;
  static const Host empty_host_;
};

// Default port for a special scheme, or nullopt.
std::optional<std::uint16_t> DefaultPortForScheme(std::string_view scheme);

}  // namespace microbrowser::url
