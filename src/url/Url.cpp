#include "url/Url.h"

#include <algorithm>

#include "util/PercentEncoding.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::url {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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

bool IsSingleDot(std::string_view segment) {
  if (segment == ".") {
    return true;
  }
  std::string lowered;
  for (const char c : segment) {
    lowered.push_back(ToLower(c));
  }
  return lowered == "%2e";
}

bool IsDoubleDot(std::string_view segment) {
  if (segment == "..") {
    return true;
  }
  std::string lowered;
  for (const char c : segment) {
    lowered.push_back(ToLower(c));
  }
  return lowered == ".%2e" || lowered == "%2e." || lowered == "%2e%2e";
}

}  // namespace

const std::string Url::empty_;

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
  return scheme_ == "http" || scheme_ == "https" || scheme_ == "ws" || scheme_ == "wss" ||
         scheme_ == "ftp" || scheme_ == "file";
}

std::optional<std::uint16_t> Url::EffectivePort() const {
  return port_.has_value() ? port_ : DefaultPortForScheme(scheme_);
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
  return out.empty() ? std::string() : out;
}

std::string Url::Serialize(bool exclude_fragment) const {
  std::string out = scheme_;
  out.push_back(':');
  if (!host_.IsEmpty() || scheme_ == "file") {
    out += "//";
    if (!username_.empty() || !password_.empty()) {
      out += username_;
      if (!password_.empty()) {
        out.push_back(':');
        out += password_;
      }
      out.push_back('@');
    }
    out += host_.Serialized();
    if (port_.has_value()) {
      out.push_back(':');
      out += std::to_string(*port_);
    }
  } else if (!opaque_path_ && path_.size() > 1 && path_[0].empty()) {
    // Prevents `/​/foo` being re-read as a host on the next parse.
    out += "/.";
  }

  if (opaque_path_) {
    out += opaque_path_value_;
  } else {
    for (const std::string& segment : path_) {
      out.push_back('/');
      out += segment;
    }
  }

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

// The parser proper. One class so the states can share the buffer and the
// output without threading either through twenty functions.
class UrlParser {
 public:
  UrlParser(std::string_view input, const Url* base, const QueryEncoder* query_encoder = nullptr)
      : base_(base), query_encoder_(query_encoder) {
    // Leading and trailing C0 controls and spaces are stripped, and tabs and
    // newlines are removed from anywhere. Both are in the standard because both
    // appear in real HTML, and a parser that kept them would disagree with
    // every other browser about where the URL starts.
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && IsC0ControlOrSpace(input[begin])) {
      ++begin;
    }
    while (end > begin && IsC0ControlOrSpace(input[end - 1])) {
      --end;
    }
    input_.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (!IsTabOrNewline(input[i])) {
        input_.push_back(input[i]);
      }
    }
  }

  std::optional<Url> Run();

 private:
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

  bool SchemeIsSpecial(std::string_view scheme) const {
    return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" ||
           scheme == "ftp" || scheme == "file";
  }

  void ShortenPath() {
    if (url_.opaque_path_ || url_.path_.empty()) {
      return;
    }
    if (url_.scheme_ == "file" && url_.path_.size() == 1 &&
        IsNormalizedWindowsDriveLetter(url_.path_[0])) {
      return;  // a drive letter is not a path segment that can be popped
    }
    url_.path_.pop_back();
  }

  // The standard's "percent-encode after encoding", run over the whole query at
  // once rather than byte by byte as it arrives.
  //
  // It has to be the whole query: an encoding is a function of a *string*, not
  // of a byte -- ISO-2022-JP writes an escape sequence when it changes
  // character set, so the bytes for a character depend on the ones before it,
  // and a per-byte loop would write one escape per character and none of the
  // closing ones.
  void FlushQuery() {
    if (!url_.query_.has_value()) {
      return;
    }
    const bool special = SchemeIsSpecial(url_.scheme_);
    const util::PercentEncodeSet set =
        special ? util::PercentEncodeSet::SpecialQuery : util::PercentEncodeSet::Query;
    // The standard forces UTF-8 back for a non-special scheme and for
    // ws/wss -- see the comment on Url::Parse. `ftp:` and `file:` are special
    // and do take the document's encoding, which looks odd and is what the
    // standard says.
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

  std::string input_;
  const Url* base_;
  const QueryEncoder* query_encoder_ = nullptr;
  Url url_;
  std::string buffer_;
  // The query as the document wrote it, before an encoding is applied to it.
  std::string query_buffer_;
  bool at_sign_seen_ = false;
  bool inside_brackets_ = false;
  bool password_token_seen_ = false;
};

std::optional<Url> UrlParser::Run() {
  State state = State::SchemeStart;
  std::size_t pointer = 0;
  const std::size_t length = input_.size();

  // `c` is the code point at the pointer; one past the end is the EOF marker,
  // which the standard treats as a real position rather than a loop exit.
  const auto at = [&](std::size_t index) -> int {
    return index < length ? static_cast<unsigned char>(input_[index]) : -1;
  };
  while (pointer <= length) {
    const int c = at(pointer);
    switch (state) {
      case State::SchemeStart:
        if (c >= 0 && IsAsciiAlpha(static_cast<char>(c))) {
          buffer_.push_back(ToLower(static_cast<char>(c)));
          state = State::Scheme;
        } else {
          state = State::NoScheme;
          continue;  // reprocess without advancing
        }
        break;

      case State::Scheme:
        if (c >= 0 && (IsAsciiAlphanumeric(static_cast<char>(c)) || c == '+' || c == '-' ||
                       c == '.')) {
          buffer_.push_back(ToLower(static_cast<char>(c)));
        } else if (c == ':') {
          url_.scheme_ = buffer_;
          buffer_.clear();
          if (url_.scheme_ == "file") {
            state = State::File;
          } else if (SchemeIsSpecial(url_.scheme_)) {
            if (base_ != nullptr && base_->scheme_ == url_.scheme_) {
              state = State::SpecialRelativeOrAuthority;
            } else {
              state = State::SpecialAuthoritySlashes;
            }
          } else if (at(pointer + 1) == '/') {
            state = State::PathOrAuthority;
            ++pointer;
          } else {
            url_.opaque_path_ = true;
            url_.opaque_path_value_.clear();
            state = State::OpaquePath;
          }
        } else {
          // Not a scheme after all; start over as a relative reference.
          buffer_.clear();
          state = State::NoScheme;
          pointer = 0;
          continue;
        }
        break;

      case State::NoScheme:
        if (base_ == nullptr) {
          return std::nullopt;
        }
        if (base_->opaque_path_) {
          if (c != '#') {
            return std::nullopt;
          }
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
        if (c == '/' || (SchemeIsSpecial(url_.scheme_) && c == '\\')) {
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
        if (SchemeIsSpecial(url_.scheme_) && (c == '/' || c == '\\')) {
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
        } else if (c < 0 || c == '/' || c == '?' || c == '#' ||
                   (SchemeIsSpecial(url_.scheme_) && c == '\\')) {
          if (at_sign_seen_ && buffer_.empty()) {
            return std::nullopt;  // credentials with no host
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
        if (c == ':' && !inside_brackets_) {
          if (buffer_.empty()) {
            return std::nullopt;
          }
          const auto host = Host::Parse(buffer_, SchemeIsSpecial(url_.scheme_));
          if (!host.has_value()) {
            return std::nullopt;
          }
          url_.host_ = *host;
          buffer_.clear();
          state = State::Port;
        } else if (c < 0 || c == '/' || c == '?' || c == '#' ||
                   (SchemeIsSpecial(url_.scheme_) && c == '\\')) {
          if (SchemeIsSpecial(url_.scheme_) && buffer_.empty()) {
            return std::nullopt;
          }
          const auto host = Host::Parse(buffer_, SchemeIsSpecial(url_.scheme_));
          if (!host.has_value()) {
            return std::nullopt;
          }
          url_.host_ = *host;
          buffer_.clear();
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
        } else if (c < 0 || c == '/' || c == '?' || c == '#' ||
                   (SchemeIsSpecial(url_.scheme_) && c == '\\')) {
          if (!buffer_.empty()) {
            // Accumulated in 32 bits and bounded, so a port of forty digits
            // cannot wrap into a plausible-looking small number.
            std::uint32_t port = 0;
            for (const char digit : buffer_) {
              port = port * 10 + static_cast<std::uint32_t>(digit - '0');
              if (port > 0xFFFF) {
                return std::nullopt;
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
          state = State::PathStart;
          continue;
        } else {
          return std::nullopt;
        }
        break;

      case State::File:
        url_.scheme_ = "file";
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
            if (!IsWindowsDriveLetter(input_.substr(pointer, 2))) {
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
            if (!IsWindowsDriveLetter(input_.substr(pointer, 2)) && !base_->path_.empty() &&
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
          if (IsWindowsDriveLetter(buffer_)) {
            // `file://c:/` — that is a path, not a host.
            state = State::Path;
            continue;
          }
          if (buffer_.empty()) {
            state = State::PathStart;
            continue;
          }
          const auto host = Host::Parse(buffer_, true);
          if (!host.has_value()) {
            return std::nullopt;
          }
          url_.host_ = *host;
          // A file URL's "localhost" host means no host at all.
          if (url_.host_.Serialized() == "localhost") {
            url_.host_ = Host();
          }
          buffer_.clear();
          state = State::PathStart;
          continue;
        }
        buffer_.push_back(static_cast<char>(c));
        break;

      case State::PathStart:
        if (SchemeIsSpecial(url_.scheme_)) {
          state = State::Path;
          if (c != '/' && c != '\\') {
            continue;
          }
        } else if (c == '?') {
          url_.query_ = std::string();
          state = State::Query;
        } else if (c == '#') {
          url_.fragment_ = std::string();
          state = State::Fragment;
        } else if (c >= 0) {
          state = State::Path;
          if (c != '/') {
            continue;
          }
        }
        break;

      case State::Path:
        if (c < 0 || c == '/' || (SchemeIsSpecial(url_.scheme_) && c == '\\') || c == '?' ||
            c == '#') {
          if (IsDoubleDot(buffer_)) {
            ShortenPath();
            if (c != '/' && !(SchemeIsSpecial(url_.scheme_) && c == '\\')) {
              url_.path_.emplace_back();
            }
          } else if (IsSingleDot(buffer_)) {
            if (c != '/' && !(SchemeIsSpecial(url_.scheme_) && c == '\\')) {
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
        } else if (c >= 0) {
          const std::string_view piece(&input_[pointer], 1);
          util::PercentEncodeInto(piece, util::PercentEncodeSet::C0Control, url_.opaque_path_value_);
        }
        break;

      case State::Query:
        if (c == '#') {
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

  if (url_.scheme_.empty()) {
    return std::nullopt;
  }
  if (SchemeIsSpecial(url_.scheme_) && url_.scheme_ != "file" && url_.host_.IsEmpty()) {
    return std::nullopt;
  }
  return url_;
}

std::optional<Url> Url::Parse(std::string_view input) { return Parse(input, nullptr); }

std::optional<Url> Url::Parse(std::string_view input, const Url& base) {
  return Parse(input, base, nullptr);
}

std::optional<Url> Url::Parse(std::string_view input, const QueryEncoder* encoder) {
  AddPerformanceCounter(PerfCounterId::UrlParses);
  UrlParser parser(input, nullptr, encoder);
  auto result = parser.Run();
  if (!result.has_value()) {
    AddPerformanceCounter(PerfCounterId::UrlParseFailures);
  }
  return result;
}

std::optional<Url> Url::Parse(std::string_view input, const Url& base,
                              const QueryEncoder* encoder) {
  AddPerformanceCounter(PerfCounterId::UrlParses);
  UrlParser parser(input, &base, encoder);
  auto result = parser.Run();
  if (!result.has_value()) {
    AddPerformanceCounter(PerfCounterId::UrlParseFailures);
  }
  return result;
}

}  // namespace microbrowser::url
