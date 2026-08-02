#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::net {

// HTTP header fields, in order, with case-insensitive names.
//
// Order is kept because it is observable — `Set-Cookie` order decides which of
// two cookies with the same name wins — and because a fixed order is one fewer
// thing for a fingerprinter to measure.
class HttpHeaders {
 public:
  struct Field {
    std::string name;
    std::string value;
  };

  // Returns false and adds nothing if the name or value is not a legal field.
  // A header containing CR or LF is header injection: it does not get sanitized
  // into something harmless, it gets rejected, because "harmless" is a judgment
  // and the request is going to a parser that may not share it.
  bool Add(std::string_view name, std::string_view value);

  // Case-insensitive. Returns the first match; use GetAll for fields that may
  // legitimately repeat.
  std::optional<std::string_view> Get(std::string_view name) const;
  std::vector<std::string_view> GetAll(std::string_view name) const;
  bool Has(std::string_view name) const { return Get(name).has_value(); }
  std::size_t Count(std::string_view name) const;

  const std::vector<Field>& Fields() const { return fields_; }
  std::size_t Size() const { return fields_.size(); }
  void Clear() { fields_.clear(); }

 private:
  std::vector<Field> fields_;
};

bool IsValidHeaderName(std::string_view name);
bool IsValidHeaderValue(std::string_view value);

struct HttpResponse {
  int status = 0;
  std::string reason;
  HttpHeaders headers;
  std::vector<std::byte> body;

  bool IsRedirect() const {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
  }
};

// Limits, applied while parsing rather than after.
//
// A parser that reads a header and then checks its length has already made the
// allocation the limit exists to prevent. Every one of these is checked as the
// bytes arrive.
struct HttpLimits {
  std::size_t max_status_line = 8 * 1024;
  std::size_t max_header_line = 16 * 1024;
  std::size_t max_header_count = 200;
  std::size_t max_headers_bytes = 256 * 1024;
  std::size_t max_body = 64u * 1024u * 1024u;
};

// Incremental HTTP/1.x response parser.
//
// Incremental because responses arrive in pieces and a parser that needs the
// whole message first is a parser that buffers an unbounded amount of attacker
// data before it can decide the message is too large.
class ResponseParser {
 public:
  enum class State : std::uint8_t {
    StatusLine,
    Headers,
    Body,
    Complete,
    Failed,
  };

  explicit ResponseParser(HttpLimits limits = {}) : limits_(limits) {}

  // Consumes as much of `data` as it can. Returns false once the message is
  // malformed; the parser stays failed and never produces a partial response.
  bool Consume(std::span<const std::byte> data);

  // No more bytes are coming. A response whose body length was implied by the
  // connection closing is only complete here.
  bool Finish();

  State GetState() const { return state_; }
  bool IsComplete() const { return state_ == State::Complete; }
  bool Failed() const { return state_ == State::Failed; }
  const char* Error() const { return error_; }

  const HttpResponse& Response() const { return response_; }
  HttpResponse TakeResponse() { return std::move(response_); }

 private:
  bool ParseStatusLine(std::string_view line);
  bool ParseHeaderLine(std::string_view line);
  bool FinishHeaders();
  bool ConsumeBody();
  bool Fail(const char* reason);

  HttpLimits limits_;
  State state_ = State::StatusLine;
  const char* error_ = nullptr;
  HttpResponse response_;
  std::string buffer_;
  std::size_t header_bytes_ = 0;

  enum class BodyMode : std::uint8_t { None, Length, Chunked, ChunkTerminator, UntilClose };
  BodyMode body_mode_ = BodyMode::None;
  std::size_t remaining_ = 0;
  bool in_chunk_trailer_ = false;
};

// Serializes a request. Takes the pieces rather than a struct with defaults so
// that no header is ever sent because a field happened to be left set.
std::string SerializeRequest(std::string_view method, std::string_view target,
                             const HttpHeaders& headers);

}  // namespace microbrowser::net
