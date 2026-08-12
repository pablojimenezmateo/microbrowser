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

  UrlParser(std::string_view input, const Url* base, Url& url,
            std::optional<State> state_override);

  // False is the standard's "return failure". With a state override, a failure
  // means the caller's URL must be left alone — which is why Url's setters
  // parse into a copy.
  bool Run();

 private:
  bool SchemeOverrideRefuses(const std::string& buffer) const;
  void ShortenPath();
  bool StartsWithWindowsDriveLetter(std::size_t pointer) const;

  std::string input_;
  const Url* base_;
  Url& url_;
  std::optional<State> state_override_;
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
