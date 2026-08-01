#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "url/Host.h"
#include "url/Origin.h"
#include "url/PartitionKey.h"
#include "url/PercentEncoding.h"
#include "url/PublicSuffixList.h"
#include "url/Url.h"

namespace microbrowser::tests {

using url::ContainerId;
using url::Origin;
using url::PartitionKey;
using url::Site;
using url::Url;

namespace {

Url MustParse(std::string_view input) {
  const auto parsed = Url::Parse(input);
  Expect(parsed.has_value(), std::string("expected to parse: ") + std::string(input));
  return *parsed;
}

void ExpectSerializes(std::string_view input, std::string_view expected) {
  const auto parsed = Url::Parse(input);
  Expect(parsed.has_value(), std::string("failed to parse: ") + std::string(input));
  ExpectEqString(parsed->Serialize(), expected,
                 std::string("wrong serialization for: ") + std::string(input));
}

void ExpectRelative(std::string_view base, std::string_view input, std::string_view expected) {
  const auto base_url = Url::Parse(base);
  Expect(base_url.has_value(), std::string("bad base: ") + std::string(base));
  const auto parsed = Url::Parse(input, *base_url);
  Expect(parsed.has_value(),
         std::string("failed to resolve '") + std::string(input) + "' against " +
             std::string(base));
  ExpectEqString(parsed->Serialize(), expected,
                 std::string("wrong resolution of '") + std::string(input) + "' against " +
                     std::string(base));
}

void ExpectRejected(std::string_view input) {
  Expect(!Url::Parse(input).has_value(),
         std::string("expected to be rejected: ") + std::string(input));
}

}  // namespace

void RegisterUrlTests(std::vector<TestCase>& tests) {
  // Cases taken from the WHATWG URL Standard's own `urltestdata.json`. Using
  // the standard's suite rather than cases invented here is the whole point:
  // a URL parser that agrees with itself and disagrees with everyone else has
  // a security bug, not a compatibility one.
  AddTest(tests, "Url/ParsesAndSerializesTheStandardsOwnCases", [] {
    ExpectSerializes("http://example.org/foo/bar", "http://example.org/foo/bar");
    ExpectSerializes("HTTP://EXAMPLE.ORG/", "http://example.org/");
    ExpectSerializes("http://example.org", "http://example.org/");
    ExpectSerializes("http://example.org:80/", "http://example.org/");
    ExpectSerializes("https://example.org:443/", "https://example.org/");
    ExpectSerializes("http://example.org:8080/", "http://example.org:8080/");
    ExpectSerializes("http://user:pass@example.org/", "http://user:pass@example.org/");
    ExpectSerializes("http://example.org/a/b/../c", "http://example.org/a/c");
    ExpectSerializes("http://example.org/a/./b", "http://example.org/a/b");
    ExpectSerializes("http://example.org/../../a", "http://example.org/a");
    ExpectSerializes("http://example.org/?q=1#frag", "http://example.org/?q=1#frag");
    ExpectSerializes("http://example.org/#", "http://example.org/#");
    ExpectSerializes("http://example.org/?", "http://example.org/?");
    // Backslashes are slashes under a special scheme, which is not a nicety:
    // a parser that treated them literally would put a different host in the
    // request than the one it checked.
    ExpectSerializes("http://example.org\\foo", "http://example.org/foo");
    ExpectSerializes("http:\\\\example.org\\foo", "http://example.org/foo");
    // Tabs and newlines are removed from anywhere in the input.
    ExpectSerializes("http://exa\tmple.org/", "http://example.org/");
    ExpectSerializes("  http://example.org/  ", "http://example.org/");
    // Non-special schemes keep an opaque path.
    ExpectSerializes("mailto:someone@example.org", "mailto:someone@example.org");
    ExpectSerializes("data:text/plain,hello", "data:text/plain,hello");
    ExpectSerializes("javascript:void(0)", "javascript:void(0)");
  });

  AddTest(tests, "Url/RejectsWhatIsNotAUrl", [] {
    ExpectRejected("");
    ExpectRejected("http://");
    ExpectRejected("://example.org");
    ExpectRejected("http://example.org:70000/");
    ExpectRejected("http://exa mple.org/");
    // Built with an explicit length: as a bare literal the NUL would terminate
    // the string_view and the parser would never see it, so the test would
    // have been checking "http://exa" — which is a perfectly good URL.
    ExpectRejected(std::string_view("http://exa\0mple.org/", 20));
    ExpectRejected("http://[1::2::3]/");
    ExpectRejected("http://256.256.256.256/");
    ExpectRejected("http://999999999999/");
    // A non-ASCII host is rejected rather than passed through, because the
    // host that gets connected to must be the host that was checked.
    ExpectRejected("http://exämple.org/");
  });

  AddTest(tests, "Url/ResolvesRelativeReferences", [] {
    const std::string base = "http://example.org/a/b/c?q#f";
    ExpectRelative(base, "d", "http://example.org/a/b/d");
    ExpectRelative(base, "/d", "http://example.org/d");
    ExpectRelative(base, "../d", "http://example.org/a/d");
    ExpectRelative(base, "../../../d", "http://example.org/d");
    ExpectRelative(base, "?x=1", "http://example.org/a/b/c?x=1");
    ExpectRelative(base, "#g", "http://example.org/a/b/c?q#g");
    ExpectRelative(base, "", "http://example.org/a/b/c?q");
    ExpectRelative(base, "//other.example/x", "http://other.example/x");
    ExpectRelative(base, "https://other.example/x", "https://other.example/x");
    ExpectRelative(base, "//other.example", "http://other.example/");
  });

  // IPv4 hosts have three radixes and four shapes, and every one of them is a
  // different spelling of the same address. Code that decides whether an
  // address is private has to agree with the code that connects to it.
  AddTest(tests, "Url/CanonicalizesEverySpellingOfAnIpv4Address", [] {
    for (const std::string_view input : {"http://127.0.0.1/", "http://127.1/",
                                         "http://2130706433/", "http://0x7f.0.0.1/",
                                         "http://0177.0.0.1/", "http://0x7f000001/"}) {
      const Url url = MustParse(input);
      ExpectEqString(url.HostSerialized(), "127.0.0.1",
                     std::string("wrong canonical host for ") + std::string(input));
      Expect(url.GetHost().IsPotentiallyPrivate(),
             "every spelling of loopback must be recognized as loopback, or a rebinding "
             "check passes for one and fails for another");
    }
  });

  AddTest(tests, "Url/CanonicalizesIpv6Addresses", [] {
    ExpectSerializes("http://[2001:db8:0:0:0:0:0:1]/", "http://[2001:db8::1]/");
    ExpectSerializes("http://[0:0:0:0:0:0:0:1]/", "http://[::1]/");
    ExpectSerializes("http://[::ffff:127.0.0.1]/", "http://[::ffff:7f00:1]/");
    Expect(MustParse("http://[::1]/").GetHost().IsPotentiallyPrivate(),
           "IPv6 loopback is loopback");
  });

  AddTest(tests, "Url/PercentEncodesPerComponent", [] {
    // The same byte is encoded in one component and left alone in another,
    // which is why there are six encode sets rather than one.
    ExpectSerializes("http://example.org/a b", "http://example.org/a%20b");
    ExpectSerializes("http://example.org/?a b", "http://example.org/?a%20b");
    ExpectSerializes("http://example.org/#a b", "http://example.org/#a%20b");
    ExpectSerializes("http://example.org/a\"b", "http://example.org/a%22b");
    // A single quote is encoded in a *special* scheme's query and not in a
    // non-special one's.
    ExpectSerializes("http://example.org/?a'b", "http://example.org/?a%27b");
    ExpectSerializes("sc://example.org/?a'b", "sc://example.org/?a'b");
  });

  AddTest(tests, "Url/PercentDecodingNeverFails", [] {
    ExpectEqString(url::PercentDecode("a%20b"), "a b", "a valid escape decodes");
    ExpectEqString(url::PercentDecode("a%zzb"), "a%zzb",
                   "an invalid escape is literal text, because that is what every other "
                   "browser does and disagreeing about what a URL means is a security bug");
    ExpectEqString(url::PercentDecode("a%"), "a%", "a truncated escape is literal too");
    ExpectEqString(url::PercentDecode("%41%42"), "AB", "consecutive escapes decode");
  });

  AddTest(tests, "Url/FileUrlsKeepTheirOwnRules", [] {
    ExpectSerializes("file:///tmp/x", "file:///tmp/x");
    ExpectSerializes("file://localhost/tmp/x", "file:///tmp/x");
    ExpectSerializes("file:///c:/x", "file:///c:/x");
  });

  // --- Origins --------------------------------------------------------------

  AddTest(tests, "Origin/TupleOriginsCompareByTheirParts", [] {
    const Origin a = Origin::FromUrl(MustParse("https://example.org/one"));
    const Origin b = Origin::FromUrl(MustParse("https://example.org/two?q#f"));
    const Origin different_scheme = Origin::FromUrl(MustParse("http://example.org/"));
    const Origin different_host = Origin::FromUrl(MustParse("https://other.example/"));
    const Origin different_port = Origin::FromUrl(MustParse("https://example.org:8443/"));

    Expect(a.IsSameOrigin(b), "path, query and fragment are not part of an origin");
    Expect(!a.IsSameOrigin(different_scheme), "scheme is");
    Expect(!a.IsSameOrigin(different_host), "host is");
    Expect(!a.IsSameOrigin(different_port), "port is");
    ExpectEqString(a.Serialize(), "https://example.org", "the default port is not serialized");
    ExpectEqString(different_port.Serialize(), "https://example.org:8443", "a non-default one is");
  });

  // The failure this prevents: every opaque origin serializes to "null", so a
  // comparison that went through the serialization would make a sandboxed
  // frame same-origin with every other sandboxed frame.
  AddTest(tests, "Origin/OpaqueOriginsAreEqualOnlyToThemselves", [] {
    const Origin a = Origin::FromUrl(MustParse("data:text/plain,one"));
    const Origin b = Origin::FromUrl(MustParse("data:text/plain,one"));
    Expect(a.IsOpaque() && b.IsOpaque(), "a data URL has an opaque origin");
    ExpectEqString(a.Serialize(), "null", "which serializes to null");
    ExpectEqString(b.Serialize(), "null", "as does the other");
    Expect(!a.IsSameOrigin(b),
           "two opaque origins are not the same origin, however identical their inputs");
    Expect(a.IsSameOrigin(a), "but each is the same origin as itself");

    const Origin file = Origin::FromUrl(MustParse("file:///tmp/x"));
    Expect(file.IsOpaque(),
           "a file URL's origin is opaque; one shared file origin is how a downloaded page "
           "reads the rest of the disk");
  });

  AddTest(tests, "Origin/TrustworthinessIsNotJustHttps", [] {
    Expect(Origin::FromUrl(MustParse("https://example.org/")).IsPotentiallyTrustworthy(), "https");
    Expect(!Origin::FromUrl(MustParse("http://example.org/")).IsPotentiallyTrustworthy(), "http");
    Expect(Origin::FromUrl(MustParse("http://localhost:3000/")).IsPotentiallyTrustworthy(),
           "localhost is trusted because an attacker on the network cannot reach it");
    Expect(Origin::FromUrl(MustParse("http://127.0.0.1/")).IsPotentiallyTrustworthy(), "loopback");
    Expect(Origin::FromUrl(MustParse("http://[::1]/")).IsPotentiallyTrustworthy(), "IPv6 loopback");
    Expect(!Origin::FromUrl(MustParse("data:text/plain,x")).IsPotentiallyTrustworthy(),
           "an opaque origin is never trustworthy");
  });

  // --- Public suffixes ------------------------------------------------------

  AddTest(tests, "PublicSuffix/FindsTheRegistrableDomain", [] {
    ExpectEqString(url::RegistrableDomain("example.com"), "example.com", "a simple case");
    ExpectEqString(url::RegistrableDomain("www.example.com"), "example.com", "subdomains drop");
    ExpectEqString(url::RegistrableDomain("a.b.c.example.com"), "example.com", "however many");
    ExpectEqString(url::RegistrableDomain("example.co.uk"), "example.co.uk",
                   "a two-label suffix is one suffix, not two labels of name");
    ExpectEqString(url::RegistrableDomain("www.example.co.uk"), "example.co.uk", "and subdomains");
  });

  AddTest(tests, "PublicSuffix/ARegistryIsNotRegistrable", [] {
    Expect(url::IsPublicSuffix("com"), "com is a registry");
    Expect(url::IsPublicSuffix("co.uk"), "and so is co.uk");
    Expect(!url::IsPublicSuffix("example.com"), "but a domain under one is not");
    ExpectEqString(url::RegistrableDomain("com"), "",
                   "nothing is registrable at a registry; returning it would make every .com "
                   "domain one site");
    ExpectEqString(url::RegistrableDomain("co.uk"), "", "same for a multi-label registry");
  });

  // Wildcards and exceptions are where a suffix-comparison implementation is
  // wrong, and a list of plain rules would never reveal it.
  AddTest(tests, "PublicSuffix/HandlesWildcardsAndExceptions", [] {
    Expect(url::IsPublicSuffix("foo.ck"), "*.ck makes any single label under ck a registry");
    ExpectEqString(url::RegistrableDomain("bar.foo.ck"), "bar.foo.ck",
                   "so the registrable domain is one label further down");
    Expect(!url::IsPublicSuffix("www.ck"),
           "!www.ck is an exception: www.ck is registrable, not a registry");
    ExpectEqString(url::RegistrableDomain("www.ck"), "www.ck", "and is its own registrable domain");
  });

  AddTest(tests, "PublicSuffix/AddressesAndUnknownSuffixesHaveNoRegistrableDomain", [] {
    ExpectEqString(url::RegistrableDomain("127.0.0.1"), "", "an address is not a name");
    ExpectEqString(url::RegistrableDomain("[::1]"), "", "nor is an IPv6 address");
    // "If no rules match, the prevailing rule is `*`" — part of the algorithm,
    // not a fallback. An unlisted TLD gets a one-label suffix, so a host under
    // it has exactly one registrable domain rather than none.
    ExpectEqString(url::RegistrableDomain("example.invalidtld"), "example.invalidtld",
                   "an unlisted TLD still yields a registrable domain, per the implicit "
                   "wildcard rule");
    ExpectEqString(url::RegistrableDomain("a.b.example.invalidtld"), "example.invalidtld",
                   "and subdomains still collapse onto it");
    ExpectEqString(url::RegistrableDomain("invalidtld"), "",
                   "but a bare unlisted TLD has nothing registrable under it");
    Expect(url::PublicSuffixListSize() > 0, "the list is compiled in, not fetched");
  });

  // --- Sites and the partition key -----------------------------------------

  AddTest(tests, "Site/IsCoarserThanAnOriginAndFinerThanARegistry", [] {
    const Site a = Site::FromUrl(MustParse("https://a.example.com/"));
    const Site b = Site::FromUrl(MustParse("https://b.example.com/"));
    const Site other = Site::FromUrl(MustParse("https://example.org/"));
    const Site insecure = Site::FromUrl(MustParse("http://a.example.com/"));

    Expect(a == b, "subdomains of one registrable domain are one site");
    Expect(!(a == other), "different registrable domains are different sites");
    Expect(!(a == insecure), "and scheme is part of a site, so http and https differ");
    ExpectEqString(a.Serialize(), "https://example.com", "a site serializes as scheme and domain");
  });

  AddTest(tests, "Site/AnAddressIsItsOwnSite", [] {
    const Site a = Site::FromUrl(MustParse("http://127.0.0.1:8080/"));
    const Site b = Site::FromUrl(MustParse("http://127.0.0.2:8080/"));
    Expect(!(a == b),
           "two addresses must not collapse into one site; falling back to empty here would "
           "put every such host in one shared bucket");
  });

  AddTest(tests, "PartitionKey/ThirdPartyStateIsKeyedByTheTopLevelSite", [] {
    // Total Cookie Protection, in one assertion. The same tracker embedded on
    // two sites gets two keys, so it cannot correlate the visits.
    const Url tracker = MustParse("https://tracker.example/pixel");
    const Site news = Site::FromUrl(MustParse("https://news.example/"));
    const Site shop = Site::FromUrl(MustParse("https://shop.example/"));

    const PartitionKey on_news = PartitionKey::ForEmbedded(ContainerId::Default(), news, tracker);
    const PartitionKey on_shop = PartitionKey::ForEmbedded(ContainerId::Default(), shop, tracker);

    Expect(!(on_news == on_shop),
           "one third party under two top-level sites must produce two partitions");
    Expect(on_news.GetOrigin().IsSameOrigin(on_shop.GetOrigin()),
           "even though it is the same origin in both");
    Expect(!on_news.IsFirstParty(), "and neither context is first-party");
  });

  AddTest(tests, "PartitionKey/ContainersPartitionTheSameSite", [] {
    const Url url = MustParse("https://example.com/");
    const PartitionKey work = PartitionKey::ForTopLevel(ContainerId{1}, url);
    const PartitionKey personal = PartitionKey::ForTopLevel(ContainerId{2}, url);
    const PartitionKey ordinary = PartitionKey::ForTopLevel(ContainerId::Default(), url);

    Expect(!(work == personal), "two containers on one site are two partitions");
    Expect(!(work == ordinary), "and neither is the default one");
    Expect(work.IsFirstParty(), "a top-level document is first-party in its own container");
    Expect(ordinary.Container().IsDefault(),
           "the default container is an ordinary value, so there is no code path that skips "
           "partitioning");
  });

  AddTest(tests, "PartitionKey/EphemeralContainersAreNeverPersisted", [] {
    const Url url = MustParse("https://example.com/");
    const PartitionKey ephemeral =
        PartitionKey::ForTopLevel(ContainerId{7, /*ephemeral=*/true}, url);
    const PartitionKey persistent = PartitionKey::ForTopLevel(ContainerId{7}, url);

    Expect(ephemeral.IsEphemeral(), "an ephemeral container marks its key");
    Expect(!persistent.IsEphemeral(), "a persistent one does not");
    Expect(!(ephemeral == persistent),
           "and the two must not share state, or a private window would inherit the "
           "ordinary one's cookies");
  });

  AddTest(tests, "PartitionKey/OpaqueOriginsGetTheirOwnPartition", [] {
    const Url data_url = MustParse("data:text/html,<p>x</p>");
    const PartitionKey a = PartitionKey::ForTopLevel(ContainerId::Default(), data_url);
    const PartitionKey b = PartitionKey::ForTopLevel(ContainerId::Default(), data_url);
    Expect(!(a == b),
           "each opaque origin is its own partition; collapsing them would make every "
           "sandboxed document share storage with every other");
  });
}

}  // namespace microbrowser::tests
