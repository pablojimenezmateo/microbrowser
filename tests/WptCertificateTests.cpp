#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string>
#include <vector>

#include "TestSupport.h"
#include "wpt/Certificate.h"

// The runner's certificate authority, and the four properties that make it safe
// to have at all.
//
// A certificate is the one thing in this tool that fails *silently and slowly*:
// a wrong SAN, a missing `serverAuth`, or a leaf that is accidentally a CA
// shows up as a twenty-second TLS timeout in each of 1,268 `.https.` tests and
// nowhere else, so finding it by running the suite costs the better part of a
// day. Compiling `tools/wpt/Certificate.cpp` into the test binary makes it
// milliseconds, which is the same argument that put `Handlers.cpp` here.
//
// The properties asserted, in the order they matter:
//
//   1. The leaf verifies **against this CA** -- which is what a test process
//      does, having been given the CA and nothing else.
//   2. The leaf is **not** a CA. A leaf that could sign would turn a leaked
//      test key into an authority over every name, rather than over the eight
//      loopback labels below.
//   3. The names are the substitution table's own. `SSL_set1_host` in
//      `src/net` is not weakened for the test run, so a name the certificate
//      does not carry fails verification -- correctly, and invisibly if nobody
//      checks the list.
//   4. Nothing outside this run verifies against it: the CA is fresh per call
//      and short-lived, which is asserted here as "two calls do not produce the
//      same authority".

namespace microbrowser::tests {

namespace {

using wpt::GeneratedCertificate;

X509* ParsePem(const std::string& pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return nullptr;
  }
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return certificate;
}

// The real check: build a store holding only the CA and ask OpenSSL, the same
// way `SSL_CTX_load_verify_locations` plus `SSL_VERIFY_PEER` will.
int VerifyLeafAgainstCa(X509* leaf, X509* ca) {
  X509_STORE* store = X509_STORE_new();
  X509_STORE_add_cert(store, ca);
  X509_STORE_CTX* context = X509_STORE_CTX_new();
  X509_STORE_CTX_init(context, store, leaf, nullptr);
  X509_STORE_CTX_set_purpose(context, X509_PURPOSE_SSL_SERVER);
  const int ok = X509_verify_cert(context);
  const int error = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(context);
  X509_STORE_CTX_free(context);
  X509_STORE_free(store);
  return error;
}

bool MatchesHost(X509* leaf, const char* host) {
  return X509_check_host(leaf, host, 0, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
}

}  // namespace

void RegisterWptCertificateTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptCertificate/LeafVerifiesAgainstTheRunnersOwnCa", [] {
    const GeneratedCertificate generated =
        wpt::GenerateRunnerCertificate(wpt::CertificateHostNames("localhost"));
    Expect(generated.ok(), "the certificate generated: " + generated.error);
    Expect(!generated.private_key_pem.empty(), "and it has a private key");

    X509* ca = ParsePem(generated.ca_certificate_pem);
    X509* leaf = ParsePem(generated.certificate_pem);
    Expect(ca != nullptr && leaf != nullptr, "both PEMs parse");

    const int error = VerifyLeafAgainstCa(leaf, ca);
    Expect(error == X509_V_OK,
           std::string("the leaf verifies as an SSL server against the CA alone, which is "
                       "exactly what a test process is given: ") +
               X509_verify_cert_error_string(error));

    X509_free(leaf);
    X509_free(ca);
  });

  AddTest(tests, "WptCertificate/TheLeafCannotSignAnything", [] {
    const GeneratedCertificate generated =
        wpt::GenerateRunnerCertificate(wpt::CertificateHostNames("localhost"));
    Expect(generated.ok(), "the certificate generated");
    X509* leaf = ParsePem(generated.certificate_pem);
    X509* ca = ParsePem(generated.ca_certificate_pem);
    Expect(leaf != nullptr && ca != nullptr, "both PEMs parse");

    // `X509_check_ca` answers 0 for a certificate that is not one. This is the
    // difference between a key that authenticates eight loopback names and a
    // key that authenticates the web.
    ExpectEqInt(X509_check_ca(leaf), 0, "the leaf is not a certificate authority");
    Expect(X509_check_ca(ca) != 0, "and the CA is");
    Expect((X509_get_extension_flags(leaf) & EXFLAG_SS) == 0,
           "the leaf is not self-signed either -- it is signed by the run's CA");

    X509_free(leaf);
    X509_free(ca);
  });

  AddTest(tests, "WptCertificate/TheNamesAreTheSubstitutionTablesOwn", [] {
    const std::vector<std::string> names = wpt::CertificateHostNames("localhost");
    const GeneratedCertificate generated = wpt::GenerateRunnerCertificate(names);
    Expect(generated.ok(), "the certificate generated");
    X509* leaf = ParsePem(generated.certificate_pem);
    Expect(leaf != nullptr, "the leaf parses");

    // Every label the checkout actually uses, counted rather than guessed. A
    // name missing here is a `.https.` test that fails hostname verification
    // and reports a TLS error nobody would trace back to a SAN list.
    for (const char* host : {"localhost", "www.localhost", "www1.localhost", "www2.localhost",
                             "www1.www1.localhost", "www2.www1.localhost", "www2.www.localhost",
                             "xn--lve-6lad.localhost", "xn--n8j6ds53lwwkrqhv28a.localhost",
                             "alt.localhost", "www.alt.localhost", "www1.alt.localhost",
                             "www2.alt.localhost"}) {
      Expect(MatchesHost(leaf, host),
             std::string("the certificate is valid for ") + host);
    }
    // `nonexistent.localhost` exists so that a test can fail to reach a host.
    // A certificate for it would be a certificate for a name nothing is allowed
    // to answer on, and the failure it produces would be the wrong one.
    Expect(!MatchesHost(leaf, "nonexistent.localhost"),
           "and deliberately not for the name that is supposed to fail");
    Expect(!MatchesHost(leaf, "example.com"),
           "and not for anything off the loopback suffix");

    X509_free(leaf);
  });

  AddTest(tests, "WptCertificate/EveryRunIsItsOwnAuthority", [] {
    const GeneratedCertificate first =
        wpt::GenerateRunnerCertificate(wpt::CertificateHostNames("localhost"));
    const GeneratedCertificate second =
        wpt::GenerateRunnerCertificate(wpt::CertificateHostNames("localhost"));
    Expect(first.ok() && second.ok(), "both generated");
    Expect(first.ca_certificate_pem != second.ca_certificate_pem,
           "two runs are two authorities: nothing this one trusts outlives it");

    X509* first_ca = ParsePem(first.ca_certificate_pem);
    X509* second_leaf = ParsePem(second.certificate_pem);
    Expect(first_ca != nullptr && second_leaf != nullptr, "both parse");
    Expect(VerifyLeafAgainstCa(second_leaf, first_ca) != X509_V_OK,
           "and one run's CA does not vouch for another run's server");
    X509_free(first_ca);
    X509_free(second_leaf);
  });

  AddTest(tests, "WptCertificate/ACertificateForNothingIsAnError", [] {
    const GeneratedCertificate generated = wpt::GenerateRunnerCertificate({});
    Expect(!generated.ok(),
           "a leaf with no subjectAltName is one no client accepts, so an empty host list is "
           "refused rather than silently producing one");
  });
}

}  // namespace microbrowser::tests
