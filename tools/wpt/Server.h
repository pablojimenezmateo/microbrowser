#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::wpt {

// The half of `wpt serve` this browser actually needs.
//
// Upstream's server is Python: wptserve plus a pile of per-test `.py` handlers.
// Depending on it would put a Python runtime and its packages between an agent
// and a test run, and would make "the tests do not run" a question about pip.
// This is a single-threaded, non-blocking static file server that implements
// the three things a WPT checkout cannot be read without:
//
//   1. `.sub.` template substitution -- `{{host}}`, `{{ports[http][0]}}`,
//      `{{domains[www]}}` and the rest. `/common/get-host-info.sub.js` is one
//      of these, and almost every cross-origin test reads its origins from it.
//   2. Generated tests -- `foo.any.js` is not a test, it is the *source* of
//      `foo.any.html` and `foo.any.worker.html`, which the server synthesises.
//      A large fraction of dom/, fetch/ and encoding/ exists only in this form.
//   3. `.headers` sidecar files, which is how a test asks for a `Content-Type`,
//      a CSP, or a CORS header without a handler.
//
// What it deliberately does not implement is the `.py` handlers. A handler is
// arbitrary Python; approximating one would make a test pass for the wrong
// reason, which is worse than a failure. A request for a `.py` path answers
// 501 and the test that made it fails visibly.
//
// **Origins without root.** WPT's own hostnames (`web-platform.test`) need an
// /etc/hosts entry. `*.localhost` does not: glibc resolves every label under it
// to loopback, and `url::Host::IsLoopbackOrLocalhost` already treats the whole
// suffix as local -- so `www1.localhost:8001` is a real, resolvable, genuinely
// cross-origin origin that costs no privilege and changes nothing on the
// machine. That is the whole reason the substitution table below reads the way
// it does.

// One listening endpoint. Each is a distinct origin as far as a page is
// concerned, which is what cross-origin tests need.
struct ServerPort {
  std::uint16_t port = 0;
};

struct ServerOptions {
  // Root of the web-platform-tests checkout.
  std::string wpt_root;
  // Files served in place of the checkout's own, by URL path. This is how our
  // `testharnessreport.js` replaces upstream's -- upstream's reports to a
  // `wptrunner` over WebDriver, which this browser has no way to speak.
  std::string harness_overrides_dir;
  // Ports to listen on, in the order `{{ports[http][N]}}` indexes them. Zero
  // means "pick a free one", and `Bind` writes the chosen number back.
  std::vector<std::uint16_t> ports{0, 0};
  // Primary host name. Alternates are derived from it (`www.` and friends), so
  // this must be a name whose subdomains resolve -- `localhost` is the one that
  // does with no configuration.
  std::string host = "localhost";
  // Prints one line per request. A firehose on a real test; useful on one.
  bool verbose = false;
};

// A bound, running server.
//
// `Bind` opens the sockets and returns before serving, so a caller can learn
// the port numbers, hand them to a child process, and only then `Serve`. That
// ordering is why there is no thread here: the runner forks this loop into its
// own process *before* it creates anything else, and every process in the test
// run stays single-threaded.
class Server {
 public:
  explicit Server(ServerOptions options);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Opens one IPv4 and one IPv6 loopback socket per requested port. Returns
  // false and leaves `Error()` set on failure.
  bool Bind();

  // The ports actually bound, in the order `ports` requested them.
  const std::vector<std::uint16_t>& Ports() const { return bound_ports_; }

  // Serves until `stop_after_idle_ms` passes with no connection at all, or
  // forever when it is negative. Returns when it stops.
  void Serve(int stop_after_idle_ms = -1);

  const std::string& Error() const { return error_; }

  // The origin a test URL is built against: "http://<host>:<port>".
  std::string Origin(std::size_t port_index = 0) const;

 private:
  struct Connection;

  bool Accept(int listener, std::uint16_t port);
  // Returns false when the connection should be closed.
  bool ReadFrom(Connection& connection);
  bool WriteTo(Connection& connection);
  void Respond(Connection& connection, std::string_view request);

  ServerOptions options_;
  std::vector<int> listeners_;
  std::vector<std::uint16_t> listener_ports_;
  std::vector<std::uint16_t> bound_ports_;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::string error_;
};

// The substitution table, exposed because the runner needs the same origins the
// server hands to pages.
struct Substitutions {
  std::string host;
  std::vector<std::uint16_t> http_ports;
  // The port a request arrived on, which `{{location[port]}}` must answer with.
  std::uint16_t request_port = 0;
  std::string query;
};

// Applies `{{...}}` substitution to `body`. Unknown substitutions are left in
// place on purpose: a test that silently got an empty string where it wanted an
// origin fails in a way nobody can read.
std::string ApplySubstitutions(std::string_view body, const Substitutions& table);

// The synthesised source of a generated test, or empty when `url_path` does not
// name one. `source_relative` is the `.any.js`/`.window.js`/`.worker.js` file
// it was generated from, which the caller must have read into `source`.
std::string GenerateGeneratedTest(std::string_view url_path, std::string_view source_relative,
                                  std::string_view source);

// The `.any.js`/`.window.js` file a generated URL comes from, or empty when the
// URL is not a generated one. Pure string work: no filesystem access.
std::string GeneratedTestSource(std::string_view url_path);

// `// META: key=value` lines from the head of a generated test's source.
struct MetaDirectives {
  std::vector<std::string> scripts;
  std::vector<std::string> globals;
  std::vector<std::string> variants;
  std::string title;
  bool long_timeout = false;
};

MetaDirectives ParseMeta(std::string_view source);

}  // namespace microbrowser::wpt
