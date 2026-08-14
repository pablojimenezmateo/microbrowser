// The `.py` handlers, transcribed. See Handlers.h for why this exists at all and what keeps it
// honest; each function below quotes the file it is a transcription of.

#include "wpt/Handlers.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace microbrowser::wpt {

namespace {

using Query = std::vector<std::pair<std::string, std::string>>;

bool EqualsAsciiCaseInsensitive(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string PercentDecodePlus(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out.push_back(' ');
      continue;
    }
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = HexDigit(text[i + 1]);
      const int low = HexDigit(text[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

// `int(...)` on a query parameter, with wptserve's own behaviour on a value that is not a number:
// most handlers let the `ValueError` escape, which upstream turns into a 500. Answering the fallback
// is the one place this deviates, and it deviates towards the test still running.
int ToInt(std::string_view text, int fallback) {
  if (text.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long value = std::strtol(std::string(text).c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return fallback;
  }
  return static_cast<int>(value);
}

std::vector<std::string> Split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t found = text.find(separator, start);
    if (found == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      return parts;
    }
    parts.emplace_back(text.substr(start, found - start));
    start = found + 1;
  }
}

// Everything a handler returns as a header, without a Content-Type of its own, is text/plain here:
// that is what wptserve's default is for a handler that sets none, and it matters because a response
// with no type is content-sniffed and several of these tests assert on the type they got.
void SetHeader(HandlerResponse& response, std::string name, std::string value) {
  for (auto& [existing, current] : response.headers) {
    if (EqualsAsciiCaseInsensitive(existing, name)) {
      current = std::move(value);
      return;
    }
  }
  response.headers.emplace_back(std::move(name), std::move(value));
}

void AddHeader(HandlerResponse& response, std::string name, std::string value) {
  response.headers.emplace_back(std::move(name), std::move(value));
}

}  // namespace

std::optional<std::string> Stash::Take(const std::string& key) {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return std::nullopt;
  }
  std::string value = std::move(found->second);
  entries_.erase(found);
  return value;
}

const std::string* Stash::Peek(const std::string& key) const {
  const auto found = entries_.find(key);
  return found == entries_.end() ? nullptr : &found->second;
}

Query ParseQuery(std::string_view query) {
  Query out;
  if (query.empty()) {
    return out;
  }
  for (const std::string& pair : Split(query, '&')) {
    if (pair.empty()) {
      continue;
    }
    const std::size_t equals = pair.find('=');
    if (equals == std::string::npos) {
      out.emplace_back(PercentDecodePlus(pair), std::string());
    } else {
      out.emplace_back(PercentDecodePlus(std::string_view(pair).substr(0, equals)),
                       PercentDecodePlus(std::string_view(pair).substr(equals + 1)));
    }
  }
  return out;
}

std::string QueryFirst(const Query& query, std::string_view name, std::string_view fallback) {
  for (const auto& [key, value] : query) {
    if (key == name) {
      return value;
    }
  }
  return std::string(fallback);
}

bool QueryHas(const Query& query, std::string_view name) {
  for (const auto& [key, value] : query) {
    if (key == name) {
      return true;
    }
  }
  return false;
}

std::string HeaderValue(const HandlerRequest& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (EqualsAsciiCaseInsensitive(key, name)) {
      return value;
    }
  }
  return {};
}

namespace {

// --- common/slow.py -------------------------------------------------------------------------
//
//   def main(request, response):
//       delay = float(request.GET.first(b"delay", 2000)) / 1000
//       time.sleep(delay)
//       return 200, [], b''
//
// The sleep is a *delay* here rather than a sleep, for the reason `HandlerResponse::delay_ms`
// gives: this server has one thread and six test processes are usually talking to it.
HandlerResponse SlowHandler(const Query& query) {
  HandlerResponse response;
  response.handled = true;
  response.delay_ms = ToInt(QueryFirst(query, "delay", "2000"), 2000);
  SetHeader(response, "Content-Type", "text/plain");
  return response;
}

// --- common/redirect.py ---------------------------------------------------------------------
//
//   status = 302; if b"status" in request.GET: status = int(...)
//   response.headers.set(b"Location", request.GET.first(b"location"))
//   if request.GET.get(b"enable-cors") is not None and Origin: ACAO: origin, ACAC: true
HandlerResponse CommonRedirectHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  response.status = ToInt(QueryFirst(query, "status", "302"), 302);
  response.status_text = "Found";
  SetHeader(response, "Location", QueryFirst(query, "location"));
  if (QueryHas(query, "enable-cors")) {
    const std::string origin = HeaderValue(request, "Origin");
    if (!origin.empty()) {
      SetHeader(response, "Content-Type", "text/plain");
      SetHeader(response, "Access-Control-Allow-Origin", origin);
      SetHeader(response, "Access-Control-Allow-Credentials", "true");
    }
  }
  return response;
}

// --- common/echo.py -------------------------------------------------------------------------
//
//   response.headers.set(b"X-XSS-Protection", b"0")
//   response.headers.set(b"Content-Type", b"text/html")
//   response.content = request.GET.first(b"content")
HandlerResponse EchoHandler(const Query& query) {
  HandlerResponse response;
  response.handled = true;
  SetHeader(response, "X-XSS-Protection", "0");
  SetHeader(response, "Content-Type", "text/html");
  response.body = QueryFirst(query, "content");
  return response;
}

// --- fetch/api/resources/status.py ------------------------------------------------------------
//
//   code = int(request.GET.first(b"code", 200)); text = ...(b"text", b"OMG")
//   content = ...(b"content", b""); type = ...(b"type", b"")
//   headers = [Content-Type: type, X-Request-Method: request.method]
HandlerResponse StatusHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  response.status = ToInt(QueryFirst(query, "code", "200"), 200);
  response.status_text = QueryFirst(query, "text", "OMG");
  AddHeader(response, "Content-Type", QueryFirst(query, "type"));
  AddHeader(response, "X-Request-Method", request.method);
  response.body = QueryFirst(query, "content");
  return response;
}

// --- fetch/api/resources/method.py ------------------------------------------------------------
HandlerResponse MethodHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  if (QueryHas(query, "cors")) {
    AddHeader(response, "Access-Control-Allow-Origin", "*");
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
    AddHeader(response, "Access-Control-Allow-Methods", "GET, POST, PUT, FOO");
    AddHeader(response, "Access-Control-Allow-Headers", "x-test, x-foo");
    AddHeader(response, "Access-Control-Expose-Headers", "x-request-method");
  }
  AddHeader(response, "x-request-method", request.method);
  // `request.headers.get(name, b"NO")` -- the literal string "NO" is what the tests assert against
  // for an absent header, so an empty string here would fail them for the wrong reason.
  for (const char* name : {"Content-Type", "Content-Length", "Content-Encoding", "Content-Language",
                           "Content-Location"}) {
    const std::string value = HeaderValue(request, name);
    std::string lowered = std::string("x-request-") + name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    AddHeader(response, lowered, value.empty() ? "NO" : value);
  }
  response.body = request.body;
  return response;
}

// --- fetch/api/resources/echo-content.py ------------------------------------------------------
HandlerResponse EchoContentHandler(const HandlerRequest& request) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "X-Request-Method", request.method);
  const std::string length = HeaderValue(request, "Content-Length");
  const std::string type = HeaderValue(request, "Content-Type");
  AddHeader(response, "X-Request-Content-Length", length.empty() ? "NO" : length);
  AddHeader(response, "X-Request-Content-Type", type.empty() ? "NO" : type);
  // Upstream's comment: "Avoid any kind of content sniffing on the response."
  AddHeader(response, "Content-Type", "text/plain");
  response.body = request.body;
  return response;
}

// --- fetch/api/resources/inspect-headers.py ---------------------------------------------------
HandlerResponse InspectHeadersHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  std::vector<std::string> checked;
  if (QueryHas(query, "headers")) {
    checked = Split(QueryFirst(query, "headers"), '|');
    for (const std::string& header : checked) {
      const std::string value = HeaderValue(request, header);
      if (!value.empty()) {
        AddHeader(response, "x-request-" + header, value);
      }
    }
  }
  if (QueryHas(query, "cors")) {
    const std::string origin = HeaderValue(request, "Origin");
    AddHeader(response, "Access-Control-Allow-Origin", origin.empty() ? "*" : origin);
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
    AddHeader(response, "Access-Control-Allow-Methods", "GET, POST, HEAD");
    std::string exposed;
    for (const std::string& header : checked) {
      if (!exposed.empty()) {
        exposed += ", ";
      }
      exposed += "x-request-" + header;
    }
    AddHeader(response, "Access-Control-Expose-Headers", exposed);
    if (QueryHas(query, "allow_headers")) {
      AddHeader(response, "Access-Control-Allow-Headers", QueryFirst(query, "allow_headers"));
    }
  }
  SetHeader(response, "Content-Type", "text/plain");
  return response;
}

// --- fetch/api/resources/preflight.py ---------------------------------------------------------
//
// The stash entry is three fields; upstream keeps a dict and this keeps the same three joined by a
// character no header value contains. A structured store would be a second format to keep in step
// with a file that already has one.
struct PreflightData {
  std::string control_request_headers;
  std::string preflight = "0";
  std::string preflight_referrer;
  std::string preflight_user_agent;
};

std::string EncodePreflight(const PreflightData& data) {
  return data.control_request_headers + '\n' + data.preflight + '\n' + data.preflight_referrer +
         '\n' + data.preflight_user_agent;
}

PreflightData DecodePreflight(const std::string& text) {
  const std::vector<std::string> parts = Split(text, '\n');
  PreflightData data;
  if (parts.size() > 0) {
    data.control_request_headers = parts[0];
  }
  if (parts.size() > 1) {
    data.preflight = parts[1];
  }
  if (parts.size() > 2) {
    data.preflight_referrer = parts[2];
  }
  if (parts.size() > 3) {
    data.preflight_user_agent = parts[3];
  }
  return data;
}

HandlerResponse PreflightHandler(const HandlerRequest& request, const Query& query, Stash& stash) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Content-Type", "text/plain");
  PreflightData data;
  const std::string token = QueryFirst(query, "token");

  if (QueryHas(query, "origin")) {
    for (const std::string& origin : Split(QueryFirst(query, "origin"), ',')) {
      std::string trimmed = origin;
      while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.erase(0, 1);
      }
      AddHeader(response, "Access-Control-Allow-Origin", trimmed);
    }
  } else {
    AddHeader(response, "Access-Control-Allow-Origin", "*");
  }

  if (QueryHas(query, "clear-stash")) {
    response.body = stash.Take(token).has_value() ? "1" : "0";
    return response;
  }
  if (QueryHas(query, "credentials")) {
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
  }

  if (request.method == "OPTIONS") {
    if (HeaderValue(request, "Access-Control-Request-Method").empty()) {
      response.status = 400;
      response.status_text = "No Access-Control-Request-Method header";
      response.body = "ERROR: No access-control-request-method in preflight!";
      return response;
    }
    if (HeaderValue(request, "Accept") != "*/*") {
      response.status = 400;
      response.status_text = "Request does not have 'Accept: */*' header";
      response.body = "ERROR: Invalid access in preflight!";
      return response;
    }
    if (QueryHas(query, "control_request_headers")) {
      data.control_request_headers = HeaderValue(request, "Access-Control-Request-Headers");
    }
    if (QueryHas(query, "max_age")) {
      AddHeader(response, "Access-Control-Max-Age", QueryFirst(query, "max_age"));
    }
    if (QueryHas(query, "allow_headers")) {
      AddHeader(response, "Access-Control-Allow-Headers", QueryFirst(query, "allow_headers"));
    }
    if (QueryHas(query, "allow_methods")) {
      AddHeader(response, "Access-Control-Allow-Methods", QueryFirst(query, "allow_methods"));
    }
    data.preflight = "1";
    data.preflight_referrer = HeaderValue(request, "Referer");
    data.preflight_user_agent = HeaderValue(request, "User-Agent");
    if (!token.empty()) {
      stash.Put(token, EncodePreflight(data));
    }
    response.status = ToInt(QueryFirst(query, "preflight_status", "200"), 200);
    return response;
  }

  if (!token.empty()) {
    if (const std::optional<std::string> stored = stash.Take(token); stored.has_value()) {
      data = DecodePreflight(*stored);
    }
  }
  if (QueryHas(query, "checkUserAgentHeaderInPreflight") &&
      HeaderValue(request, "User-Agent") != data.preflight_user_agent) {
    response.status = 400;
    response.body = "ERROR: No user-agent header in preflight";
    return response;
  }
  AddHeader(response, "Access-Control-Expose-Headers",
            "x-did-preflight, x-control-request-headers, x-referrer, x-preflight-referrer, "
            "x-origin");
  AddHeader(response, "x-did-preflight", data.preflight);
  AddHeader(response, "x-control-request-headers", data.control_request_headers);
  AddHeader(response, "x-preflight-referrer", data.preflight_referrer);
  AddHeader(response, "x-referrer", HeaderValue(request, "Referer"));
  AddHeader(response, "x-origin", HeaderValue(request, "Origin"));
  if (!token.empty()) {
    stash.Put(token, EncodePreflight(data));
  }
  return response;
}

// --- fetch/api/resources/redirect.py ----------------------------------------------------------
//
// The count in the stash is what makes `max_count` work, and the appended `&count=N` is what makes
// the Location change between hops -- a browser that cached the redirect target would otherwise
// short-circuit the loop the test is measuring.
HandlerResponse FetchRedirectHandler(const HandlerRequest& request, const Query& query,
                                     Stash& stash) {
  HandlerResponse response;
  response.handled = true;
  int status = 302;
  AddHeader(response, "Content-Type", "text/plain");
  AddHeader(response, "Cache-Control", "no-cache");
  AddHeader(response, "Pragma", "no-cache");
  const std::string origin = HeaderValue(request, "Origin");
  if (!origin.empty()) {
    AddHeader(response, "Access-Control-Allow-Origin", origin);
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
  } else {
    AddHeader(response, "Access-Control-Allow-Origin", "*");
  }

  const std::string token = QueryFirst(query, "token");
  int count = 0;
  std::string preflight = "0";
  if (!token.empty()) {
    if (const std::optional<std::string> stored = stash.Take(token); stored.has_value()) {
      const std::vector<std::string> parts = Split(*stored, '\n');
      if (parts.size() > 0) {
        count = ToInt(parts[0], 0);
      }
      if (parts.size() > 1) {
        preflight = parts[1];
      }
    }
  }

  if (request.method == "OPTIONS") {
    if (QueryHas(query, "allow_headers")) {
      AddHeader(response, "Access-Control-Allow-Headers", QueryFirst(query, "allow_headers"));
    }
    preflight = "1";
    if (!QueryHas(query, "redirect_preflight")) {
      if (!token.empty()) {
        stash.Put(token, std::to_string(count) + "\n" + preflight);
      }
      response.status = 200;
      return response;
    }
  }

  if (QueryHas(query, "redirect_status")) {
    status = ToInt(QueryFirst(query, "redirect_status", "302"), 302);
  }
  ++count;

  if (QueryHas(query, "location")) {
    std::string url = QueryFirst(query, "location");
    if (!QueryHas(query, "simple")) {
      // Upstream only rewrites when the location has no scheme or an http(s) one.
      const std::size_t colon = url.find(':');
      const std::size_t slash = url.find('/');
      const std::string scheme =
          (colon != std::string::npos && (slash == std::string::npos || colon < slash))
              ? url.substr(0, colon)
              : std::string();
      if (scheme.empty() || scheme == "http" || scheme == "https") {
        url += url.find('?') != std::string::npos ? "&" : "?";
        bool first = true;
        for (const auto& [key, value] : query) {
          if (!first) {
            url += "&";
          }
          first = false;
          url += key + "=" + value;
        }
        url += "&count=" + std::to_string(count);
      }
    }
    AddHeader(response, "Location", url);
  }
  if (QueryHas(query, "redirect_referrerpolicy")) {
    AddHeader(response, "Referrer-Policy", QueryFirst(query, "redirect_referrerpolicy"));
  }
  if (QueryHas(query, "delay")) {
    response.delay_ms = ToInt(QueryFirst(query, "delay", "0"), 0);
  }
  if (!token.empty()) {
    stash.Put(token, std::to_string(count) + "\n" + preflight);
    if (QueryHas(query, "max_count")) {
      const int max_count = ToInt(QueryFirst(query, "max_count", "0"), 0);
      if (count > max_count) {
        // Upstream returns the count as a *body* with a 200, which is how the test reads it back.
        response.status = 200;
        response.body = std::to_string(count - 1);
        return response;
      }
    }
  }
  response.status = status;
  return response;
}

// --- xhr/resources/redirect.py ----------------------------------------------------------------
HandlerResponse XhrRedirectHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  const int code = ToInt(QueryFirst(query, "code", "302"), 302);
  std::string location = QueryFirst(query, "location");
  if (location.empty()) {
    location = "/" + request.path + "?followed";
  }
  if (location.rfind("redirect.py", 0) == 0) {
    location += "&code=" + std::to_string(code);
  }
  if (QueryHas(query, "delay")) {
    response.delay_ms = ToInt(QueryFirst(query, "delay", "0"), 0);
  }
  if (QueryHas(query, "followed")) {
    AddHeader(response, "Content-Type", "text/plain");
    response.body = "MAGIC HAPPENED";
    return response;
  }
  response.status = code;
  response.status_text = "WEBSRT MARKETING";
  AddHeader(response, "Location", location);
  response.body = "TEST";
  return response;
}

// --- xhr/resources/content.py -----------------------------------------------------------------
HandlerResponse XhrContentHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  std::string type = "text/plain";
  if (QueryHas(query, "response_charset_label")) {
    type += ";charset=" + QueryFirst(query, "response_charset_label");
  }
  AddHeader(response, "Content-type", type);
  AddHeader(response, "X-Request-Method", request.method);
  // `request.url_parts.query` is the *raw* query, not a decoded parameter -- and `"NO"` when there
  // is none, which several tests assert against directly.
  AddHeader(response, "X-Request-Query", request.query.empty() ? "NO" : request.query);
  const std::string length = HeaderValue(request, "Content-Length");
  const std::string content_type = HeaderValue(request, "Content-Type");
  AddHeader(response, "X-Request-Content-Length", length.empty() ? "NO" : length);
  AddHeader(response, "X-Request-Content-Type", content_type.empty() ? "NO" : content_type);
  response.body = QueryHas(query, "content") ? QueryFirst(query, "content") : request.body;
  return response;
}

// --- xhr/resources/delay.py -------------------------------------------------------------------
HandlerResponse XhrDelayHandler(const Query& query) {
  HandlerResponse response;
  response.handled = true;
  response.delay_ms = ToInt(QueryFirst(query, "ms", "500"), 500);
  AddHeader(response, "Access-Control-Allow-Origin", "*");
  AddHeader(response, "Access-Control-Allow-Methods", "YO");
  AddHeader(response, "Content-type", "text/plain");
  response.body = "TEST_DELAY";
  return response;
}

// --- xhr/resources/corsenabled.py -------------------------------------------------------------
HandlerResponse XhrCorsEnabledHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Access-Control-Allow-Origin", "*");
  AddHeader(response, "Access-Control-Allow-Credentials", "true");
  AddHeader(response, "Access-Control-Allow-Methods", "GET, POST, PUT, FOO");
  AddHeader(response, "Access-Control-Allow-Headers", "x-test, x-foo");
  AddHeader(response, "Access-Control-Expose-Headers",
            "x-request-method, x-request-content-type, x-request-query, x-request-content-length, "
            "x-request-data");
  if (QueryHas(query, "delay")) {
    // Upstream's `time.sleep(delay)` -- *seconds*, unlike every other handler in this directory.
    response.delay_ms = ToInt(QueryFirst(query, "delay", "0"), 0) * 1000;
  }
  if (QueryHas(query, "safelist_content_type")) {
    AddHeader(response, "Access-Control-Allow-Headers", "content-type");
  }
  AddHeader(response, "X-Request-Method", request.method);
  AddHeader(response, "X-Request-Query", request.query.empty() ? "NO" : request.query);
  const std::string length = HeaderValue(request, "Content-Length");
  const std::string type = HeaderValue(request, "Content-Type");
  AddHeader(response, "X-Request-Content-Length", length.empty() ? "NO" : length);
  AddHeader(response, "X-Request-Content-Type", type.empty() ? "NO" : type);
  AddHeader(response, "X-Request-Data", request.body);
  response.body = "Test";
  return response;
}

// --- xhr/resources/inspect-headers.py ---------------------------------------------------------
//
// A different file from the fetch/ one of the same name and a different behaviour: this reports the
// *request's* headers back in the body, filtered either by value or by name.
HandlerResponse XhrInspectHeadersHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  if (QueryHas(query, "cors")) {
    AddHeader(response, "Access-Control-Allow-Origin", "*");
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
    AddHeader(response, "Access-Control-Allow-Methods", "GET, POST, PUT, FOO");
    AddHeader(response, "Access-Control-Allow-Headers", "x-test, x-foo");
    AddHeader(response, "Access-Control-Expose-Headers",
              "x-request-method, x-request-content-type, x-request-query, "
              "x-request-content-length");
  }
  AddHeader(response, "content-type", "text/plain");
  const std::string filter_value = QueryFirst(query, "filter_value");
  std::string filter_name = QueryFirst(query, "filter_name");
  std::transform(filter_name.begin(), filter_name.end(), filter_name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& [name, value] : request.headers) {
    if (!filter_value.empty()) {
      if (value == filter_value) {
        response.body += name + ",";
      }
    } else if (EqualsAsciiCaseInsensitive(name, filter_name)) {
      response.body += name + ": " + value + "\n";
    }
  }
  return response;
}

// --- xhr/resources/headers.py -----------------------------------------------------------------
HandlerResponse XhrHeadersHandler() {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Content-Type", "text/plain");
  AddHeader(response, "X-Custom-Header", "test");
  AddHeader(response, "Set-Cookie", "test");
  AddHeader(response, "Set-Cookie2", "test");
  AddHeader(response, "X-Custom-Header-Empty", "");
  AddHeader(response, "X-Custom-Header-Comma", "1");
  AddHeader(response, "X-Custom-Header-Comma", "2");
  AddHeader(response, "X-Custom-Header-Bytes", "\xe2\x80\xa6");  // U+2026, as UTF-8
  response.body = "TEST";
  return response;
}

// --- xhr/resources/requri.py ------------------------------------------------------------------
HandlerResponse XhrRequestUriHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Content-Type", "text/plain");
  const std::string path = "/" + request.path + (request.query.empty() ? "" : "?" + request.query);
  response.body = QueryHas(query, "full") ? request.origin + path : path;
  return response;
}

// --- xhr/resources/echo-content-type.py -------------------------------------------------------
HandlerResponse XhrEchoContentTypeHandler(const HandlerRequest& request) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Content-Type", "text/plain");
  response.body = HeaderValue(request, "Content-Type");
  return response;
}

// --- xhr/resources/echo-content-cors.py -------------------------------------------------------
HandlerResponse XhrEchoContentCorsHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "X-Request-Method", request.method);
  const std::string length = HeaderValue(request, "Content-Length");
  const std::string type = HeaderValue(request, "Content-Type");
  AddHeader(response, "X-Request-Content-Length", length.empty() ? "NO" : length);
  AddHeader(response, "X-Request-Content-Type", type.empty() ? "NO" : type);
  AddHeader(response, "Access-Control-Allow-Credentials", "true");
  AddHeader(response, "Content-Type", "text/plain");
  // Upstream reads `request.GET.first(b"origin", <header>)` for all three, which is a bug in the
  // handler rather than in this transcription -- the same `origin` parameter is the fallback for
  // the headers and the methods. Transcribed as written, because a fixed version would answer
  // differently from the file the test was written against.
  const std::string origin =
      QueryHas(query, "origin") ? QueryFirst(query, "origin") : HeaderValue(request, "Origin");
  if (!origin.empty()) {
    AddHeader(response, "Access-Control-Allow-Origin", origin);
  }
  const std::string request_headers =
      QueryHas(query, "origin") ? QueryFirst(query, "origin")
                                : HeaderValue(request, "Access-Control-Request-Headers");
  if (!request_headers.empty()) {
    AddHeader(response, "Access-Control-Allow-Headers", request_headers);
  }
  const std::string request_method =
      QueryHas(query, "origin") ? QueryFirst(query, "origin")
                                : HeaderValue(request, "Access-Control-Request-Method");
  if (!request_method.empty()) {
    AddHeader(response, "Access-Control-Allow-Methods", "OPTIONS, " + request_method);
  }
  response.body = request.body;
  return response;
}

// --- xhr/resources/redirect-cors.py -----------------------------------------------------------
HandlerResponse XhrRedirectCorsHandler(const HandlerRequest& request, const Query& query) {
  HandlerResponse response;
  response.handled = true;
  const std::string location = QueryFirst(query, "location");
  if (request.method == "OPTIONS") {
    if (QueryHas(query, "redirect_preflight")) {
      response.status = 302;
      AddHeader(response, "Location", location);
    } else {
      response.status = 200;
    }
    AddHeader(response, "Access-Control-Allow-Methods", "GET");
    AddHeader(response, "Access-Control-Max-Age", "1");
  } else if (request.method == "GET") {
    response.status = 302;
    AddHeader(response, "Location", location);
  }
  if (QueryHas(query, "allow_origin")) {
    AddHeader(response, "Access-Control-Allow-Origin", HeaderValue(request, "Origin"));
  }
  if (QueryHas(query, "allow_header")) {
    AddHeader(response, "Access-Control-Allow-Headers", QueryFirst(query, "allow_header"));
  }
  return response;
}

// --- xhr/resources/reset-token.py -------------------------------------------------------------
HandlerResponse XhrResetTokenHandler(const HandlerRequest& request, const Query& query,
                                     Stash& stash) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Access-Control-Allow-Origin", HeaderValue(request, "Origin"));
  stash.Put(QueryFirst(query, "token"), std::string());
  AddHeader(response, "Content-Type", "text/plain");
  response.body = "PASS";
  return response;
}

// --- the four xhr/resources/access-control-basic-* files ---------------------------------------
//
// Four files, four lines each, and they differ only in which `Access-Control-Allow-*` they set. One
// function with a switch would be a fifth behaviour none of them has, so each is spelled out.
HandlerResponse XhrAccessControlHandler(const HandlerRequest& request, std::string_view which) {
  HandlerResponse response;
  response.handled = true;
  AddHeader(response, "Content-Type", "text/plain");
  if (which == "allow") {
    AddHeader(response, "Access-Control-Allow-Credentials", "true");
    AddHeader(response, "Access-Control-Allow-Origin", HeaderValue(request, "Origin"));
    response.body = "PASS: Cross-domain access allowed.";
  } else if (which == "allow-star") {
    AddHeader(response, "Access-Control-Allow-Origin", "*");
    response.body = "PASS: Cross-domain access allowed.";
  } else if (which == "allow-no-credentials") {
    AddHeader(response, "Access-Control-Allow-Origin", HeaderValue(request, "Origin"));
    response.body = "PASS: Cross-domain access allowed.";
  } else {  // denied: no header at all, which is the point of the file
    SetHeader(response, "Cache-Control", "no-store");
    response.body = "FAIL: Cross-domain access allowed.";
  }
  return response;
}

}  // namespace

HandlerResponse RunHandler(const HandlerRequest& request, Stash& stash) {
  const Query query = ParseQuery(request.query);
  const std::string& path = request.path;

  // **A closed list, keyed by repo-relative path.** Three different `redirect.py` files exist in the
  // checkout with three different behaviours; dispatching on a basename would apply one of them to
  // all three, which is exactly the "passes for the wrong reason" ADR 0040 §2 refuses.
  if (path == "common/slow.py") {
    return SlowHandler(query);
  }
  if (path == "common/redirect.py") {
    return CommonRedirectHandler(request, query);
  }
  if (path == "common/echo.py") {
    return EchoHandler(query);
  }
  if (path == "fetch/api/resources/status.py") {
    return StatusHandler(request, query);
  }
  if (path == "fetch/api/resources/method.py") {
    return MethodHandler(request, query);
  }
  if (path == "fetch/api/resources/echo-content.py") {
    return EchoContentHandler(request);
  }
  if (path == "fetch/api/resources/inspect-headers.py") {
    return InspectHeadersHandler(request, query);
  }
  if (path == "fetch/api/resources/preflight.py") {
    return PreflightHandler(request, query, stash);
  }
  if (path == "fetch/api/resources/redirect.py") {
    return FetchRedirectHandler(request, query, stash);
  }
  if (path == "xhr/resources/redirect.py") {
    return XhrRedirectHandler(request, query);
  }
  if (path == "xhr/resources/content.py") {
    return XhrContentHandler(request, query);
  }
  // Byte-for-byte the same file as `fetch/api/resources/status.py`, and dispatched to the same
  // transcription rather than copied -- two copies would be two things to keep in step.
  if (path == "xhr/resources/status.py") {
    return StatusHandler(request, query);
  }
  if (path == "xhr/resources/delay.py") {
    return XhrDelayHandler(query);
  }
  if (path == "xhr/resources/corsenabled.py") {
    return XhrCorsEnabledHandler(request, query);
  }
  if (path == "xhr/resources/inspect-headers.py") {
    return XhrInspectHeadersHandler(request, query);
  }
  if (path == "xhr/resources/headers.py") {
    return XhrHeadersHandler();
  }
  if (path == "xhr/resources/requri.py") {
    return XhrRequestUriHandler(request, query);
  }
  if (path == "xhr/resources/echo-content-type.py") {
    return XhrEchoContentTypeHandler(request);
  }
  if (path == "xhr/resources/echo-content-cors.py") {
    return XhrEchoContentCorsHandler(request, query);
  }
  if (path == "xhr/resources/redirect-cors.py") {
    return XhrRedirectCorsHandler(request, query);
  }
  if (path == "xhr/resources/reset-token.py") {
    return XhrResetTokenHandler(request, query, stash);
  }
  if (path == "xhr/resources/access-control-basic-allow.py") {
    return XhrAccessControlHandler(request, "allow");
  }
  if (path == "xhr/resources/access-control-basic-allow-star.py") {
    return XhrAccessControlHandler(request, "allow-star");
  }
  if (path == "xhr/resources/access-control-basic-allow-no-credentials.py") {
    return XhrAccessControlHandler(request, "allow-no-credentials");
  }
  if (path == "xhr/resources/access-control-basic-denied.py") {
    return XhrAccessControlHandler(request, "denied");
  }
  return HandlerResponse{};
}

void ApplyPipes(std::string_view pipe, HandlerResponse& response) {
  // wptserve's chain syntax: `status(404)|header(X,Y)`. Arguments are comma-separated and may be
  // escaped with a backslash; nothing in this checkout's queries uses the escape, so a plain split
  // is the transcription rather than a simplification.
  for (const std::string& stage : Split(pipe, '|')) {
    const std::size_t open = stage.find('(');
    const std::string name = open == std::string::npos ? stage : stage.substr(0, open);
    std::string arguments;
    if (open != std::string::npos && stage.size() > open + 1 && stage.back() == ')') {
      arguments = stage.substr(open + 1, stage.size() - open - 2);
    }
    const std::vector<std::string> parts = Split(arguments, ',');
    if (name == "status" && !parts.empty()) {
      response.status = ToInt(parts[0], response.status);
      response.status_text = parts.size() > 1 ? parts[1] : std::string("OK");
    } else if (name == "header" && parts.size() >= 2) {
      // A third argument that is truthy means *append* rather than replace, which is how a test asks
      // for two `Set-Cookie`s or two `Access-Control-Allow-Origin`s.
      if (parts.size() >= 3 && !parts[2].empty() && parts[2] != "False" && parts[2] != "false") {
        AddHeader(response, parts[0], parts[1]);
      } else {
        SetHeader(response, parts[0], parts[1]);
      }
    } else if (name == "slice") {
      const std::size_t size = response.body.size();
      const std::size_t start =
          parts.size() > 0 && !parts[0].empty()
              ? std::min(size, static_cast<std::size_t>(std::max(0, ToInt(parts[0], 0))))
              : 0;
      const std::size_t end =
          parts.size() > 1 && !parts[1].empty()
              ? std::min(size, static_cast<std::size_t>(std::max(0, ToInt(parts[1], 0))))
              : size;
      response.body = end > start ? response.body.substr(start, end - start) : std::string();
    } else if (name == "trickle") {
      // `trickle(d1:5:d1)` delivers the body in chunks with delays between them. This server writes
      // a response in one piece, so the *total* delay is applied and the body arrives whole. That is
      // a deviation and it is named here rather than hidden: a test asserting on progressive
      // delivery will fail, which is the correct outcome for a browser with no incremental parse
      // (ADR 0030) anyway.
      int total = 0;
      for (const std::string& part : Split(arguments, ':')) {
        if (!part.empty() && part.front() == 'd') {
          total += ToInt(std::string_view(part).substr(1), 0) * 1000;
        }
      }
      response.delay_ms += total;
    }
    // `sub` is already applied by the server for a `.sub.` file, and anything else is ignored rather
    // than guessed -- an unimplemented pipe must not change the response.
  }
}

}  // namespace microbrowser::wpt
