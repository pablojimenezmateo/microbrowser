// The web-platform-tests `.py` handlers this server answers natively. See Handlers.h for why this
// is a table rather than an interpreter, and for the measurement that says it is worth having.
//
// **Every handler here is a transcription, and the original is the specification.** Where one of
// these differs from `third_party/wpt/<path>` the original wins, because a test was written
// against it. The comments name the behaviours that look like mistakes and are not.

#include "wpt/Handlers.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::wpt {

namespace {

// --- query strings -----------------------------------------------------------

std::string PercentDecodeQuery(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (text[i] == '%' && i + 2 < text.size()) {
      const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int high = digit(text[i + 1]);
      const int low = digit(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

// The query as an ordered list, because `wptserve`'s `request.GET` is multi-valued: `first(name)`
// is the first occurrence and `GET[name]` is all of them, and `preflight.py` uses both.
class Query {
 public:
  explicit Query(std::string_view raw) {
    std::size_t position = 0;
    while (position <= raw.size() && !raw.empty()) {
      const std::size_t end = raw.find('&', position);
      const std::string_view pair =
          raw.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
      if (!pair.empty()) {
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos) {
          pairs_.emplace_back(PercentDecodeQuery(pair), std::string());
        } else {
          pairs_.emplace_back(PercentDecodeQuery(pair.substr(0, equals)),
                              PercentDecodeQuery(pair.substr(equals + 1)));
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      position = end + 1;
    }
  }

  bool Has(std::string_view name) const {
    return std::any_of(pairs_.begin(), pairs_.end(),
                       [&](const auto& pair) { return pair.first == name; });
  }
  // The first value, or `fallback`. Note that a *present but empty* parameter answers with the
  // empty string rather than the fallback -- `status.py?type=` is how a test asks for an empty
  // Content-Type, and folding it into the default would silently send `text/plain`.
  std::string First(std::string_view name, std::string_view fallback = {}) const {
    for (const auto& pair : pairs_) {
      if (pair.first == name) {
        return pair.second;
      }
    }
    return std::string(fallback);
  }
  std::vector<std::string> All(std::string_view name) const {
    std::vector<std::string> found;
    for (const auto& pair : pairs_) {
      if (pair.first == name) {
        found.push_back(pair.second);
      }
    }
    return found;
  }

 private:
  std::vector<std::pair<std::string, std::string>> pairs_;
};

bool EqualsIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char lhs = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
    const char rhs = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

// One request header by name, case-insensitively. Returns `fallback` when absent, which several
// handlers report literally as the string "NO".
std::string Header(const HandlerRequest& request, std::string_view name,
                   std::string_view fallback = {}) {
  if (request.headers != nullptr) {
    for (const auto& header : *request.headers) {
      if (EqualsIgnoringCase(header.first, name)) {
        return header.second;
      }
    }
  }
  return std::string(fallback);
}

bool HasHeader(const HandlerRequest& request, std::string_view name) {
  if (request.headers == nullptr) {
    return false;
  }
  return std::any_of(request.headers->begin(), request.headers->end(),
                     [&](const auto& header) { return EqualsIgnoringCase(header.first, name); });
}

// The directory the handler lives in, with its trailing slash. `wptserve` scopes the stash by this
// so that two tests using the key "1" in different directories do not collide.
std::string DirectoryOf(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string("/") : std::string(path.substr(0, slash + 1));
}

std::string FileNameOf(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

void SetContentType(HandlerResponse* out, std::string value) {
  out->content_type = std::move(value);
  out->send_content_type = true;
}

// --- the handlers ------------------------------------------------------------

// xhr/resources/status.py -- the response line itself is the subject, so `code` and `text` are both
// under the test's control, and `type` may deliberately be empty.
void Status(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  out->status = std::atoi(query.First("code", "200").c_str());
  out->status_text = query.First("text", "OMG");
  SetContentType(out, query.First("type", ""));
  out->headers.push_back("X-Request-Method: " + std::string(request.method));
  out->body = query.First("content", "");
}

// xhr/resources/content.py -- reflects what arrived back as headers, which is how a test checks
// that its own request was formed correctly. `X-Request-Query` is the literal string "NO" when
// there was none, and that is asserted on.
void Content(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  std::string type = "text/plain";
  if (query.Has("response_charset_label")) {
    type += ";charset=" + query.First("response_charset_label");
  }
  SetContentType(out, type);
  out->headers.push_back("X-Request-Method: " + std::string(request.method));
  out->headers.push_back("X-Request-Query: " +
                         (request.query.empty() ? std::string("NO") : std::string(request.query)));
  out->headers.push_back("X-Request-Content-Length: " + Header(request, "Content-Length", "NO"));
  out->headers.push_back("X-Request-Content-Type: " + Header(request, "Content-Type", "NO"));
  out->body = query.Has("content") ? query.First("content") : std::string(request.body);
}

// common/redirect.py and common/redirect-opt-in.py.
void Redirect(const HandlerRequest& request, const Query& query, bool opt_in,
              HandlerResponse* out) {
  out->status = query.Has("status") ? std::atoi(query.First("status").c_str()) : 302;
  if (out->status < 100 || out->status > 599) {
    out->status = 302;
  }
  out->status_text = "Found";
  out->headers.push_back("Location: " + query.First("location"));
  if (opt_in) {
    out->headers.push_back("Timing-Allow-Origin: *");
  }
  if (query.Has("enable-cors")) {
    const std::string origin = Header(request, "Origin");
    if (!origin.empty()) {
      SetContentType(out, "text/plain");
      out->headers.push_back("Access-Control-Allow-Origin: " + origin);
      out->headers.push_back("Access-Control-Allow-Credentials: true");
    }
  }
}

// xhr/resources/inspect-headers.py -- reports the header *name as the client wrote it*, which is
// why the request's headers are an ordered list here rather than a folded map.
void InspectHeaders(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  if (query.Has("cors")) {
    out->headers.push_back("Access-Control-Allow-Origin: *");
    out->headers.push_back("Access-Control-Allow-Credentials: true");
    out->headers.push_back("Access-Control-Allow-Methods: GET, POST, PUT, FOO");
    out->headers.push_back("Access-Control-Allow-Headers: x-test, x-foo");
    out->headers.push_back(
        "Access-Control-Expose-Headers: x-request-method, x-request-content-type, "
        "x-request-query, x-request-content-length");
  }
  SetContentType(out, "text/plain");
  const std::string filter_value = query.First("filter_value");
  const std::string filter_name = query.First("filter_name");
  std::string result;
  if (request.headers != nullptr) {
    for (const auto& header : *request.headers) {
      if (!filter_value.empty()) {
        if (header.second == filter_value) {
          result += header.first + ",";
        }
      } else if (EqualsIgnoringCase(header.first, filter_name)) {
        result += header.first + ": " + header.second + "\n";
      }
    }
  }
  out->body = result;
}

// xhr/resources/echo-headers.py -- the whole request head, back as text.
void EchoHeaders(const HandlerRequest& request, HandlerResponse* out) {
  SetContentType(out, "text/plain");
  if (request.headers != nullptr) {
    for (const auto& header : *request.headers) {
      out->body += header.first + ": " + header.second + "\r\n";
    }
  }
}

// xhr/resources/echo-method.py, echo-content-type.py, echo-content-cors.py.
void EchoMethod(const HandlerRequest& request, HandlerResponse* out) {
  SetContentType(out, "text/plain");
  out->body = std::string(request.method);
}

void EchoContentType(const HandlerRequest& request, HandlerResponse* out) {
  SetContentType(out, "text/plain");
  out->body = Header(request, "Content-Type");
}

void EchoContentCors(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  SetContentType(out, "text/plain");
  out->headers.push_back("X-Request-Method: " + std::string(request.method));
  out->headers.push_back("X-Request-Content-Length: " + Header(request, "Content-Length", "NO"));
  out->headers.push_back("X-Request-Content-Type: " + Header(request, "Content-Type", "NO"));
  out->headers.push_back("Access-Control-Allow-Credentials: true");
  const std::string origin =
      query.Has("origin") ? query.First("origin") : Header(request, "Origin");
  if (!origin.empty()) {
    out->headers.push_back("Access-Control-Allow-Origin: " + origin);
  }
  if (query.Has("headers")) {
    out->headers.push_back("Access-Control-Allow-Headers: " + query.First("headers"));
  }
  if (query.Has("credentials")) {
    out->headers.push_back("Access-Control-Allow-Credentials: " + query.First("credentials"));
  }
  out->body = std::string(request.body);
}

// common/slow.py, xhr/resources/delay.py, common/trickle.py.
//
// **The delay is deliberately not performed.** This server is single-threaded and forked before
// anything else exists (ADR 0040): sleeping in it stops every other test in the run, and the thing
// under test is almost always "did the response arrive at all" rather than "did it arrive late".
// A test that genuinely measures lateness will fail, and it should -- a *wrong* answer that looks
// timing-dependent is worse than a refusal, so the cases that need real delay want the server to
// grow a timer queue rather than a sleep.
void Slow(HandlerResponse* out) { out->body.clear(); }

void Delay(HandlerResponse* out) {
  out->headers.push_back("Access-Control-Allow-Origin: *");
  out->headers.push_back("Access-Control-Allow-Methods: YO");
  SetContentType(out, "text/plain");
  out->body = "TEST_DELAY";
}

// fetch/api/resources/stash-put.py and stash-take.py, and cors/resources/cors-makeheader.py's
// `check`. One process, one thread, so the stash is a map.
void StashPut(const HandlerRequest& request, const Query& query, Stash& stash,
              HandlerResponse* out) {
  if (request.method == "OPTIONS") {
    out->headers.push_back("Access-Control-Allow-Origin: *");
    out->headers.push_back("Access-Control-Allow-Methods: *");
    out->headers.push_back("Access-Control-Allow-Headers: *");
    out->body = "done";
    return;
  }
  out->headers.push_back("Access-Control-Allow-Origin: *");
  stash.Put(DirectoryOf(request.path), query.First("key"), std::string(request.body));
  out->body = "done";
}

void StashTake(const HandlerRequest& request, const Query& query, Stash& stash,
               HandlerResponse* out) {
  out->headers.push_back("Access-Control-Allow-Origin: *");
  SetContentType(out, "application/json");
  std::string value;
  if (!stash.Take(DirectoryOf(request.path), query.First("key"), &value)) {
    // JSON `null`, which is what `json_handler` serializes a missing stash entry to, and what the
    // test compares against. An empty body would be a parse error at the other end.
    out->body = "null";
    return;
  }
  out->body = "\"" + value + "\"";
}

// cors/resources/cors-makeheader.py -- the CORS suite's workhorse.
void CorsMakeHeader(const HandlerRequest& request, const Query& query, Stash& stash,
                    HandlerResponse* out) {
  const std::string directory = DirectoryOf(request.path);
  if (query.Has("check")) {
    const std::string token = query.First("token");
    std::string value;
    const bool present = stash.Take(directory, token, &value);
    if (present && query.First("check") == "keep") {
      stash.Put(directory, token, value);
    }
    SetContentType(out, "text/plain");
    out->body = present ? "1" : "0";
    return;
  }
  const std::string origin =
      query.Has("origin") ? query.First("origin") : Header(request, "Origin", "none");
  if (origin != "none") {
    out->headers.push_back("Access-Control-Allow-Origin: " + origin);
  }
  if (query.Has("origin2")) {
    out->headers.push_back("Access-Control-Allow-Origin: " + query.First("origin2"));
  }
  if (query.Has("headers")) {
    out->headers.push_back("Access-Control-Allow-Headers: " + query.First("headers"));
  }
  if (query.Has("credentials")) {
    out->headers.push_back("Access-Control-Allow-Credentials: " + query.First("credentials"));
  }
  if (query.Has("methods")) {
    out->headers.push_back("Access-Control-Allow-Methods: " + query.First("methods"));
  }
  int code = query.Has("code") ? std::atoi(query.First("code").c_str()) : 0;
  if (request.method == "OPTIONS") {
    if (query.Has("preflight")) {
      code = std::atoi(query.First("preflight").c_str());
    }
    // The preflight is *recorded*, so a later `check` request can assert it happened. That is the
    // whole mechanism the CORS suite uses to test that a preflight was or was not sent.
    if (query.Has("token")) {
      stash.Put(directory, query.First("token"), "true");
    }
  }
  if (code != 0) {
    out->status = code;
    out->status_text = "OK";
  }
  SetContentType(out, "text/plain");
  out->body = "{\"headers\": {}}";
}

// fetch/api/resources/preflight.py and clean-stash.py.
void Preflight(const HandlerRequest& request, const Query& query, Stash& stash,
               HandlerResponse* out) {
  const std::string directory = DirectoryOf(request.path);
  SetContentType(out, "text/plain");
  const std::vector<std::string> origins = query.All("origin");
  if (origins.empty()) {
    out->headers.push_back("Access-Control-Allow-Origin: *");
  } else {
    for (const std::string& origin : origins) {
      out->headers.push_back("Access-Control-Allow-Origin: " + origin);
    }
  }
  const std::string token = query.First("token");
  if (query.Has("clear-stash")) {
    std::string value;
    out->body = stash.Take(directory, token, &value) ? "1" : "0";
    return;
  }
  if (query.Has("credentials")) {
    out->headers.push_back("Access-Control-Allow-Credentials: true");
  }
  if (request.method == "OPTIONS") {
    if (!HasHeader(request, "Access-Control-Request-Method")) {
      out->status = 400;
      out->status_text = "Bad Request";
      out->body = "Preflight request without Access-Control-Request-Method";
      return;
    }
    if (query.Has("allow_headers")) {
      out->headers.push_back("Access-Control-Allow-Headers: " + query.First("allow_headers"));
    }
    if (query.Has("allow_methods")) {
      out->headers.push_back("Access-Control-Allow-Methods: " + query.First("allow_methods"));
    }
    const std::string max_age = query.First("max_age");
    if (!max_age.empty()) {
      out->headers.push_back("Access-Control-Max-Age: " + max_age);
    }
    if (!token.empty()) {
      stash.Put(directory, token, Header(request, "Access-Control-Request-Headers"));
    }
  }
}

void CleanStash(const HandlerRequest& request, const Query& query, Stash& stash,
                HandlerResponse* out) {
  std::string value;
  out->body = stash.Take(DirectoryOf(request.path), query.First("token"), &value) ? "1" : "0";
  SetContentType(out, "text/plain");
}

// common/blank.html's Python cousin, and the handful that just answer with something.
void Hello(HandlerResponse* out) {
  SetContentType(out, "text/html");
  out->body = "<!doctype html>hello";
}

// xhr/resources/content-type.py and serve-with-content-type.py: the Content-Type is the subject.
void ServeWithContentType(const Query& query, HandlerResponse* out) {
  SetContentType(out, query.First("content_type"));
  out->body = query.First("content", "");
}

// --- responses that are about their own bytes ---------------------------------
//
// Each of these originals calls `response.writer.write(...)` with a whole HTTP response and sets
// `close_connection`. See HandlerResponse::raw: they are testing the parser against something no
// well-formed server sends, so the ordinary path -- which would add Content-Length, Cache-Control
// and Connection -- cannot express them.

// fetch/h1-parsing/resources/status-code.py. The status line is whatever the test asked for, and
// the original's line endings are bare LF, which is part of the test.
void RawStatusCode(const Query& query, HandlerResponse* out) {
  out->has_raw = true;
  out->raw = "HTTP/1.1 " + query.First("input") + "\nheader-parsing: is sad\n";
}

// resource-timing/resources/status-code.py -- the *other* `status-code.py`, and nothing like the one
// above: an ordinary response with the status the test named, plus the two headers resource-timing
// needs to decide whether timing detail is exposed. Kept next to its namesake so the difference is
// visible rather than discovered.
void TimingStatusCode(const Query& query, HandlerResponse* out) {
  const int status = std::atoi(query.First("status").c_str());
  if (status >= 100 && status <= 599) {
    out->status = status;
    // The original passes an empty reason phrase, and it matters here: `Timing-Allow-Origin` is the
    // subject and a substituted phrase would be this server inventing part of the response.
    out->status_text.clear();
  }
  if (query.Has("tao_value")) {
    out->headers.push_back("Timing-Allow-Origin: " + query.First("tao_value"));
  }
  if (query.Has("allow_origin")) {
    out->headers.push_back("Access-Control-Allow-Origin: " + query.First("allow_origin"));
  }
}

// fetch/content-length/resources/content-length.py. The `length` parameter is a whole header line
// and is usually a *wrong* Content-Length -- that is the subject. The body is exactly the
// forty-two bytes the original's comment counts.
void RawContentLength(const Query& query, HandlerResponse* out) {
  out->has_raw = true;
  out->raw = "HTTP/1.1 200 OK\r\n";
  out->raw += "Content-Type: text/plain;charset=UTF-8\r\n";
  out->raw += "Connection: close\r\n";
  out->raw += query.First("length") + "\r\n";
  out->raw += "\r\n";
  out->raw += "Fact: this is really forty-two bytes long.";
}

// fetch/nosniff/resources/nosniff.py. Status 220 and `Content-Type: x/x` are both deliberate; the
// `nosniff` parameter is the whole `X-Content-Type-Options` line, including the cases where it is
// misspelled.
void RawNosniff(const Query& query, HandlerResponse* out) {
  out->has_raw = true;
  out->raw = "HTTP/1.1 220 YOU HAVE NO POWER HERE\r\n";
  out->raw += "Content-Length: 22\r\n";
  out->raw += "Connection: close\r\n";
  out->raw += "Content-Type: x/x\r\n";
  out->raw += query.First("nosniff") + "\r\n";
  out->raw += "\r\n";
  out->raw += "// nothing to see here";
}

// cors/resources/expose-headers.py. Status 221, and the `expose` parameter is a whole header line
// -- the test supplies the `Access-Control-Expose-Headers` it wants to see enforced.
void RawExposeHeaders(const Query& query, HandlerResponse* out) {
  out->has_raw = true;
  out->raw = "HTTP/1.1 221 ALL YOUR BASE BELONG TO H1\r\n";
  out->raw += "Access-Control-Allow-Origin: *\r\n";
  out->raw += "Connection: close\r\n";
  out->raw += "BB-8: hey\r\n";
  out->raw += "Content-Language: mkay\r\n";
  out->raw += query.First("expose") + "\r\n";
  out->raw += "\r\n";
}

// --- ordinary handlers -------------------------------------------------------

// fetch/api/resources/method.py. Reports what arrived, as headers, and echoes the body.
void MethodHandler(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  if (query.Has("cors")) {
    out->headers.push_back("Access-Control-Allow-Origin: *");
    out->headers.push_back("Access-Control-Allow-Credentials: true");
    out->headers.push_back("Access-Control-Allow-Methods: GET, POST, PUT, FOO");
    out->headers.push_back("Access-Control-Allow-Headers: x-test, x-foo");
    out->headers.push_back("Access-Control-Expose-Headers: x-request-method");
  }
  out->headers.push_back("x-request-method: " + std::string(request.method));
  // "NO" rather than an absent header, which is the original's choice and the thing the tests
  // assert on: they distinguish "the browser sent no Content-Type" from "the header never arrived
  // here", and only a present-but-"NO" value can say the first.
  out->headers.push_back("x-request-content-type: " + Header(request, "Content-Type", "NO"));
  out->headers.push_back("x-request-content-length: " + Header(request, "Content-Length", "NO"));
  out->headers.push_back("x-request-content-encoding: " + Header(request, "Content-Encoding", "NO"));
  out->headers.push_back("x-request-content-language: " + Header(request, "Content-Language", "NO"));
  out->headers.push_back("x-request-content-location: " + Header(request, "Content-Location", "NO"));
  out->body = std::string(request.body);
}

// xhr/resources/corsenabled.py.
//
// The original sleeps for `delay`. This does not, for the reason `slow.py` does not: the server is
// single-threaded and forked before anything else exists (ADR 0040), so sleeping in it stops every
// other test in the run. A test that genuinely measures lateness will fail, and should.
void CorsEnabled(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  out->headers.push_back("Access-Control-Allow-Origin: *");
  out->headers.push_back("Access-Control-Allow-Credentials: true");
  out->headers.push_back("Access-Control-Allow-Methods: GET, POST, PUT, FOO");
  out->headers.push_back("Access-Control-Allow-Headers: x-test, x-foo");
  out->headers.push_back(
      "Access-Control-Expose-Headers: x-request-method, x-request-content-type, x-request-query, "
      "x-request-content-length, x-request-data");
  if (query.Has("safelist_content_type")) {
    out->headers.push_back("Access-Control-Allow-Headers: content-type");
  }
  out->headers.push_back("X-Request-Method: " + std::string(request.method));
  out->headers.push_back("X-Request-Query: " +
                         (request.query.empty() ? std::string("NO") : std::string(request.query)));
  out->headers.push_back("X-Request-Content-Length: " + Header(request, "Content-Length", "NO"));
  out->headers.push_back("X-Request-Content-Type: " + Header(request, "Content-Type", "NO"));
  out->headers.push_back("X-Request-Data: " + std::string(request.body));
  out->body = "Test";
}

// fetch/nosniff/resources/image.py, and *only* that one -- see the directory match in
// InvokeHandler; the other three `image.py` files are three different handlers. Serves a real image
// out of
// the checkout with `X-Content-Type-Options: nosniff` and whatever Content-Type the test named,
// which is the point: the test asks whether a wrong type on a real image is honoured.
void ImageHandler(const HandlerRequest& request, const Query& query, HandlerResponse* out) {
  const bool svg = query.First("type").find("svg") != std::string::npos;
  const std::string name = svg ? "green-96x96.svg" : "blue96x96.png";
  out->headers.push_back("X-Content-Type-Options: nosniff");
  if (query.Has("type")) {
    SetContentType(out, query.First("type"));
  }
  if (request.wpt_root.empty()) {
    // No checkout to read from. An empty body rather than a made-up one: the test then fails on the
    // image, which is honest, where invented bytes would fail on the decoder.
    return;
  }
  const std::filesystem::path path =
      std::filesystem::path(std::string(request.wpt_root)) / "images" / name;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out->body = buffer.str();
}

// One JSON string, escaped enough for a header value to survive it.
std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // The four above are the ones that occur; anything else in that range would be a header
          // value no client sent, and JSON forbids it raw.
          out += "\\u00";
          const char* digits = "0123456789abcdef";
          out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
          out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
        } else {
          out.push_back(c);
        }
    }
  }
  out += "\"";
  return out;
}

// fetch/metadata/resources/record-headers.py -- by request count the largest unimplemented handler
// in `fetch/`, and the whole of how `fetch/metadata/` is written: one request records its own
// headers under a key, a later one retrieves them as JSON and asserts on `Sec-Fetch-*`.
//
// **The original hashes the key with MD5 and this does not, deliberately.** The hash is only there
// to turn an arbitrary key into a stash-safe identifier, and both the put and the take hash the
// same input -- so any deterministic function serves, and the identity function serves best. There
// is no MD5 in this tree and adding one to be bug-compatible with a key derivation would be adding
// a broken digest for no observable.
void RecordHeaders(const HandlerRequest& request, const Query& query, Stash& stash,
                   HandlerResponse* out) {
  // The body of a form POST carries `key` and `body` when the query does not.
  const Query posted(request.body);
  const auto parameter = [&](std::string_view name, std::string fallback = {}) {
    if (query.Has(name)) {
      return query.First(name);
    }
    if (posted.Has(name)) {
      return posted.First(name);
    }
    return fallback;
  };

  // Avoids a false positive from a CORS preflight, where the request under test is immediately
  // preceded by an OPTIONS to the same URL: without this the recorded headers would be the
  // preflight's.
  if (query.Has("requireOPTIONS") && request.method != "OPTIONS") {
    return;
  }

  const std::string directory = DirectoryOf(request.path);
  const std::string key = parameter("key");
  if (query.Has("retrieve")) {
    std::string recorded;
    if (!stash.Take(directory, key, &recorded)) {
      // 204, which is what the original answers when nothing was recorded, and which the test reads
      // as "the request never arrived" -- distinct from a 200 with an empty object.
      out->status = 204;
      out->status_text = "No Content";
      return;
    }
    out->body = std::move(recorded);
    return;
  }

  // Recorded as the JSON object of every header, in arrival order. `try/except KeyError: pass` in
  // the original means a second record under one key is silently ignored, so this does not
  // overwrite either.
  std::string recorded;
  if (!stash.Take(directory, key, &recorded)) {
    std::string json = "{";
    if (request.headers != nullptr) {
      bool first = true;
      for (const auto& header : *request.headers) {
        if (!first) {
          json += ", ";
        }
        first = false;
        json += JsonString(header.first) + ": " + JsonString(header.second);
      }
    }
    json += "}";
    stash.Put(directory, key, std::move(json));
  } else {
    // Taken to look, so put it back unchanged -- the first recording wins.
    stash.Put(directory, key, std::move(recorded));
  }

  out->headers.push_back("Access-Control-Allow-Origin: *");
  out->headers.push_back("Cache-Control: no-cache, no-store, must-revalidate");
  out->headers.push_back("Pragma: no-cache");
  out->headers.push_back("Expires: 0");
  if (query.Has("mime")) {
    SetContentType(out, query.First("mime"));
  }
  out->body = parameter("body", "");
}

// common/dispatcher/dispatcher.py -- by request count the single most-asked-for unimplemented
// handler in the tree (1,091 requests across fetch/, cors/ and xhr/ alone).
//
// A per-uuid FIFO queue in the stash, and that is the whole of it: POST appends a body, GET pops
// the front or answers "not ready", and `show-headers` appends the request's headers as JSON. It is
// the transport under `dispatcher.js`, which is how a test coordinates between two browsing
// contexts.
//
// **Implementing it will not by itself make those tests pass, and that is still the reason to do
// it.** They need somewhere to send *from* -- a popup, an iframe with a realm, a worker -- which is
// ADR 0042 §5 and ADR 0022. What this changes is what their failure means: a 501 says nothing about
// whether the browser could have passed, so every one of those tests was filed under "the server
// refused" rather than under the feature it actually needs. The ranked-cause table only works if a
// failure names its own cause.
//
// The original takes a lock around the read-modify-write because wptserve is threaded. This server
// is one thread (ADR 0040), so the Take-then-Put below cannot interleave with anything.
void Dispatcher(const HandlerRequest& request, const Query& query, Stash& stash,
                HandlerResponse* out) {
  out->headers.push_back("Access-Control-Allow-Credentials: true");
  out->headers.push_back("Access-Control-Allow-Methods: OPTIONS, GET, POST");
  out->headers.push_back("Access-Control-Allow-Headers: Content-Type");
  // The requesting origin reflected, or `*`. Credentials are allowed above, so a reflected origin is
  // what makes the pair usable -- `*` with credentials is rejected by every CORS implementation,
  // including this browser's.
  out->headers.push_back("Access-Control-Allow-Origin: " + Header(request, "Origin", "*"));
  if (query.Has("cacheable")) {
    out->headers.push_back("Cache-Control: max-age=31536000");
  } else {
    out->headers.push_back("Cache-Control: no-cache, no-store, must-revalidate");
  }
  if (request.method == "OPTIONS") {
    return;
  }

  // One queue, length-prefixed rather than delimited, because an entry is an arbitrary body: a
  // posted message containing a newline would split into two under any separator.
  const std::string key = "dispatcher:" + query.First("uuid");
  const std::string directory = "/common/dispatcher";
  std::string stored;
  (void)stash.Take(directory, key, &stored);

  std::vector<std::string> queue;
  for (std::size_t at = 0; at < stored.size();) {
    const std::size_t newline = stored.find('\n', at);
    if (newline == std::string::npos) {
      break;
    }
    const std::size_t length = static_cast<std::size_t>(std::strtoull(
        stored.substr(at, newline - at).c_str(), nullptr, 10));
    if (newline + 1 + length > stored.size()) {
      break;  // truncated; the rest is unreadable and dropping it is better than reading past it
    }
    queue.push_back(stored.substr(newline + 1, length));
    at = newline + 1 + length;
  }

  if (query.Has("show-headers")) {
    std::string json = "{";
    if (request.headers != nullptr) {
      bool first = true;
      for (const auto& header : *request.headers) {
        if (!first) {
          json += ", ";
        }
        first = false;
        json += JsonString(header.first) + ": " + JsonString(header.second);
      }
    }
    json += "}";
    queue.push_back(std::move(json));
  } else if (request.method == "POST") {
    queue.emplace_back(request.body);
    out->body = "done";
  } else if (queue.empty()) {
    // The string `dispatcher.js` polls for. Not an error and not an empty body: the client retries
    // until something is queued, and an empty body would read as a delivered empty message.
    out->body = "not ready";
  } else {
    out->body = queue.front();
    queue.erase(queue.begin());
  }

  std::string encoded;
  for (const std::string& entry : queue) {
    encoded += std::to_string(entry.size());
    encoded += '\n';
    encoded += entry;
  }
  stash.Put(directory, key, std::move(encoded));
}

}  // namespace

void Stash::Put(const std::string& directory, const std::string& key, std::string value) {
  values_[{directory, key}] = std::move(value);
}

bool Stash::Take(const std::string& directory, const std::string& key, std::string* out) {
  const auto found = values_.find({directory, key});
  if (found == values_.end()) {
    return false;
  }
  if (out != nullptr) {
    *out = found->second;
  }
  values_.erase(found);
  return true;
}

bool InvokeHandler(const HandlerRequest& request, Stash& stash, HandlerResponse* out) {
  const std::string name = FileNameOf(request.path);
  const std::string directory = DirectoryOf(request.path);
  const Query query(request.query);

  // **A handler name is not unique in the checkout, and three of the ones below would answer
  // wrongly without this.** `image.py` exists four times with four different bodies -- one serves a
  // PNG with `nosniff`, one serves a *different* PNG with `Cross-Origin-Resource-Policy`, one
  // generates a BMP in Python, and one alternates green and red across two requests to test an
  // ETag. `status-code.py` exists twice: `fetch/h1-parsing/`'s writes a raw malformed status line,
  // `resource-timing/`'s sets an ordinary status plus `Timing-Allow-Origin`. `corsenabled.py`
  // exists three times: `xhr/resources/`'s echoes the request, and the two under `auth*/` delegate
  // to `authentication.py` and are HTTP-auth handlers.
  //
  // Serving the transcribed one for a path it was not transcribed from is worse than a 501, and
  // worse in the specific way Handlers.h's opening argument is about: a 501 says "nobody has written
  // this", and a plausible wrong answer says nothing at all while making the test fail somewhere
  // else. So these are matched on their directory, and a copy nobody has transcribed stays a 501 and
  // stays visible in `MICROBROWSER_WPT_HANDLER_REPORT=1`.
  //
  // The same trap is *already* live for ten of the handlers that predate this -- `redirect.py` alone
  // exists eight times with eight distinct bodies -- and it is not fixed here because it is not
  // uniformly a bug: several of those copies are compatible subsets of the transcription (the
  // four-line `redirect.py` under `html/browsers/` does exactly what `common/redirect.py` does with
  // fewer parameters), so a blanket directory match would turn working answers into 501s. Which
  // copies are subsets and which are not needs deciding one at a time. TD-0061 has the audit.
  const auto transcribed_from = [&](std::string_view expected) { return directory == expected; };

  if (name == "status.py") {
    Status(request, query, out);
  } else if (name == "content.py") {
    Content(request, query, out);
  } else if (name == "redirect.py") {
    Redirect(request, query, false, out);
  } else if (name == "redirect-opt-in.py") {
    Redirect(request, query, true, out);
  } else if (name == "inspect-headers.py") {
    InspectHeaders(request, query, out);
  } else if (name == "echo-headers.py") {
    EchoHeaders(request, out);
  } else if (name == "echo-method.py") {
    EchoMethod(request, out);
  } else if (name == "echo-content-type.py") {
    EchoContentType(request, out);
  } else if (name == "echo-content-cors.py" || name == "echo-content.py") {
    EchoContentCors(request, query, out);
  } else if (name == "slow.py" || name == "trickle.py") {
    Slow(out);
  } else if (name == "delay.py") {
    Delay(out);
  } else if (name == "stash-put.py") {
    StashPut(request, query, stash, out);
  } else if (name == "stash-take.py") {
    StashTake(request, query, stash, out);
  } else if (name == "cors-makeheader.py") {
    CorsMakeHeader(request, query, stash, out);
  } else if (name == "preflight.py") {
    Preflight(request, query, stash, out);
  } else if (name == "clean-stash.py") {
    CleanStash(request, query, stash, out);
  } else if (name == "hello.py") {
    Hello(out);
  } else if (name == "status-code.py" && transcribed_from("fetch/h1-parsing/resources/")) {
    RawStatusCode(query, out);
  } else if (name == "status-code.py" && transcribed_from("resource-timing/resources/")) {
    TimingStatusCode(query, out);
  } else if (name == "content-length.py") {
    RawContentLength(query, out);
  } else if (name == "nosniff.py") {
    RawNosniff(query, out);
  } else if (name == "expose-headers.py") {
    RawExposeHeaders(query, out);
  } else if (name == "method.py") {
    MethodHandler(request, query, out);
  } else if (name == "corsenabled.py" && transcribed_from("xhr/resources/")) {
    CorsEnabled(request, query, out);
  } else if (name == "image.py" && transcribed_from("fetch/nosniff/resources/")) {
    ImageHandler(request, query, out);
  } else if (name == "record-headers.py") {
    RecordHeaders(request, query, stash, out);
  } else if (name == "dispatcher.py") {
    Dispatcher(request, query, stash, out);
  } else if (name == "serve-with-content-type.py" || name == "content-type.py") {
    ServeWithContentType(query, out);
  } else {
    return false;
  }
  return true;
}

std::vector<std::string_view> ImplementedHandlers() {
  return {"status.py",       "content.py",          "redirect.py",
          "redirect-opt-in.py", "inspect-headers.py", "echo-headers.py",
          "echo-method.py",  "echo-content-type.py", "echo-content-cors.py",
          "echo-content.py", "slow.py",             "trickle.py",
          "delay.py",        "stash-put.py",        "stash-take.py",
          "cors-makeheader.py", "preflight.py",     "clean-stash.py",
          "hello.py",        "serve-with-content-type.py", "content-type.py",
          "status-code.py",  "content-length.py",   "nosniff.py",
          "expose-headers.py", "method.py",         "corsenabled.py",
          "image.py",        "record-headers.py",   "dispatcher.py"};
}

}  // namespace microbrowser::wpt
