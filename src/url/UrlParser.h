#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "url/Url.h"

namespace microbrowser::url {

// The basic URL parser (URL Standard §4.4), as one class.
//
// It is a class rather than a function so the states can share the buffer, the
// flags and the output without threading five things through twenty branches;
// and it is a *header* rather than a detail of Url.cpp because the setters need
// its `State` enum by name. That is the standard's own shape: `url.hostname = x`
// is defined as "basic URL parse x with url as url and hostname state as state
// override", so the states are part of this module's vocabulary, not an
// implementation detail of one file.
//
// `url` is an in/out parameter for the same reason. With a state override the
// parser edits a URL that already exists, and every one of those edits is a
// setter on a live object.
class UrlParser {
 public:
  // Named exactly as the standard names them, so this file can be read against
  // §4.4 line by line.
  enum class State {
    SchemeStart,
    Scheme,
    NoScheme,
    SpecialRelativeOrAuthority,
    PathOrAuthority,
    Relative,
    RelativeSlash,
    SpecialAuthoritySlashes,
    SpecialAuthorityIgnoreSlashes,
    Authority,
    Host,
    Hostname,
    Port,
    File,
    FileSlash,
    FileHost,
    PathStart,
    Path,
    OpaquePath,
    Query,
    Fragment,
  };

  // `query_encoder` is the document's character set for the query, and null is
  // UTF-8 -- which is what every caller that is not a document wants. See the
  // comment on `url::QueryEncoder`; the query is the one part of a URL whose
  // bytes depend on the page the link was written on.
  UrlParser(std::string_view input, const Url* base, Url& url,
            std::optional<State> state_override, const QueryEncoder* query_encoder = nullptr);

  // False is the standard's "return failure". With a state override, a failure
  // means the caller's URL must be left alone — which is why Url's setters
  // parse into a copy.
  bool Run();

 private:
  bool SchemeOverrideRefuses(const std::string& buffer) const;
  void ShortenPath();
  bool StartsWithWindowsDriveLetter(std::size_t pointer) const;
  // The standard's "percent-encode after encoding", run over the whole query
  // at once rather than byte by byte as it arrives. It has to be the whole
  // query: an encoding is a function of a *string*, not of a byte --
  // ISO-2022-JP writes an escape sequence when it changes character set, so
  // the bytes for a character depend on the ones before it, and a per-byte
  // loop would write one escape per character and none of the closing ones.
  void FlushQuery();

  std::string input_;
  const Url* base_;
  Url& url_;
  std::optional<State> state_override_;
  const QueryEncoder* query_encoder_;
  std::string query_buffer_;
  std::string buffer_;
  bool at_sign_seen_ = false;
  bool inside_brackets_ = false;
  bool password_token_seen_ = false;
};

// True for the six schemes the standard gives special parsing rules to. Free
// rather than a member because the parser asks it of a *buffer* before that
// buffer is anybody's scheme.
bool SchemeIsSpecial(std::string_view scheme);

}  // namespace microbrowser::url
