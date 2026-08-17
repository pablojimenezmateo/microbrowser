#pragma once

#include <string>
#include <vector>

namespace microbrowser::wpt {

// The runner's own certificate authority, generated fresh for every run.
//
// ADR 0040 §2 says the server is ours and is not Python. The one thing upstream
// gets from wptserve that a static file server does not is an **https origin**:
// 1,268 of the 44,144 files in scope have `.https.` in their name and cannot be
// loaded over `http:` at all, and `{{ports[https][0]}}` is how the rest of the
// suite reaches a second, secure origin.
//
// **How the trust is scoped, and why that is the whole design.** A trust anchor
// that leaks out of the test runner into the product is a far worse outcome
// than an unrunnable test, so nothing here is persistent, global, or reachable
// from the browser:
//
//   1. **The key pair is generated in memory, per run.** There is no key file
//      in the repository to be found later and trusted by something else, and
//      no certificate whose validity outlives the process that made it. The CA
//      is valid for a day, from an hour ago.
//   2. **The CA's private key never touches the disk.** The server process is
//      `fork`ed from the runner, so it inherits both PEMs as memory. Only the
//      CA *certificate* -- a public object -- is written to a file, because
//      `net::SocketTransportFactory::Options::ca_bundle_path` takes a path.
//   3. **Trust is installed by the tool, in the tool's own transport.** The
//      runner hands each test process a `SocketTransportFactory` built with
//      that path; `SharedContext` then calls `SSL_CTX_load_verify_locations`
//      *instead of* `SSL_CTX_set_default_verify_paths`, so a test process
//      trusts this one CA and nothing else -- not even a real public root.
//      `src/` gains nothing: no new option, no environment variable, no code
//      path a shipped binary can reach. `microbrowser` and
//      `microbrowser_snapshot` never set `ca_bundle_path` and so still use the
//      system store, exactly as before.
//   4. **The leaf is not a CA.** `basicConstraints` is `CA:FALSE`, so the
//      certificate the server presents cannot sign anything, and its key
//      leaking would authorise nothing beyond the names below.
//
// The names it is valid for are the ones the substitution table can generate:
// `{{domains[X]}}` is `X.localhost` and `{{hosts[alt][X]}}` is `X.alt.localhost`,
// and the labels the checkout actually uses are a short, countable list
// (`www`, `www1`, `www2`, two two-label forms, and two IDN ones). A name not on
// the list fails hostname verification, which is the honest answer -- the
// client's `SSL_set1_host` check is the one thing in the TLS stack this must
// not weaken.
struct GeneratedCertificate {
  // The trust anchor. Public: safe to write to a file, useless without the key.
  std::string ca_certificate_pem;
  // What the server presents. Signed by the CA above, `CA:FALSE`.
  std::string certificate_pem;
  // The leaf's private key. Never written to disk; inherited across `fork`.
  std::string private_key_pem;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Every DNS name the leaf is valid for, given the server's primary host. The
// substitution table's own vocabulary, in one place, so the certificate and the
// origins a test is handed cannot disagree.
std::vector<std::string> CertificateHostNames(const std::string& host);

// Generates the pair. `hosts` becomes the leaf's subjectAltName; a leaf with no
// SAN is one no client will accept, so an empty list is an error rather than a
// certificate for nothing.
GeneratedCertificate GenerateRunnerCertificate(const std::vector<std::string>& hosts);

}  // namespace microbrowser::wpt
