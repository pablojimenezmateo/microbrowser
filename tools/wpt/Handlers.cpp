// The web-platform-tests `.py` handlers this server answers natively. See Handlers.h for why this
// is a table rather than an interpreter, and for the measurement that says it is worth having.
//
// **Every handler here is a transcription, and the original is the specification.** Where one of
// these differs from `third_party/wpt/<path>` the original wins, because a test was written
// against it. The comments name the behaviours that look like mistakes and are not.

#include "wpt/Handlers.h"

#include <algorithm>
#include <cstdlib>
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
  const Query query(request.query);

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
          "hello.py",        "serve-with-content-type.py", "content-type.py"};
}

}  // namespace microbrowser::wpt
