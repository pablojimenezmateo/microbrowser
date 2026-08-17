#include "wpt/Certificate.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdint>
#include <memory>

namespace microbrowser::wpt {
namespace {

// OpenSSL objects are C, so every one of them gets an owner here rather than a
// `goto out:` ladder. The generator has eleven allocations and six failure
// points; the ladder was written first and was wrong twice.
struct EvpKeyDeleter {
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct X509Deleter {
  void operator()(X509* certificate) const { X509_free(certificate); }
};
struct BioDeleter {
  void operator()(BIO* bio) const { BIO_free(bio); }
};
using KeyPtr = std::unique_ptr<EVP_PKEY, EvpKeyDeleter>;
using CertificatePtr = std::unique_ptr<X509, X509Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// The last thing OpenSSL complained about, so a failure says which step failed
// rather than "could not generate a certificate".
std::string LastOpenSslError() {
  const unsigned long code = ERR_get_error();
  if (code == 0) {
    return "no OpenSSL error was recorded";
  }
  char buffer[256];
  ERR_error_string_n(code, buffer, sizeof(buffer));
  return buffer;
}

bool AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  if (extension == nullptr) {
    return false;
  }
  const int added = X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
  return added == 1;
}

bool SetName(X509_NAME* name, const char* common_name) {
  return X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>(common_name), -1, -1,
                                    0) == 1;
}

bool SetRandomSerial(X509* certificate) {
  // 64 random bits. A fixed serial would make two runs' certificates
  // indistinguishable to anything that caches by (issuer, serial).
  unsigned char bytes[8];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    return false;
  }
  bytes[0] = static_cast<unsigned char>(bytes[0] & 0x7F);  // positive
  BIGNUM* number = BN_bin2bn(bytes, static_cast<int>(sizeof(bytes)), nullptr);
  if (number == nullptr) {
    return false;
  }
  const bool ok = BN_to_ASN1_INTEGER(number, X509_get_serialNumber(certificate)) != nullptr;
  BN_free(number);
  return ok;
}

std::string ToPem(X509* certificate) {
  BioPtr bio{BIO_new(BIO_s_mem())};
  if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) {
    return {};
  }
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  return length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string();
}

std::string ToPem(EVP_PKEY* key) {
  BioPtr bio{BIO_new(BIO_s_mem())};
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    return {};
  }
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  return length > 0 ? std::string(data, static_cast<std::size_t>(length)) : std::string();
}

// An hour of backdate, a day of life. The backdate is for a machine whose clock
// disagrees with itself across a fork; the day is because a certificate that
// outlives its run is a certificate somebody can still use.
constexpr long kBackdateSeconds = 3600;
constexpr long kLifetimeSeconds = 24 * 3600;

}  // namespace

std::vector<std::string> CertificateHostNames(const std::string& host) {
  // The labels `{{domains[...]}}` and `{{hosts[...][...]}}` are used with in the
  // checkout, counted rather than guessed:
  //
  //     216 {{domains[www1]}}      11 {{domains[天気の良い日]}}
  //      92 {{domains[www2]}}       3 {{domains[élève]}}
  //      77 {{domains[www]}}        2 {{domains[www2.www1]}}
  //      36 {{domains[]}}           2 {{domains[www1.www1]}}
  //                                 1 {{domains[www2.www]}}
  //
  // `nonexistent` is deliberately absent: that label exists so a test can fail
  // to resolve, and a certificate for it would be a certificate for a name
  // nothing is allowed to answer on.
  //
  // The two IDN labels are here in their A-label form, because that is what the
  // client sends and what `SSL_set1_host` compares -- `src/url` runs the name
  // through UTS #46 before any of this sees it.
  static const char* const kLabels[] = {
      "www", "www1", "www2", "www2.www1", "www1.www1", "www2.www",
      "xn--lve-6lad",              // élève
      "xn--n8j6ds53lwwkrqhv28a",   // 天気の良い日
  };
  std::vector<std::string> names;
  names.push_back(host);
  names.push_back("alt." + host);
  for (const char* label : kLabels) {
    names.push_back(std::string(label) + "." + host);
    names.push_back(std::string(label) + ".alt." + host);
  }
  // A wildcard beside the explicit list rather than instead of it. OpenSSL's
  // `valid_star` rejects a pattern with fewer than two dots after the star, so
  // `*.localhost` may match nothing at all -- which is exactly why the names
  // above are spelled out. When it does match, a label nobody counted still
  // works.
  names.push_back("*." + host);
  names.push_back("*.alt." + host);
  return names;
}

GeneratedCertificate GenerateRunnerCertificate(const std::vector<std::string>& hosts) {
  GeneratedCertificate result;
  if (hosts.empty()) {
    result.error = "a certificate needs at least one host name";
    return result;
  }

  // P-256 rather than RSA: a key pair per run is on the critical path of every
  // `microbrowser_wpt` invocation, and an EC keygen is microseconds against
  // tens of milliseconds for RSA-2048.
  KeyPtr ca_key{EVP_EC_gen("prime256v1")};
  KeyPtr leaf_key{EVP_EC_gen("prime256v1")};
  if (!ca_key || !leaf_key) {
    result.error = "generating a P-256 key pair failed: " + LastOpenSslError();
    return result;
  }

  CertificatePtr ca{X509_new()};
  if (!ca) {
    result.error = "X509_new for the CA failed: " + LastOpenSslError();
    return result;
  }
  // Version 3. The constant is one less than the version, which is the ASN.1
  // encoding rather than a typo.
  X509_set_version(ca.get(), 2);
  if (!SetRandomSerial(ca.get())) {
    result.error = "the CA serial number could not be generated: " + LastOpenSslError();
    return result;
  }
  X509_gmtime_adj(X509_getm_notBefore(ca.get()), -kBackdateSeconds);
  X509_gmtime_adj(X509_getm_notAfter(ca.get()), kLifetimeSeconds);
  if (!SetName(X509_get_subject_name(ca.get()), "microbrowser web-platform-tests runner CA") ||
      X509_set_issuer_name(ca.get(), X509_get_subject_name(ca.get())) != 1 ||
      X509_set_pubkey(ca.get(), ca_key.get()) != 1) {
    result.error = "the CA subject could not be set: " + LastOpenSslError();
    return result;
  }
  if (!AddExtension(ca.get(), ca.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
      !AddExtension(ca.get(), ca.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
      !AddExtension(ca.get(), ca.get(), NID_subject_key_identifier, "hash")) {
    result.error = "the CA extensions could not be set: " + LastOpenSslError();
    return result;
  }
  if (X509_sign(ca.get(), ca_key.get(), EVP_sha256()) == 0) {
    result.error = "signing the CA failed: " + LastOpenSslError();
    return result;
  }

  CertificatePtr leaf{X509_new()};
  if (!leaf) {
    result.error = "X509_new for the leaf failed: " + LastOpenSslError();
    return result;
  }
  X509_set_version(leaf.get(), 2);
  if (!SetRandomSerial(leaf.get())) {
    result.error = "the leaf serial number could not be generated: " + LastOpenSslError();
    return result;
  }
  X509_gmtime_adj(X509_getm_notBefore(leaf.get()), -kBackdateSeconds);
  X509_gmtime_adj(X509_getm_notAfter(leaf.get()), kLifetimeSeconds);
  if (!SetName(X509_get_subject_name(leaf.get()), hosts.front().c_str()) ||
      X509_set_issuer_name(leaf.get(), X509_get_subject_name(ca.get())) != 1 ||
      X509_set_pubkey(leaf.get(), leaf_key.get()) != 1) {
    result.error = "the leaf subject could not be set: " + LastOpenSslError();
    return result;
  }

  std::string alt_names;
  for (const std::string& host : hosts) {
    if (!alt_names.empty()) {
      alt_names += ",";
    }
    alt_names += "DNS:" + host;
  }
  // `CA:FALSE` is the load-bearing half of this: the certificate the server
  // presents can authenticate these names and sign nothing.
  if (!AddExtension(leaf.get(), ca.get(), NID_basic_constraints, "critical,CA:FALSE") ||
      !AddExtension(leaf.get(), ca.get(), NID_key_usage,
                    "critical,digitalSignature,keyEncipherment") ||
      !AddExtension(leaf.get(), ca.get(), NID_ext_key_usage, "serverAuth") ||
      !AddExtension(leaf.get(), ca.get(), NID_subject_alt_name, alt_names.c_str()) ||
      !AddExtension(leaf.get(), ca.get(), NID_subject_key_identifier, "hash") ||
      !AddExtension(leaf.get(), ca.get(), NID_authority_key_identifier, "keyid:always")) {
    result.error = "the leaf extensions could not be set: " + LastOpenSslError();
    return result;
  }
  if (X509_sign(leaf.get(), ca_key.get(), EVP_sha256()) == 0) {
    result.error = "signing the leaf failed: " + LastOpenSslError();
    return result;
  }

  result.ca_certificate_pem = ToPem(ca.get());
  result.certificate_pem = ToPem(leaf.get());
  result.private_key_pem = ToPem(leaf_key.get());
  if (result.ca_certificate_pem.empty() || result.certificate_pem.empty() ||
      result.private_key_pem.empty()) {
    result.error = "serializing the certificate failed: " + LastOpenSslError();
  }
  return result;
}

}  // namespace microbrowser::wpt
