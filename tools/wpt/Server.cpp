#include "wpt/Server.h"

#include "wpt/Handlers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace microbrowser::wpt {
namespace {

// A request line plus headers larger than this is not a browser, it is an
// attempt on the server. Everything this serves is loopback, but a test tool
// that treats its input as trusted teaches the habit that loses the browser.
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxConnections = 256;

std::string ReadWholeFile(const std::filesystem::path& path, bool* found) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (found != nullptr) {
      *found = false;
    }
    return {};
  }
  if (found != nullptr) {
    *found = true;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// **No `charset` on anything, which is wptserve's table and was this server's bug.**
//
// It used to send `text/html; charset=utf-8` for every document. That is one header, and it
// silently disabled the whole of the Encoding Standard for the whole of the suite: a `charset` in
// `Content-Type` outranks a `<meta charset>` in the bytes, so every test that declares an encoding
// in its markup -- which is how the 105 files under `encoding/legacy-mb-*` say what they are -- was
// decoded as UTF-8 and tested nothing it meant to test. `gbk-encoder.html` carries the comment
// "if the server overrides this, it is stupid", which is the bug written down by an author who had
// met it before.
//
// A test that needs a charset asks for one with a `.headers` sidecar, and 25 files under
// `encoding/` do exactly that. Matching wptserve's `content_types` table is what makes those the
// only ones that get it.
std::string_view MimeTypeFor(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  const std::string_view extension = dot == std::string_view::npos ? "" : path.substr(dot);
  static const std::unordered_map<std::string_view, std::string_view> kTypes = {
      {".html", "text/html"},                  {".htm", "text/html"},
      {".xhtml", "application/xhtml+xml"},     {".xht", "application/xhtml+xml"},
      {".xml", "text/xml"},                    {".js", "text/javascript"},
      {".mjs", "text/javascript"},
      {".json", "application/json"},           {".css", "text/css"},
      {".txt", "text/plain"},                  {".png", "image/png"},
      {".jpg", "image/jpeg"},                  {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},                   {".webp", "image/webp"},
      {".svg", "image/svg+xml"},               {".ico", "image/x-icon"},
      {".bmp", "image/bmp"},                   {".woff", "font/woff"},
      {".woff2", "font/woff2"},                {".ttf", "font/ttf"},
      {".otf", "font/otf"},                    {".mp4", "video/mp4"},
      {".m4v", "video/mp4"},                   {".m4a", "audio/mp4"},
      {".webm", "video/webm"},                 {".mp3", "audio/mpeg"},
      {".wav", "audio/wav"},                   {".ogg", "audio/ogg"},
      {".vtt", "text/vtt"},                    {".m3u8", "application/vnd.apple.mpegurl"},
      {".pdf", "application/pdf"},             {".wasm", "application/wasm"},
  };
  const auto found = kTypes.find(extension);
  return found == kTypes.end() ? "application/octet-stream" : found->second;
}

std::string PercentDecode(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '%' && index + 2 < input.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int high = hex(input[index + 1]);
      const int low = hex(input[index + 2]);
      if (high >= 0 && low >= 0) {
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    output.push_back(input[index]);
  }
  return output;
}

// Resolves a URL path to a path relative to the checkout root, or empty when it
// escapes it. `..` is resolved here rather than handed to the filesystem: a
// server that lets a request name `/../../etc/shadow` is the canonical mistake,
// and `std::filesystem::canonical` would answer *after* the fact.
std::string NormalizeUrlPath(std::string_view path) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view segment =
        path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    if (segment == "..") {
      if (segments.empty()) {
        return {};  // escapes the root
      }
      segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  std::string joined;
  for (const std::string_view segment : segments) {
    if (!joined.empty()) {
      joined.push_back('/');
    }
    joined.append(segment);
  }
  return joined;
}

bool SetNonBlocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string EscapeForAttribute(std::string_view value) {
  std::string escaped;
  for (const char c : value) {
    if (c == '&') {
      escaped += "&amp;";
    } else if (c == '"') {
      escaped += "&quot;";
    } else if (c == '<') {
      escaped += "&lt;";
    } else {
      escaped.push_back(c);
    }
  }
  return escaped;
}

}  // namespace

// --- Substitution ------------------------------------------------------------

std::string ApplySubstitutions(std::string_view body, const Substitutions& table) {
  const auto domain = [&](std::string_view name) -> std::string {
    if (name.empty()) {
      return table.host;
    }
    return std::string(name) + "." + table.host;
  };
  // WPT names a second *site* rather than a second origin for the tests that
  // care about registrable domains. Under `.localhost` that is one more label.
  const auto alt_domain = [&](std::string_view name) -> std::string {
    if (name.empty()) {
      return "alt." + table.host;
    }
    return std::string(name) + ".alt." + table.host;
  };
  const auto port_at = [&](std::size_t index) -> std::string {
    if (index < table.http_ports.size()) {
      return std::to_string(table.http_ports[index]);
    }
    return "0";
  };

  std::string output;
  output.reserve(body.size());
  std::size_t position = 0;
  while (position < body.size()) {
    const std::size_t open = body.find("{{", position);
    if (open == std::string_view::npos) {
      output.append(body.substr(position));
      break;
    }
    const std::size_t close = body.find("}}", open);
    if (close == std::string_view::npos) {
      output.append(body.substr(position));
      break;
    }
    output.append(body.substr(position, open - position));
    const std::string_view name = body.substr(open + 2, close - open - 2);
    std::string replacement;
    bool replaced = true;

    const auto bracketed = [&](std::string_view prefix, std::string* first,
                               std::string* second) -> bool {
      if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
        return false;
      }
      std::string_view rest = name.substr(prefix.size());
      if (rest.empty() || rest.front() != '[') {
        return false;
      }
      const std::size_t first_close = rest.find(']');
      if (first_close == std::string_view::npos) {
        return false;
      }
      *first = std::string(rest.substr(1, first_close - 1));
      rest = rest.substr(first_close + 1);
      if (second == nullptr) {
        return rest.empty();
      }
      if (rest.empty() || rest.front() != '[') {
        return false;
      }
      const std::size_t second_close = rest.find(']');
      if (second_close == std::string_view::npos) {
        return false;
      }
      *second = std::string(rest.substr(1, second_close - 1));
      return true;
    };

    std::string first;
    std::string second;
    if (name == "host") {
      replacement = table.host;
    } else if (bracketed("domains", &first, nullptr)) {
      replacement = domain(first);
    } else if (bracketed("hosts", &first, &second)) {
      replacement = first.empty() ? domain(second) : alt_domain(second);
    } else if (bracketed("ports", &first, &second)) {
      std::size_t index = 0;
      for (const char c : second) {
        if (c >= '0' && c <= '9') {
          index = index * 10 + static_cast<std::size_t>(c - '0');
        }
      }
      if (first == "http" || first == "http-private" || first == "http-public") {
        replacement = port_at(index);
      } else {
        // No TLS and no WebSocket server here. A port nothing listens on makes
        // the test fail to connect, which is the honest answer; a port that
        // answered plain HTTP to an `https:` fetch would make it fail somewhere
        // far away from the reason.
        replacement = std::to_string(1 + index);
      }
    } else if (bracketed("location", &first, nullptr)) {
      if (first == "host") {
        replacement = table.host + ":" + std::to_string(table.request_port);
      } else if (first == "hostname") {
        replacement = table.host;
      } else if (first == "port") {
        replacement = std::to_string(table.request_port);
      } else if (first == "scheme") {
        replacement = "http";
      } else if (first == "server") {
        replacement = "http://" + table.host + ":" + std::to_string(table.request_port);
      } else {
        replaced = false;
      }
    } else if (bracketed("GET", &first, nullptr)) {
      replacement.clear();
      std::string_view query = table.query;
      while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == first) {
          replacement = PercentDecode(pair.substr(equals + 1));
          break;
        }
        if (amp == std::string_view::npos) {
          break;
        }
        query = query.substr(amp + 1);
      }
    } else {
      replaced = false;
    }

    if (replaced) {
      output.append(replacement);
    } else {
      // Left verbatim. See the header: a silently-empty origin is a failure
      // nobody can read.
      output.append(body.substr(open, close + 2 - open));
    }
    position = close + 2;
  }
  return output;
}

// --- Generated tests ---------------------------------------------------------

MetaDirectives ParseMeta(std::string_view source) {
  MetaDirectives directives;
  std::size_t position = 0;
  while (position < source.size()) {
    const std::size_t end = source.find('\n', position);
    std::string_view line =
        source.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                             : end - position);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    constexpr std::string_view kPrefix = "// META: ";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) {
      // META lines are a contiguous block at the head of the file. The first
      // line that is not one ends the block -- including a blank line, which is
      // what upstream does.
      break;
    }
    const std::string_view directive = line.substr(kPrefix.size());
    const std::size_t equals = directive.find('=');
    if (equals != std::string_view::npos) {
      const std::string_view key = directive.substr(0, equals);
      const std::string value(directive.substr(equals + 1));
      if (key == "script") {
        directives.scripts.push_back(value);
      } else if (key == "title") {
        directives.title = value;
      } else if (key == "timeout") {
        directives.long_timeout = value == "long";
      } else if (key == "variant") {
        directives.variants.push_back(value);
      } else if (key == "global") {
        std::string_view globals = value;
        while (!globals.empty()) {
          const std::size_t comma = globals.find(',');
          directives.globals.emplace_back(globals.substr(0, comma));
          if (comma == std::string_view::npos) {
            break;
          }
          globals = globals.substr(comma + 1);
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  if (directives.globals.empty()) {
    directives.globals = {"window", "dedicatedworker"};
  }
  return directives;
}

std::string GeneratedTestSource(std::string_view url_path) {
  struct Rule {
    std::string_view suffix;
    std::string_view replacement;
  };
  // Order matters: `.any.worker.html` must be tried before `.any.html` would
  // have a chance to look at it, and `.worker.html` before `.html`.
  static constexpr Rule kRules[] = {
      {".any.worker.html", ".any.js"}, {".any.worker.js", ".any.js"},
      {".any.html", ".any.js"},        {".window.html", ".window.js"},
      {".worker.html", ".worker.js"},
  };
  for (const Rule& rule : kRules) {
    if (url_path.size() > rule.suffix.size() &&
        url_path.compare(url_path.size() - rule.suffix.size(), rule.suffix.size(),
                         rule.suffix) == 0) {
      std::string source(url_path.substr(0, url_path.size() - rule.suffix.size()));
      source.append(rule.replacement);
      return source;
    }
  }
  return {};
}

std::string GenerateGeneratedTest(std::string_view url_path, std::string_view source_relative,
                                  std::string_view source) {
  const MetaDirectives directives = ParseMeta(source);
  std::string meta;
  if (!directives.title.empty()) {
    meta += "<title>" + EscapeForAttribute(directives.title) + "</title>\n";
  }
  if (directives.long_timeout) {
    meta += "<meta name=\"timeout\" content=\"long\">\n";
  }
  std::string scripts;
  for (const std::string& script : directives.scripts) {
    scripts += "<script src=\"" + EscapeForAttribute(script) + "\"></script>\n";
  }
  // `source_relative` is root-relative; a generated document loads it by the
  // same absolute path so a nested test directory needs no base URL.
  const std::string script_url = "/" + std::string(source_relative);

  const auto ends_with = [&](std::string_view suffix) {
    return url_path.size() > suffix.size() &&
           url_path.compare(url_path.size() - suffix.size(), suffix.size(), suffix) == 0;
  };

  if (ends_with(".any.worker.js")) {
    std::string worker_scripts;
    for (const std::string& script : directives.scripts) {
      worker_scripts += "importScripts(\"" + script + "\");\n";
    }
    return "self.GLOBAL = {\n"
           "  isWindow: function() { return false; },\n"
           "  isWorker: function() { return true; },\n"
           "  isShadowRealm: function() { return false; },\n"
           "};\n"
           "importScripts(\"/resources/testharness.js\");\n" +
           worker_scripts + "importScripts(\"" + script_url + "\");\ndone();\n";
  }
  if (ends_with(".any.worker.html") || ends_with(".worker.html")) {
    const std::string worker_url =
        ends_with(".any.worker.html")
            // `foo.any.worker.html` is served by `foo.any.worker.js`, which is
            // itself generated. Dropping `.html` without putting `.js` back
            // asked for a name nothing serves, so every one of the suite's
            // 1,763 worker tests 404ed its worker and then timed out waiting
            // for results that could never arrive.
            ? std::string(url_path.substr(0, url_path.size() - 5)) + ".js"
            : "/" + std::string(source_relative);
    return "<!doctype html>\n<meta charset=utf-8>\n" + meta +
           "<script src=\"/resources/testharness.js\"></script>\n"
           "<script src=\"/resources/testharnessreport.js\"></script>\n"
           "<div id=log></div>\n"
           "<script>\nfetch_tests_from_worker(new Worker(\"" +
           (worker_url.front() == '/' ? worker_url : "/" + worker_url) + "\"));\n</script>\n";
  }
  if (ends_with(".any.html")) {
    return "<!doctype html>\n<meta charset=utf-8>\n" + meta +
           "<script>\n"
           "self.GLOBAL = {\n"
           "  isWindow: function() { return true; },\n"
           "  isWorker: function() { return false; },\n"
           "  isShadowRealm: function() { return false; },\n"
           "};\n"
           "</script>\n"
           "<script src=\"/resources/testharness.js\"></script>\n"
           "<script src=\"/resources/testharnessreport.js\"></script>\n" +
           scripts + "<div id=log></div>\n<script src=\"" + script_url + "\"></script>\n";
  }
  if (ends_with(".window.html")) {
    return "<!doctype html>\n<meta charset=utf-8>\n" + meta +
           "<script src=\"/resources/testharness.js\"></script>\n"
           "<script src=\"/resources/testharnessreport.js\"></script>\n" +
           scripts + "<div id=log></div>\n<script src=\"" + script_url + "\"></script>\n";
  }
  return {};
}

// --- The server --------------------------------------------------------------

struct Server::Connection {
  int descriptor = -1;
  std::uint16_t port = 0;
  std::string input;
  std::string output;
  std::size_t written = 0;
  bool closing = false;
  // A response held back until its delay elapses. `common/slow.py` is 208 files of the suite and
  // upstream implements it with `time.sleep`; a sleep here would stall every other test in the run,
  // so the bytes wait in the connection and the poll loop's timeout accounts for them.
  std::string delayed;
  std::chrono::steady_clock::time_point deliver_at{};
  bool has_delayed = false;
};

Server::Server(ServerOptions options) : options_(std::move(options)) {}

Server::~Server() {
  if (!unhandled_handlers_.empty()) {
    // Rank unhandled .py handlers by request count so the next handler to transcribe is
    // the one the suite actually asks for the most. This is the demand report that was
    // dropped in the 2026-08-15 merge and is what stops handler prioritization from being
    // guesswork (grep for a handler name counts references in comments, not requests).
    std::vector<std::pair<std::string, int>> ranked(unhandled_handlers_.begin(),
                                                     unhandled_handlers_.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "\n--- unhandled .py handler demand (requests) ---\n");
    for (const auto& [path, count] : ranked) {
      std::fprintf(stderr, "  %5d  %s\n", count, path.c_str());
    }
    std::fprintf(stderr, "--- %zu distinct handlers, %d total requests ---\n\n",
                 ranked.size(),
                 [&] {
                   int total = 0;
                   for (const auto& [_, c] : ranked) total += c;
                   return total;
                 }());
  }

  for (const auto& connection : connections_) {
    if (connection->descriptor >= 0) {
      ::close(connection->descriptor);
    }
  }
  for (const int listener : listeners_) {
    ::close(listener);
  }
}

bool Server::Bind() {
  bound_ports_.clear();
  for (std::uint16_t requested : options_.ports) {
    std::uint16_t chosen = requested;
    // IPv4 first: it is the one that can be asked to choose a free port and
    // then told to the IPv6 socket, so the pair always agrees.
    for (int family : {AF_INET, AF_INET6}) {
      const int listener = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener < 0) {
        error_ = std::string("socket: ") + strerror(errno);
        return false;
      }
      const int one = 1;
      ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      if (family == AF_INET6) {
        ::setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
      }
      int result = 0;
      if (family == AF_INET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(chosen);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        result = ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      } else {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(chosen);
        address.sin6_addr = in6addr_loopback;
        result = ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      }
      if (result != 0 || ::listen(listener, 64) != 0) {
        error_ = std::string("bind/listen on port ") + std::to_string(chosen) + ": " +
                 strerror(errno);
        ::close(listener);
        return false;
      }
      if (chosen == 0) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
          error_ = std::string("getsockname: ") + strerror(errno);
          ::close(listener);
          return false;
        }
        chosen = ntohs(address.sin_port);
      }
      SetNonBlocking(listener);
      listeners_.push_back(listener);
      listener_ports_.push_back(chosen);
    }
    bound_ports_.push_back(chosen);
  }
  return true;
}

std::string Server::Origin(std::size_t port_index) const {
  if (port_index >= bound_ports_.size()) {
    return {};
  }
  return "http://" + options_.host + ":" + std::to_string(bound_ports_[port_index]);
}

bool Server::Accept(int listener, std::uint16_t port) {
  const int descriptor = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (descriptor < 0) {
    return false;
  }
  if (connections_.size() >= kMaxConnections) {
    ::close(descriptor);
    return true;
  }
  const int one = 1;
  ::setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  auto connection = std::make_unique<Connection>();
  connection->descriptor = descriptor;
  connection->port = port;
  connections_.push_back(std::move(connection));
  return true;
}

bool Server::ReadFrom(Connection& connection) {
  char buffer[16384];
  while (true) {
    const ssize_t count = ::recv(connection.descriptor, buffer, sizeof(buffer), 0);
    if (count > 0) {
      if (connection.input.size() + static_cast<std::size_t>(count) > kMaxRequestBytes) {
        return false;
      }
      connection.input.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      return false;  // peer closed
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return false;
  }
  // One request per pass: a pipelined second request stays in the buffer and is
  // answered on the next turn.
  while (true) {
    const std::size_t end = connection.input.find("\r\n\r\n");
    if (end == std::string::npos) {
      break;
    }
    const std::string request = connection.input.substr(0, end);
    // **The body, which this server used to discard.** A `POST` to a handler is most of what
    // `fetch/` and `xhr/` do, and a server that answered before reading the body left the bytes in
    // the buffer to be parsed as the next request line. Waiting for the whole of it is what makes a
    // handler able to echo it back.
    std::size_t content_length = 0;
    {
      std::size_t position = 0;
      while (position < request.size()) {
        const std::size_t line_end = request.find("\r\n", position);
        const std::string_view line(request.data() + position,
                                    (line_end == std::string::npos ? request.size() : line_end) -
                                        position);
        if (line.size() > 15 && strncasecmp(line.data(), "Content-Length:", 15) == 0) {
          content_length = static_cast<std::size_t>(std::strtoul(
              std::string(line.substr(15)).c_str(), nullptr, 10));
        }
        if (line_end == std::string::npos) {
          break;
        }
        position = line_end + 2;
      }
    }
    if (connection.input.size() < end + 4 + content_length) {
      break;  // the body has not all arrived yet
    }
    const std::string body = connection.input.substr(end + 4, content_length);
    connection.input.erase(0, end + 4 + content_length);
    Respond(connection, request, body);
    if (connection.closing) {
      break;
    }
  }
  return true;
}

bool Server::WriteTo(Connection& connection) {
  while (connection.written < connection.output.size()) {
    const ssize_t count = ::send(connection.descriptor, connection.output.data() + connection.written,
                                 connection.output.size() - connection.written, MSG_NOSIGNAL);
    if (count > 0) {
      connection.written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }
    return false;
  }
  connection.output.clear();
  connection.written = 0;
  return !connection.closing;
}

void Server::Respond(Connection& connection, std::string_view request,
                     std::string_view request_body) {
  const std::size_t line_end = request.find("\r\n");
  const std::string_view request_line = request.substr(0, line_end);
  std::size_t first_space = request_line.find(' ');
  std::size_t second_space = first_space == std::string_view::npos
                                 ? std::string_view::npos
                                 : request_line.find(' ', first_space + 1);
  const std::string_view method =
      first_space == std::string_view::npos ? "" : request_line.substr(0, first_space);
  const std::string_view target =
      second_space == std::string_view::npos
          ? ""
          : request_line.substr(first_space + 1, second_space - first_space - 1);

  int status = 200;
  std::string status_text = "OK";
  std::string body;
  std::string content_type;
  std::vector<std::string> extra_headers;

  const std::size_t question = target.find('?');
  const std::string_view raw_path = target.substr(0, question);
  const std::string query =
      question == std::string_view::npos ? "" : std::string(target.substr(question + 1));
  const std::string decoded = PercentDecode(raw_path);
  std::string relative = NormalizeUrlPath(decoded);

  const auto fail = [&](int code, std::string_view text) {
    status = code;
    status_text = text;
    body = std::string(text);
    content_type = "text/plain; charset=utf-8";
  };

  // Every request header, in order, because a handler reads them and because the pipe stage that
  // echoes one needs the value rather than the presence.
  std::vector<std::pair<std::string, std::string>> request_headers;
  {
    std::size_t position = line_end == std::string_view::npos ? request.size() : line_end + 2;
    while (position < request.size()) {
      const std::size_t header_end = request.find("\r\n", position);
      const std::string_view line = request.substr(
          position, (header_end == std::string_view::npos ? request.size() : header_end) - position);
      const std::size_t colon = line.find(':');
      if (colon != std::string_view::npos) {
        std::string value(line.substr(colon + 1));
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
          value.erase(0, 1);
        }
        request_headers.emplace_back(std::string(line.substr(0, colon)), std::move(value));
      }
      if (header_end == std::string_view::npos) {
        break;
      }
      position = header_end + 2;
    }
  }

  int delay_ms = 0;
  const bool is_python = relative.size() > 3 && relative.compare(relative.size() - 3, 3, ".py") == 0;

  if (relative.empty() || decoded.find('\0') != std::string::npos) {
    fail(404, "Not Found");
  } else if (is_python) {
    HandlerRequest handler_request;
    handler_request.method = std::string(method);
    handler_request.path = relative;
    handler_request.query = query;
    handler_request.body = std::string(request_body);
    handler_request.headers = request_headers;
    handler_request.origin =
        "http://" + options_.host + ":" + std::to_string(connection.port);
    HandlerResponse answer = RunHandler(handler_request, stash_);
    if (!answer.handled) {
      // Still the ADR's answer for everything not on the closed list: a test that needs an
      // unimplemented handler fails visibly rather than getting a plausible 200.
      ++unhandled_handlers_[relative];
      fail(501, "Python handlers are not implemented");
    } else {
      status = answer.status;
      status_text = answer.status_text;
      body = std::move(answer.body);
      delay_ms = answer.delay_ms;
      content_type.clear();
      for (const auto& [name, value] : answer.headers) {
        if (strncasecmp(name.c_str(), "Content-Type", 13) == 0) {
          content_type = value;
        } else {
          extra_headers.push_back(name + ": " + value);
        }
      }
      if (content_type.empty()) {
        content_type = "text/plain";
      }
    }
  } else if (method != "GET" && method != "HEAD") {
    // A method a static file cannot answer. 501 rather than 404 keeps the two apart in a log.
    fail(501, "Not Implemented");
  } else {
    bool found = false;

    // 0. The one rewrite `wpt serve` performs. Every `idlharness` test asks for
    // `/resources/WebIDLParser.js`, which has not been a file in the checkout
    // for years: upstream's server maps it onto the vendored parser
    // (tools/serve/serve.py, `rewrites`). Without it the whole IDL half of the
    // suite 404s its parser and reports one failed `idl_test setup`.
    if (relative == "resources/WebIDLParser.js") {
      relative = "resources/webidl2/lib/webidl2.js";
    }
    std::string served_name = relative;

    // 1. A harness override, which is how our testharnessreport.js wins.
    if (!options_.harness_overrides_dir.empty()) {
      const std::filesystem::path override_path =
          std::filesystem::path(options_.harness_overrides_dir) /
          std::filesystem::path(relative).filename();
      if (relative.rfind("resources/", 0) == 0) {
        body = ReadWholeFile(override_path, &found);
        if (found && options_.timeout_multiplier > 1) {
          body = "self.__wpt_timeout_multiplier = " +
                 std::to_string(options_.timeout_multiplier) + ";\n" + body;
        }
      }
    }
    // 2. The file itself.
    if (!found) {
      body = ReadWholeFile(std::filesystem::path(options_.wpt_root) / relative, &found);
    }
    // 3. A generated test, synthesised from its `.any.js`/`.window.js` source.
    if (!found) {
      const std::string source_relative = GeneratedTestSource(relative);
      if (!source_relative.empty()) {
        bool source_found = false;
        const std::string source =
            ReadWholeFile(std::filesystem::path(options_.wpt_root) / source_relative, &source_found);
        if (source_found) {
          body = GenerateGeneratedTest(relative, source_relative, source);
          found = !body.empty();
          served_name = relative;
        }
      }
    }

    if (!found) {
      fail(404, "Not Found");
    } else {
      content_type = std::string(MimeTypeFor(served_name));
      // A generated document has no extension of its own worth trusting -- the
      // `.js` in `.any.worker.js` is real, the `.html` in `.any.html` is not a
      // file. MimeTypeFor already answers correctly for both.

      // 4. A `.headers` sidecar. This is how a test asks for a Content-Type, a
      // CORS header or a CSP without a Python handler, and fetch/ leans on it.
      bool headers_found = false;
      const std::string sidecar =
          ReadWholeFile(std::filesystem::path(options_.wpt_root) / (relative + ".headers"),
                        &headers_found);
      if (headers_found) {
        std::size_t position = 0;
        while (position < sidecar.size()) {
          const std::size_t end = sidecar.find('\n', position);
          std::string header = sidecar.substr(
              position, end == std::string::npos ? std::string::npos : end - position);
          while (!header.empty() && (header.back() == '\r' || header.back() == ' ')) {
            header.pop_back();
          }
          if (!header.empty()) {
            const std::size_t colon = header.find(':');
            if (colon != std::string::npos &&
                strncasecmp(header.c_str(), "Content-Type", 12) == 0 && colon == 12) {
              content_type = header.substr(colon + 1);
              while (!content_type.empty() && content_type.front() == ' ') {
                content_type.erase(0, 1);
              }
            } else {
              extra_headers.push_back(header);
            }
          }
          if (end == std::string::npos) {
            break;
          }
          position = end + 1;
        }
      }

      // 5. Substitution, for a `.sub.` file only.
      if (relative.find(".sub.") != std::string::npos) {
        Substitutions table;
        table.host = options_.host;
        table.http_ports = bound_ports_;
        table.request_port = connection.port;
        table.query = query;
        body = ApplySubstitutions(body, table);
      }
    }
  }

  // `?pipe=` -- wptserve's chain, applied to whatever the response now is. Hundreds of tests ask for
  // a status or a header on an *ordinary static file* this way, and a server that ignored it served
  // the file as itself: a wrong answer rather than a missing one, and therefore the worse of the two.
  {
    const std::vector<std::pair<std::string, std::string>> parsed = ParseQuery(query);
    if (QueryHas(parsed, "pipe")) {
      HandlerResponse piped;
      piped.status = status;
      piped.status_text = status_text;
      piped.body = std::move(body);
      if (!content_type.empty()) {
        piped.headers.emplace_back("Content-Type", content_type);
      }
      for (const std::string& header : extra_headers) {
        const std::size_t colon = header.find(':');
        if (colon != std::string::npos) {
          std::string value = header.substr(colon + 1);
          while (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
          }
          piped.headers.emplace_back(header.substr(0, colon), std::move(value));
        }
      }
      ApplyPipes(QueryFirst(parsed, "pipe"), piped);
      status = piped.status;
      status_text = piped.status_text;
      body = std::move(piped.body);
      delay_ms += piped.delay_ms;
      content_type.clear();
      extra_headers.clear();
      for (const auto& [name, value] : piped.headers) {
        if (content_type.empty() && strncasecmp(name.c_str(), "Content-Type", 13) == 0) {
          content_type = value;
        } else {
          extra_headers.push_back(name + ": " + value);
        }
      }
    }
  }

  std::string response = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  if (!content_type.empty()) {
    response += "Content-Type: " + content_type + "\r\n";
  }
  for (const std::string& header : extra_headers) {
    response += header + "\r\n";
  }
  // Nothing here may be cached: a test run that reused a response would be
  // measuring the run before it.
  response += "Cache-Control: no-store\r\n";
  response += "Connection: keep-alive\r\n\r\n";
  if (method != "HEAD") {
    response += body;
  }
  if (delay_ms > 0) {
    // Held, not slept on. One delayed response per connection is enough: a test that pipelines two
    // slow requests down one socket does not exist, and the second would simply wait its turn.
    connection.delayed += response;
    connection.deliver_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    connection.has_delayed = true;
  } else {
    connection.output += response;
  }

  if (options_.verbose) {
    std::fprintf(stderr, "[wptserve] %d %.*s\n", status, static_cast<int>(target.size()),
                 target.data());
  }
}

int Server::ReleaseDelayedResponses() {
  int soonest = -1;
  const auto now = std::chrono::steady_clock::now();
  for (const auto& connection : connections_) {
    if (!connection->has_delayed) {
      continue;
    }
    if (connection->deliver_at <= now) {
      connection->output += connection->delayed;
      connection->delayed.clear();
      connection->has_delayed = false;
      continue;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               connection->deliver_at - now)
                               .count();
    const int milliseconds = static_cast<int>(remaining <= 0 ? 1 : remaining);
    soonest = soonest < 0 ? milliseconds : std::min(soonest, milliseconds);
  }
  return soonest;
}

void Server::Serve(int stop_after_idle_ms) {
  // A client that vanishes mid-response must not take the server with it.
  ::signal(SIGPIPE, SIG_IGN);
  auto last_activity = std::chrono::steady_clock::now();
  std::vector<pollfd> descriptors;
  while (true) {
    // Before the descriptor list, so that a response whose delay has just elapsed is in `output` and
    // its connection is polled for POLLOUT on this turn rather than the next.
    const int next_delayed = ReleaseDelayedResponses();
    descriptors.clear();
    for (const int listener : listeners_) {
      descriptors.push_back(pollfd{listener, POLLIN, 0});
    }
    for (const auto& connection : connections_) {
      short events = POLLIN;
      if (connection->written < connection->output.size()) {
        events |= POLLOUT;
      }
      descriptors.push_back(pollfd{connection->descriptor, events, 0});
    }
    int timeout_ms = stop_after_idle_ms < 0 ? 1000 : std::min(stop_after_idle_ms, 1000);
    if (next_delayed >= 0) {
      timeout_ms = std::min(timeout_ms, next_delayed);
    }
    const int ready = ::poll(descriptors.data(), descriptors.size(), timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready > 0) {
      last_activity = std::chrono::steady_clock::now();
    } else if (next_delayed >= 0) {
      // A held response is activity: a run that gave up here would kill the very test that asked
      // for the delay.
      last_activity = std::chrono::steady_clock::now();
    } else if (stop_after_idle_ms >= 0 && connections_.empty()) {
      const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - last_activity)
                            .count();
      if (idle >= stop_after_idle_ms) {
        break;
      }
    }

    // How many connections `descriptors` describes. It has to be taken *now*,
    // because `Accept` below appends to `connections_` and a connection that
    // arrived this instant has no entry in an array built before it existed.
    //
    // Indexing past that entry is a heap-buffer-overflow, and AddressSanitizer
    // caught it as one: a `READ of size 2` -- a `revents` field -- six bytes
    // past the end. The consequence was worse than the read. Every freshly
    // accepted connection was serviced against whatever those two bytes held,
    // so a request could be read from, written to, or *closed* on garbage, at
    // random, in the server that is this project's primary correctness signal.
    // A new connection waits for the next poll instead, which costs it one
    // timeout-bounded turn and nothing else.
    std::size_t polled_connections = descriptors.size() - listeners_.size();
    for (std::size_t index = 0; index < listeners_.size(); ++index) {
      if ((descriptors[index].revents & POLLIN) != 0) {
        while (Accept(listeners_[index], listener_ports_[index])) {
        }
      }
    }
    for (std::size_t index = 0; index < polled_connections;) {
      Connection& connection = *connections_[index];
      const pollfd& state = descriptors[listeners_.size() + index];
      bool alive = true;
      if ((state.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        alive = ReadFrom(connection);
      }
      if (alive && connection.written < connection.output.size()) {
        alive = WriteTo(connection);
      }
      if (!alive && connection.written >= connection.output.size()) {
        ::close(connection.descriptor);
        connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
        // The erase shifts every later connection down, and their descriptor
        // entries with them, so the bound moves too.
        --polled_connections;
        continue;
      }
      ++index;
    }
  }
}

}  // namespace microbrowser::wpt
