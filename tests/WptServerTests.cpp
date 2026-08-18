#include <string>
#include <vector>

#include "TestSupport.h"
#include "wpt/Server.h"

// The substitution table, which is the half of `wpt serve` that a checkout
// cannot be read without -- and which had no test until an https origin needed
// two more entries in it.
//
// `/common/get-host-info.sub.js` is one of these files and almost every
// cross-origin test reads its origins from it, so a wrong answer here is not a
// failing test: it is a test that asks its question of the wrong server. The
// three properties below are the ones that were wrong or absent before task H9.

namespace microbrowser::tests {

namespace {

using wpt::ApplySubstitutions;
using wpt::Substitutions;

Substitutions Table(bool secure) {
  Substitutions table;
  table.host = "localhost";
  table.http_ports = {8000, 8001};
  table.https_ports = {8443, 8444};
  table.request_port = secure ? 8443 : 8000;
  table.scheme = secure ? "https" : "http";
  return table;
}

}  // namespace

void RegisterWptServerTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WptServer/HttpsPortsAreRealPorts", [] {
    // Before task H9 this answered the literal `1`, because nothing listened on
    // an https port. `fetch/metadata/helper.sub.js` builds *every* origin it
    // uses out of this substitution, so the wrong answer here was breaking
    // plain-http tests as well as `.https.` ones.
    ExpectEqString(ApplySubstitutions("{{ports[https][0]}}", Table(false)), "8443",
                   "ports[https][0]");
    ExpectEqString(ApplySubstitutions("{{ports[https][1]}}", Table(false)), "8444",
                   "ports[https][1]");
    ExpectEqString(ApplySubstitutions("{{ports[http][0]}}", Table(false)), "8000",
                   "and the http ports are unchanged");
  });

  AddTest(tests, "WptServer/AnHttpsPortWithNoServerBehindItIsNotInvented", [] {
    // A run with no certificate binds no TLS port. Answering with a plausible
    // number would make the test fail somewhere far away from the reason; the
    // old placeholder is kept, and it is a port nothing listens on.
    Substitutions table = Table(false);
    table.https_ports.clear();
    Expect(ApplySubstitutions("{{ports[https][0]}}", table) != "8443",
           "with no https port bound, the substitution does not name one that exists");
  });

  AddTest(tests, "WptServer/TheSchemeIsTheOneTheRequestArrivedOver", [] {
    // A page served over TLS that built a same-origin URL out of `http://`
    // would be issuing a cross-origin request wearing a same-origin name, and
    // every test that did it would fail nowhere near the cause.
    ExpectEqString(ApplySubstitutions("{{location[scheme]}}", Table(true)), "https", "scheme");
    ExpectEqString(ApplySubstitutions("{{location[server]}}", Table(true)),
                   "https://localhost:8443", "server");
    ExpectEqString(ApplySubstitutions("{{location[scheme]}}", Table(false)), "http",
                   "and a plain request still says http");
    ExpectEqString(ApplySubstitutions("{{location[server]}}", Table(false)),
                   "http://localhost:8000", "and names the port it arrived on");
  });

  AddTest(tests, "WptServer/AnUnknownSubstitutionIsLeftAlone", [] {
    // Deliberate, and older than this task: a test that silently got an empty
    // string where it wanted an origin fails in a way nobody can read.
    ExpectEqString(ApplySubstitutions("{{nosuchthing}}", Table(false)), "{{nosuchthing}}",
                   "an unknown substitution survives verbatim");
  });
}

}  // namespace microbrowser::tests
