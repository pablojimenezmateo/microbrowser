#include "net/HttpMessage.h"

#include <algorithm>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::net {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ToLower(a[i]) != ToLower(b[i])) {
      return false;
    }
  }
  return true;
}

// RFC 9110 tchar. A name outside this set is not a header name, and accepting
// one means the next parser in the chain may split the message differently
// from this one — which is what request smuggling is.
bool IsTchar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
      return true;
    default:
      return false;
  }
}

std::string_view TrimOptionalWhitespace(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

bool IsValidHeaderName(std::string_view name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), IsTchar);
}

bool IsHeaderOwnedByFetch(std::string_view name) {
  const auto is = [name](std::string_view expected) {
    return util::EqualsAsciiCaseInsensitive(name, expected);
  };
  return is("origin") || is("access-control-request-method") ||
         is("access-control-request-headers") || is("content-length") ||
         is("transfer-encoding") || is("host") || is("connection") || is("proxy-connection") ||
         is("accept-language") || is("accept-encoding") || is("cookie") || is("referer") ||
         is("user-agent") || is("te") || is("trailer") || is("upgrade");
}

bool DropHeadersOwnedByFetch(HttpHeaders& headers) {
  HttpHeaders kept;
  bool dropped = false;
  for (const HttpHeaders::Field& field : headers.Fields()) {
    if (IsHeaderOwnedByFetch(field.name)) {
      dropped = true;
      continue;
    }
    kept.Add(field.name, field.value);
  }
  if (dropped) {
    headers = std::move(kept);
  }
  return dropped;
}

bool IsValidHeaderValue(std::string_view value) {
  // No CR, no LF, no NUL. Anything else printable is allowed through, including
  // bytes above 0x7F, which appear in the wild and are the server's problem
  // rather than ours to reinterpret.
  return std::none_of(value.begin(), value.end(), [](char c) {
    return c == '\r' || c == '\n' || c == '\0';
  });
}

bool HttpHeaders::Add(std::string_view name, std::string_view value) {
  if (!IsValidHeaderName(name) || !IsValidHeaderValue(value)) {
    AddPerformanceCounter(PerfCounterId::NetHeaderRejections);
    return false;
  }
  fields_.push_back(Field{std::string(name), std::string(value)});
  return true;
}

std::optional<std::string_view> HttpHeaders::Get(std::string_view name) const {
  for (const Field& field : fields_) {
    if (EqualsIgnoringCase(field.name, name)) {
      return std::string_view(field.value);
    }
  }
  return std::nullopt;
}

std::vector<std::string_view> HttpHeaders::GetAll(std::string_view name) const {
  std::vector<std::string_view> values;
  for (const Field& field : fields_) {
    if (EqualsIgnoringCase(field.name, name)) {
      values.emplace_back(field.value);
    }
  }
  return values;
}

std::size_t HttpHeaders::Count(std::string_view name) const {
  std::size_t count = 0;
  for (const Field& field : fields_) {
    if (EqualsIgnoringCase(field.name, name)) {
      ++count;
    }
  }
  return count;
}

bool ResponseParser::Fail(const char* reason) {
  state_ = State::Failed;
  error_ = reason;
  AddPerformanceCounter(PerfCounterId::NetResponseParseFailures);
  return false;
}

bool ResponseParser::Complete() {
  if (state_ != State::Complete) {
    state_ = State::Complete;
    AddPerformanceCounter(PerfCounterId::NetResponsesParsed);
  }
  return true;
}

bool ResponseParser::ParseStatusLine(std::string_view line) {
  // "HTTP/1.1 200 OK"
  if (line.size() < 12 || line.substr(0, 5) != "HTTP/") {
    return Fail("not an HTTP status line");
  }
  const std::size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return Fail("malformed status line");
  }
  const std::string_view version = line.substr(5, first_space - 5);
  if (version != "1.0" && version != "1.1") {
    return Fail("unsupported HTTP version");
  }
  response_.version_minor = version == "1.1" ? 1 : 0;

  std::string_view rest = line.substr(first_space + 1);
  if (rest.size() < 3) {
    return Fail("missing status code");
  }
  const std::string_view code = rest.substr(0, 3);
  const auto parsed = util::ParseInt(code);
  if (!parsed.has_value() || *parsed < 100 || *parsed > 599) {
    return Fail("status code out of range");
  }
  if (rest.size() > 3 && rest[3] != ' ') {
    return Fail("malformed status line");
  }
  response_.status = *parsed;
  if (rest.size() > 4) {
    response_.reason = std::string(TrimOptionalWhitespace(rest.substr(4)));
    if (!IsValidHeaderValue(response_.reason)) {
      return Fail("invalid reason phrase");
    }
  }
  return true;
}

bool ResponseParser::ParseHeaderLine(std::string_view line) {
  // An obs-fold continuation line. RFC 9110 says a recipient must reject or
  // replace it; rejecting is the choice that cannot be got wrong, since the two
  // parsers on either side of a proxy may fold differently.
  if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
    return Fail("obsolete line folding");
  }
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return Fail("header with no colon");
  }
  const std::string_view name = line.substr(0, colon);
  // No space before the colon. `Foo : bar` is rejected rather than trimmed,
  // because a proxy that trims and a server that does not disagree about the
  // header's name.
  if (!IsValidHeaderName(name)) {
    return Fail("invalid header name");
  }
  const std::string_view value = TrimOptionalWhitespace(line.substr(colon + 1));
  if (!response_.headers.Add(name, value)) {
    return Fail("invalid header value");
  }
  if (response_.headers.Size() > limits_.max_header_count) {
    return Fail("too many headers");
  }
  return true;
}

bool ResponseParser::FinishHeaders() {
  const bool has_length = response_.headers.Has("content-length");
  const bool has_encoding = response_.headers.Has("transfer-encoding");

  // **A response to HEAD has no body, whatever its framing headers say.** RFC 9110 §6.4.1: the
  // `Content-Length` on one describes the body a GET would have returned, and reading it as a
  // count of bytes to wait for means waiting for bytes no server will ever send. Decided here,
  // before the two framings are looked at, because it outranks both -- and it is checked before
  // the Content-Length/Transfer-Encoding conflict below for the same reason: a HEAD response
  // carrying both is still a response with no body, and there is no smuggling risk in a message
  // whose body is known to be empty.
  if (head_request_) {
    body_mode_ = BodyMode::None;
    state_ = State::Body;
    return Complete();
  }

  // The request smuggling case. A message carrying both framings is ambiguous,
  // and every documented smuggling attack is two intermediaries resolving that
  // ambiguity differently. There is no safe way to pick one.
  if (has_length && has_encoding) {
    return Fail("both Content-Length and Transfer-Encoding");
  }

  if (has_encoding) {
    if (response_.headers.Count("transfer-encoding") > 1) {
      return Fail("duplicate Transfer-Encoding");
    }
    const std::string_view encoding = *response_.headers.Get("transfer-encoding");
    std::string lowered(encoding);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ToLower);
    if (lowered != "chunked") {
      // `identity`, or a coding we do not implement. Guessing at the framing is
      // how a body is cut short or over-read.
      return Fail("unsupported transfer-encoding");
    }
    body_mode_ = BodyMode::Chunked;
    remaining_ = 0;
    in_chunk_trailer_ = false;
  } else if (has_length) {
    // Two Content-Length headers that disagree is the same ambiguity as above.
    // Two that agree is still a malformed message and still rejected: the
    // benefit of accepting it is zero and the cost is a special case.
    if (response_.headers.Count("content-length") > 1) {
      return Fail("duplicate Content-Length");
    }
    const auto length = util::ParseSize(*response_.headers.Get("content-length"));
    if (!length.has_value()) {
      return Fail("malformed Content-Length");
    }
    if (*length > limits_.max_body) {
      return Fail("body exceeds the limit");
    }
    body_mode_ = BodyMode::Length;
    remaining_ = *length;
  } else if (response_.status == 204 || response_.status == 304 || response_.status < 200) {
    body_mode_ = BodyMode::None;
  } else {
    body_mode_ = BodyMode::UntilClose;
  }

  state_ = State::Body;
  if (body_mode_ == BodyMode::None || (body_mode_ == BodyMode::Length && remaining_ == 0)) {
    return Complete();
  }
  return true;
}

bool ResponseParser::ConsumeBody() {
  while (true) {
    switch (body_mode_) {
      case BodyMode::None:
        return Complete();

      case BodyMode::Length: {
        const std::size_t take = std::min(remaining_, buffer_.size());
        const auto* bytes = reinterpret_cast<const std::byte*>(buffer_.data());
        response_.body.insert(response_.body.end(), bytes, bytes + take);
        buffer_.erase(0, take);
        remaining_ -= take;
        if (remaining_ == 0) {
          return Complete();
        }
        return true;
      }

      case BodyMode::UntilClose: {
        if (response_.body.size() + buffer_.size() > limits_.max_body) {
          return Fail("body exceeds the limit");
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(buffer_.data());
        response_.body.insert(response_.body.end(), bytes, bytes + buffer_.size());
        buffer_.clear();
        return true;
      }

      case BodyMode::ChunkTerminator: {
        const std::size_t newline = buffer_.find('\n');
        if (newline == std::string::npos) {
          if (buffer_.size() > limits_.max_header_line) {
            return Fail("chunk line too long");
          }
          return true;
        }
        std::string_view line(buffer_.data(), newline);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        if (!line.empty()) {
          return Fail("missing chunk terminator");
        }
        buffer_.erase(0, newline + 1);
        body_mode_ = BodyMode::Chunked;
        continue;
      }

      case BodyMode::Chunked: {
        while (true) {
          if (remaining_ > 0) {
            const std::size_t take = std::min(remaining_, buffer_.size());
            const auto* bytes = reinterpret_cast<const std::byte*>(buffer_.data());
            response_.body.insert(response_.body.end(), bytes, bytes + take);
            buffer_.erase(0, take);
            remaining_ -= take;
            if (remaining_ > 0) {
              return true;  // need more bytes
            }
            body_mode_ = BodyMode::ChunkTerminator;
            break;
          }

          const std::size_t newline = buffer_.find('\n');
          if (newline == std::string::npos) {
            if (buffer_.size() > limits_.max_header_line) {
              return Fail("chunk line too long");
            }
            return true;
          }
          std::string_view line(buffer_.data(), newline);
          if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
          }

          if (in_chunk_trailer_) {
            const bool blank = line.empty();
            buffer_.erase(0, newline + 1);
            if (blank) {
              return Complete();
            }
            // Trailer fields. Parsed for validity and discarded: merging them
            // into the header set would let a server change Content-Type after
            // the body was already being interpreted.
            if (line.find(':') == std::string_view::npos) {
              return Fail("malformed trailer");
            }
            continue;
          }

          // "1a3f" or "1a3f;ext=value". The extension is ignored, the size is
          // parsed as hex and bounded before it becomes an allocation.
          const std::size_t semicolon = line.find(';');
          const std::string_view size_text =
              semicolon == std::string_view::npos ? line : line.substr(0, semicolon);
          if (size_text.empty() || size_text.size() > 16) {
            return Fail("malformed chunk size");
          }
          std::size_t size = 0;
          for (const char c : size_text) {
            int digit = -1;
            if (c >= '0' && c <= '9') {
              digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
              digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
              digit = c - 'A' + 10;
            } else {
              return Fail("malformed chunk size");
            }
            size = size * 16 + static_cast<std::size_t>(digit);
            if (size > limits_.max_body) {
              return Fail("chunk exceeds the body limit");
            }
          }
          buffer_.erase(0, newline + 1);

          if (size == 0) {
            in_chunk_trailer_ = true;
            continue;
          }
          if (response_.body.size() + size > limits_.max_body) {
            return Fail("body exceeds the limit");
          }
          remaining_ = size;
        }
        continue;
      }
    }
  }
}

bool ResponseParser::Consume(std::span<const std::byte> data) {
  if (state_ == State::Failed) {
    return false;
  }
  if (state_ == State::Complete) {
    // Bytes after a complete message are the next response on a reused
    // connection, and are not this parser's business.
    return true;
  }

  buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());

  while (state_ == State::StatusLine || state_ == State::Headers) {
    const std::size_t newline = buffer_.find('\n');
    if (newline == std::string::npos) {
      const std::size_t limit =
          state_ == State::StatusLine ? limits_.max_status_line : limits_.max_header_line;
      if (buffer_.size() > limit) {
        return Fail("line too long");
      }
      return true;
    }
    std::string_view line(buffer_.data(), newline);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (state_ == State::StatusLine) {
      if (line.size() > limits_.max_status_line) {
        return Fail("status line too long");
      }
      if (!ParseStatusLine(line)) {
        return false;
      }
      state_ = State::Headers;
    } else if (line.empty()) {
      buffer_.erase(0, newline + 1);
      if (!FinishHeaders()) {
        return false;
      }
      break;
    } else {
      header_bytes_ += line.size();
      if (line.size() > limits_.max_header_line || header_bytes_ > limits_.max_headers_bytes) {
        return Fail("headers too large");
      }
      if (!ParseHeaderLine(line)) {
        return false;
      }
    }
    buffer_.erase(0, newline + 1);
  }

  if (state_ == State::Body) {
    return ConsumeBody();
  }
  return true;
}

bool ResponseParser::Finish() {
  if (state_ == State::Failed) {
    return false;
  }
  if (state_ == State::Complete) {
    return true;
  }
  if (state_ == State::Body && body_mode_ == BodyMode::UntilClose) {
    // A body delimited by the connection closing is complete when it closes.
    return Complete();
  }
  return Fail("connection closed mid-message");
}

std::string SerializeRequest(std::string_view method, std::string_view target,
                             const HttpHeaders& headers) {
  std::string out;
  out.reserve(256);
  out += method;
  out.push_back(' ');
  out += target;
  out += " HTTP/1.1\r\n";
  for (const HttpHeaders::Field& field : headers.Fields()) {
    out += field.name;
    out += ": ";
    out += field.value;
    out += "\r\n";
  }
  out += "\r\n";
  return out;
}

}  // namespace microbrowser::net
