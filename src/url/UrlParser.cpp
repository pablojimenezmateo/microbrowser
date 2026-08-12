#include "url/UrlParser.h"

#include <algorithm>

#include "util/PercentEncoding.h"

namespace microbrowser::url {

namespace {

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAsciiAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsAsciiAlphanumeric(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

bool IsC0ControlOrSpace(char c) {
  return static_cast<unsigned char>(c) <= 0x20;
}

bool IsTabOrNewline(char c) {
  return c == '\t' || c == '\n' || c == '\r';
}

// A Windows drive letter, which the file scheme has to recognize because
// `file:///c:/` and `file://c:/` mean the same thing on the platform that
// invented them.
bool IsWindowsDriveLetter(std::string_view input) {
  return input.size() == 2 && IsAsciiAlpha(input[0]) && (input[1] == ':' || input[1] == '|');
}

bool IsNormalizedWindowsDriveLetter(std::string_view input) {
  return input.size() == 2 && IsAsciiAlpha(input[0]) && input[1] == ':';
}

std::string AsciiLower(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char c : input) {
    out.push_back(ToLower(c));
  }
  return out;
}

bool IsSingleDot(std::string_view segment) {
  return segment == "." || AsciiLower(segment) == "%2e";
}

bool IsDoubleDot(std::string_view segment) {
  if (segment == "..") {
    return true;
  }
  const std::string lowered = AsciiLower(segment);
  return lowered == ".%2e" || lowered == "%2e." || lowered == "%2e%2e";
}

}  // namespace

bool SchemeIsSpecial(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" ||
         scheme == "ftp" || scheme == "file";
}

UrlParser::UrlParser(std::string_view input, const Url* base, Url& url,
                     std::optional<State> state_override, const QueryEncoder* query_encoder)
    : base_(base), url_(url), state_override_(state_override), query_encoder_(query_encoder) {
  // Leading and trailing C0 controls and spaces are stripped -- but only when
  // this is a fresh parse. A setter hands the parser a fragment of a URL that
  // already exists, and trimming there would silently accept a value the
  // standard says to keep. Tabs and newlines come out either way: both appear
  // in real HTML, and a parser that kept them would disagree with every other
  // browser about where the URL starts.
  std::size_t begin = 0;
  std::size_t end = input.size();
  if (!state_override_.has_value()) {
    while (begin < end && IsC0ControlOrSpace(input[begin])) {
      ++begin;
    }
    while (end > begin && IsC0ControlOrSpace(input[end - 1])) {
      --end;
    }
  }
  input_.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    if (!IsTabOrNewline(input[i])) {
      input_.push_back(input[i]);
    }
  }
}

bool UrlParser::SchemeOverrideRefuses(const std::string& buffer) const {
  if (SchemeIsSpecial(url_.scheme_) != SchemeIsSpecial(buffer)) {
    return true;
  }
  const bool includes_credentials = !url_.username_.empty() || !url_.password_.empty();
  if ((includes_credentials || url_.port_.has_value()) && buffer == "file") {
    return true;
  }
  return url_.scheme_ == "file" && url_.host_.has_value() && url_.host_->IsEmpty();
}

void UrlParser::ShortenPath() {
  if (url_.opaque_path_ || url_.path_.empty()) {
    return;
  }
  if (url_.scheme_ == "file" && url_.path_.size() == 1 &&
      IsNormalizedWindowsDriveLetter(url_.path_[0])) {
    return;  // a drive letter is not a path segment that can be popped
  }
  url_.path_.pop_back();
}

bool UrlParser::StartsWithWindowsDriveLetter(std::size_t pointer) const {
  const std::string_view rest = std::string_view(input_).substr(std::min(pointer, input_.size()));
  if (rest.size() < 2 || !IsWindowsDriveLetter(rest.substr(0, 2))) {
    return false;
  }
  return rest.size() == 2 || rest[2] == '/' || rest[2] == '\\' || rest[2] == '?' || rest[2] == '#';
}

void UrlParser::FlushQuery() {
  if (!url_.query_.has_value() || query_buffer_.empty()) {
    query_buffer_.clear();
    return;
  }
  const bool special = SchemeIsSpecial(url_.scheme_);
  const util::PercentEncodeSet set =
      special ? util::PercentEncodeSet::SpecialQuery : util::PercentEncodeSet::Query;
  // The standard forces UTF-8 back for a non-special scheme and for ws/wss --
  // see the comment on `url::QueryEncoder`. `ftp:` and `file:` are special and
  // do take the document's encoding, which looks odd and is what it says.
  const bool honour_encoding =
      query_encoder_ != nullptr && special && url_.scheme_ != "ws" && url_.scheme_ != "wss";
  if (!honour_encoding) {
    util::PercentEncodeInto(query_buffer_, set, *url_.query_);
  } else if (set == util::PercentEncodeSet::SpecialQuery) {
    query_encoder_->EncodeQuery(
        query_buffer_,
        [](unsigned char byte) {
          return util::ShouldPercentEncode(byte, util::PercentEncodeSet::SpecialQuery);
        },
        *url_.query_);
  } else {
    query_encoder_->EncodeQuery(
        query_buffer_,
        [](unsigned char byte) {
          return util::ShouldPercentEncode(byte, util::PercentEncodeSet::Query);
        },
        *url_.query_);
  }
  query_buffer_.clear();
}

bool UrlParser::Run() {
  State state = state_override_.value_or(State::SchemeStart);
  std::size_t pointer = 0;
  const std::size_t length = input_.size();

  // `c` is the code point at the pointer; one past the end is the EOF marker,
  // which the standard treats as a real position rather than a loop exit.
  const auto at = [&](std::size_t index) -> int {
    return index < length ? static_cast<unsigned char>(input_[index]) : -1;
  };
  const auto url_is_special = [&] { return SchemeIsSpecial(url_.scheme_); };

  while (pointer <= length) {
    const int c = at(pointer);
    switch (state) {
      case State::SchemeStart:
        if (c >= 0 && IsAsciiAlpha(static_cast<char>(c))) {
          buffer_.push_back(ToLower(static_cast<char>(c)));
          state = State::Scheme;
        } else if (!state_override_.has_value()) {
          state = State::NoScheme;
          continue;  // reprocess without advancing
        } else {
          return false;
        }
        break;

      case State::Scheme:
        if (c >= 0 &&
            (IsAsciiAlphanumeric(static_cast<char>(c)) || c == '+' || c == '-' || c == '.')) {
          buffer_.push_back(ToLower(static_cast<char>(c)));
        } else if (c == ':') {
          if (state_override_.has_value()) {
            if (SchemeOverrideRefuses(buffer_)) {
              return true;  // the standard's "return", which is not a failure
            }
            url_.scheme_ = buffer_;
            if (DefaultPortForScheme(url_.scheme_) == url_.port_) {
              url_.port_.reset();
            }
            return true;
          }
          url_.scheme_ = buffer_;
          buffer_.clear();
          if (url_.scheme_ == "file") {
            state = State::File;
          } else if (url_is_special() && base_ != nullptr && base_->scheme_ == url_.scheme_) {
            state = State::SpecialRelativeOrAuthority;
          } else if (url_is_special()) {
            state = State::SpecialAuthoritySlashes;
          } else if (at(pointer + 1) == '/') {
            state = State::PathOrAuthority;
            ++pointer;
          } else {
            url_.opaque_path_ = true;
            url_.opaque_path_value_.clear();
            state = State::OpaquePath;
          }
        } else if (!state_override_.has_value()) {
          // Not a scheme after all; start over as a relative reference.
          buffer_.clear();
          state = State::NoScheme;
          pointer = 0;
          continue;
        } else {
          return false;
        }
        break;

      case State::NoScheme:
        if (base_ == nullptr || (base_->opaque_path_ && c != '#')) {
          return false;
        }
        if (base_->opaque_path_) {
          url_.scheme_ = base_->scheme_;
          url_.opaque_path_ = true;
          url_.opaque_path_value_ = base_->opaque_path_value_;
          url_.query_ = base_->query_;
          url_.fragment_ = std::string();
          state = State::Fragment;
        } else if (base_->scheme_ != "file") {
          state = State::Relative;
          continue;
        } else {
          state = State::File;
          continue;
        }
        break;

      case State::SpecialRelativeOrAuthority:
        if (c == '/' && at(pointer + 1) == '/') {
          state = State::SpecialAuthorityIgnoreSlashes;
          ++pointer;
        } else {
          state = State::Relative;
          continue;
        }
        break;

      case State::PathOrAuthority:
        if (c == '/') {
          state = State::Authority;
        } else {
          state = State::Path;
          continue;
        }
        break;

      case State::Relative:
        url_.scheme_ = base_->scheme_;
        if (c == '/' || (url_is_special() && c == '\\')) {
          state = State::RelativeSlash;
        } else {
          url_.username_ = base_->username_;
          url_.password_ = base_->password_;
          url_.host_ = base_->host_;
          url_.port_ = base_->port_;
          url_.path_ = base_->path_;
          url_.query_ = base_->query_;
          if (c == '?') {
            url_.query_ = std::string();
            state = State::Query;
          } else if (c == '#') {
            url_.fragment_ = std::string();
            state = State::Fragment;
          } else if (c >= 0) {
            url_.query_.reset();
            ShortenPath();
            state = State::Path;
            continue;
          }
        }
        break;

      case State::RelativeSlash:
        if (url_is_special() && (c == '/' || c == '\\')) {
          state = State::SpecialAuthorityIgnoreSlashes;
        } else if (c == '/') {
          state = State::Authority;
        } else {
          url_.username_ = base_->username_;
          url_.password_ = base_->password_;
          url_.host_ = base_->host_;
          url_.port_ = base_->port_;
          state = State::Path;
          continue;
        }
        break;

      case State::SpecialAuthoritySlashes:
        if (c == '/' && at(pointer + 1) == '/') {
          state = State::SpecialAuthorityIgnoreSlashes;
          ++pointer;
        } else {
          state = State::SpecialAuthorityIgnoreSlashes;
          continue;
        }
        break;

      case State::SpecialAuthorityIgnoreSlashes:
        if (c != '/' && c != '\\') {
          state = State::Authority;
          continue;
        }
        break;

      case State::Authority:
        if (c == '@') {
          // Everything buffered so far was credentials, not a host.
          if (at_sign_seen_) {
            buffer_.insert(0, "%40");
          }
          at_sign_seen_ = true;
          for (std::size_t i = 0; i < buffer_.size(); ++i) {
            if (buffer_[i] == ':' && !password_token_seen_) {
              password_token_seen_ = true;
              continue;
            }
            const std::string_view piece(&buffer_[i], 1);
            if (password_token_seen_) {
              util::PercentEncodeInto(piece, util::PercentEncodeSet::Userinfo, url_.password_);
            } else {
              util::PercentEncodeInto(piece, util::PercentEncodeSet::Userinfo, url_.username_);
            }
          }
          buffer_.clear();
        } else if (c < 0 || c == '/' || c == '?' || c == '#' || (url_is_special() && c == '\\')) {
          if (at_sign_seen_ && buffer_.empty()) {
            return false;  // credentials with no host
          }
          // "Decrease pointer by buffer's code point length + 1" — and then the
          // loop's own increment applies, so the net rewind is the buffer
          // length and the next character read is the first one of the host.
          // `break` rather than `continue`: `continue` would skip that
          // increment and rewind one character too far, onto the slash that
          // ended the authority.
          pointer -= buffer_.size() + 1;
          buffer_.clear();
          state = State::Host;
          break;
        } else {
          buffer_.push_back(static_cast<char>(c));
        }
        break;

      case State::Host:
      case State::Hostname:
        if (state_override_.has_value() && url_.scheme_ == "file") {
          state = State::FileHost;
          continue;
        }
        if (c == ':' && !inside_brackets_) {
          if (buffer_.empty()) {
            return false;
          }
          if (state_override_ == State::Hostname) {
            return true;
          }
          const auto host = Host::Parse(buffer_, url_is_special());
          if (!host.has_value()) {
            return false;
          }
          url_.host_ = *host;
          buffer_.clear();
          state = State::Port;
        } else if (c < 0 || c == '/' || c == '?' || c == '#' || (url_is_special() && c == '\\')) {
          if (url_is_special() && buffer_.empty()) {
            return false;
          }
          if (state_override_.has_value() && buffer_.empty() &&
              (!url_.username_.empty() || !url_.password_.empty() || url_.port_.has_value())) {
            return true;
          }
          const auto host = Host::Parse(buffer_, url_is_special());
          if (!host.has_value()) {
            return false;
          }
          url_.host_ = *host;
          buffer_.clear();
          if (state_override_.has_value()) {
            return true;
          }
          state = State::PathStart;
          continue;
        } else {
          if (c == '[') {
            inside_brackets_ = true;
          } else if (c == ']') {
            inside_brackets_ = false;
          }
          buffer_.push_back(static_cast<char>(c));
        }
        break;

      case State::Port:
        if (c >= 0 && IsAsciiDigit(static_cast<char>(c))) {
          buffer_.push_back(static_cast<char>(c));
        } else if (c < 0 || c == '/' || c == '?' || c == '#' || (url_is_special() && c == '\\') ||
                   state_override_.has_value()) {
          if (!buffer_.empty()) {
            // Accumulated in 32 bits and bounded, so a port of forty digits
            // cannot wrap into a plausible-looking small number.
            std::uint32_t port = 0;
            for (const char digit : buffer_) {
              port = port * 10 + static_cast<std::uint32_t>(digit - '0');
              if (port > 0xFFFF) {
                return false;
              }
            }
            // A port equal to the scheme's default is dropped, so that
            // `https://x:443/` and `https://x/` are one origin rather than two.
            if (DefaultPortForScheme(url_.scheme_) == static_cast<std::uint16_t>(port)) {
              url_.port_.reset();
            } else {
              url_.port_ = static_cast<std::uint16_t>(port);
            }
            buffer_.clear();
          }
          if (state_override_.has_value()) {
            return true;
          }
          state = State::PathStart;
          continue;
        } else {
          return false;
        }
        break;

      case State::File:
        url_.scheme_ = "file";
        url_.host_ = Host();  // the empty host, which is not a null host
        if (c == '/' || c == '\\') {
          state = State::FileSlash;
        } else if (base_ != nullptr && base_->scheme_ == "file") {
          url_.host_ = base_->host_;
          url_.path_ = base_->path_;
          url_.query_ = base_->query_;
          if (c == '?') {
            url_.query_ = std::string();
            state = State::Query;
          } else if (c == '#') {
            url_.fragment_ = std::string();
            state = State::Fragment;
          } else if (c >= 0) {
            url_.query_.reset();
            if (!StartsWithWindowsDriveLetter(pointer)) {
              ShortenPath();
            } else {
              url_.path_.clear();
            }
            state = State::Path;
            continue;
          }
        } else {
          state = State::Path;
          continue;
        }
        break;

      case State::FileSlash:
        if (c == '/' || c == '\\') {
          state = State::FileHost;
        } else {
          if (base_ != nullptr && base_->scheme_ == "file") {
            url_.host_ = base_->host_;
            if (!StartsWithWindowsDriveLetter(pointer) && !base_->path_.empty() &&
                IsNormalizedWindowsDriveLetter(base_->path_[0])) {
              url_.path_.push_back(base_->path_[0]);
            }
          }
          state = State::Path;
          continue;
        }
        break;

      case State::FileHost:
        if (c < 0 || c == '/' || c == '\\' || c == '?' || c == '#') {
          if (!state_override_.has_value() && IsWindowsDriveLetter(buffer_)) {
            // `file://c:/` — that is a path, not a host.
            state = State::Path;
            continue;
          }
          if (buffer_.empty()) {
            url_.host_ = Host();
            if (state_override_.has_value()) {
              return true;
            }
            state = State::PathStart;
            continue;
          }
          auto host = Host::Parse(buffer_, url_is_special());
          if (!host.has_value()) {
            return false;
          }
          // A file URL's "localhost" host means no host at all.
          if (host->Serialized() == "localhost") {
            host = Host();
          }
          url_.host_ = *host;
          if (state_override_.has_value()) {
            return true;
          }
          buffer_.clear();
          state = State::PathStart;
          continue;
        }
        buffer_.push_back(static_cast<char>(c));
        break;

      case State::PathStart:
        if (url_is_special()) {
          state = State::Path;
          if (c != '/' && c != '\\') {
            continue;
          }
        } else if (!state_override_.has_value() && c == '?') {
          url_.query_ = std::string();
          state = State::Query;
        } else if (!state_override_.has_value() && c == '#') {
          url_.fragment_ = std::string();
          state = State::Fragment;
        } else if (c >= 0) {
          state = State::Path;
          if (c != '/') {
            continue;
          }
        } else if (state_override_.has_value() && !url_.host_.has_value()) {
          url_.path_.emplace_back();
        }
        break;

      case State::Path:
        if (c < 0 || c == '/' || (url_is_special() && c == '\\') ||
            (!state_override_.has_value() && (c == '?' || c == '#'))) {
          if (IsDoubleDot(buffer_)) {
            ShortenPath();
            if (c != '/' && !(url_is_special() && c == '\\')) {
              url_.path_.emplace_back();
            }
          } else if (IsSingleDot(buffer_)) {
            if (c != '/' && !(url_is_special() && c == '\\')) {
              url_.path_.emplace_back();
            }
          } else {
            if (url_.scheme_ == "file" && url_.path_.empty() && IsWindowsDriveLetter(buffer_)) {
              buffer_[1] = ':';
            }
            url_.path_.push_back(buffer_);
          }
          buffer_.clear();
          if (c == '?') {
            url_.query_ = std::string();
            state = State::Query;
          } else if (c == '#') {
            url_.fragment_ = std::string();
            state = State::Fragment;
          }
        } else {
          const std::string_view piece(&input_[pointer], 1);
          util::PercentEncodeInto(piece, util::PercentEncodeSet::Path, buffer_);
        }
        break;

      case State::OpaquePath:
        if (c == '?') {
          url_.query_ = std::string();
          state = State::Query;
        } else if (c == '#') {
          url_.fragment_ = std::string();
          state = State::Fragment;
        } else if (c == ' ') {
          // A space inside an opaque path stays a space -- unless it is the
          // last thing before the query or the fragment, where a literal one
          // would be re-read as part of neither. `data:x ?y` round-trips only
          // because that one space is escaped and the ones before it are not.
          const int next = at(pointer + 1);
          if (next == '?' || next == '#') {
            url_.opaque_path_value_ += "%20";
          } else {
            url_.opaque_path_value_.push_back(' ');
          }
        } else if (c >= 0) {
          const std::string_view piece(&input_[pointer], 1);
          util::PercentEncodeInto(piece, util::PercentEncodeSet::C0Control,
                                  url_.opaque_path_value_);
        }
        break;

      case State::Query:
        // Buffered rather than encoded as it arrives, because the document's
        // character set may be one whose bytes for a character depend on the
        // characters before it. See FlushQuery.
        if (!state_override_.has_value() && c == '#') {
          FlushQuery();
          url_.fragment_ = std::string();
          state = State::Fragment;
        } else if (c >= 0) {
          query_buffer_.push_back(input_[pointer]);
        } else {
          FlushQuery();
        }
        break;

      case State::Fragment:
        if (c >= 0) {
          const std::string_view piece(&input_[pointer], 1);
          util::PercentEncodeInto(piece, util::PercentEncodeSet::Fragment, *url_.fragment_);
        }
        break;
    }
    ++pointer;
  }

  return true;
}

}  // namespace microbrowser::url
