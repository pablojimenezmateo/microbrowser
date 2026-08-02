#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "privacy/BlockingEngine.h"
#include "privacy/PrivacyPolicy.h"
#include "privacy/Request.h"
#include "privacy/Verdict.h"
#include "url/Url.h"

namespace microbrowser::tests {

using privacy::BlockingEngine;
using privacy::PrivacyPolicy;
using privacy::Request;
using privacy::ResourceType;
using privacy::Verdict;
using url::ContainerId;
using url::Site;
using url::Url;

namespace {

Url MustParse(std::string_view text) {
  const auto url = Url::Parse(text);
  Expect(url.has_value(), std::string("expected to parse: ") + std::string(text));
  return *url;
}

Request MakeRequest(std::string_view url, std::string_view top_level,
                    ResourceType type = ResourceType::Script) {
  Request request;
  request.url = MustParse(url);
  const Url top = MustParse(top_level);
  request.top_level_site = Site::FromUrl(top);
  request.initiator = url::Origin::FromUrl(top);
  request.type = type;
  return request;
}

bool Blocks(const BlockingEngine& engine, std::string_view url, std::string_view top_level,
            ResourceType type = ResourceType::Script) {
  return engine.Match(MakeRequest(url, top_level, type)).blocked;
}

}  // namespace

void RegisterPrivacyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Blocking/HostAnchoredRulesCoverSubdomains", [] {
    BlockingEngine engine;
    engine.AddRules("||ads.example^");

    Expect(Blocks(engine, "https://ads.example/x.js", "https://news.example/"), "the host itself");
    Expect(Blocks(engine, "https://a.b.ads.example/x.js", "https://news.example/"),
           "and every subdomain, which is what the `||` anchor means");
    Expect(!Blocks(engine, "https://notads.example/x.js", "https://news.example/"),
           "but not a host that merely ends with the same letters — that is the bug a "
           "string-suffix implementation has");
    Expect(!Blocks(engine, "https://example.org/ads.example", "https://news.example/"),
           "and not a path that contains the pattern");
  });

  AddTest(tests, "Blocking/SubstringAndAnchorPatterns", [] {
    BlockingEngine engine;
    engine.AddRules(
        "/banner.gif\n"
        "|https://exact.example/only\n"
        "swf|\n"
        "/track/*/pixel");

    Expect(Blocks(engine, "https://cdn.example/img/banner.gif", "https://a.example/"),
           "a plain substring matches anywhere");
    Expect(Blocks(engine, "https://exact.example/only", "https://a.example/"),
           "a start anchor matches from the beginning");
    Expect(!Blocks(engine, "https://other.example/https://exact.example/only",
                   "https://a.example/"),
           "and only from the beginning");
    Expect(Blocks(engine, "https://a.example/movie.swf", "https://a.example/"),
           "an end anchor matches at the end");
    Expect(!Blocks(engine, "https://a.example/movie.swf?x=1", "https://a.example/"),
           "and only at the end");
    Expect(Blocks(engine, "https://a.example/track/abc/pixel", "https://a.example/"),
           "a wildcard spans anything");
  });

  AddTest(tests, "Blocking/ExceptionsOverrideBlocksAndImportantOverridesExceptions", [] {
    BlockingEngine engine;
    engine.AddRules(
        "||tracker.example^\n"
        "@@||tracker.example/allowed\n"
        "||evil.example^$important\n"
        "@@||evil.example^");

    Expect(Blocks(engine, "https://tracker.example/pixel", "https://a.example/"), "blocked");
    Expect(!Blocks(engine, "https://tracker.example/allowed", "https://a.example/"),
           "an exception rule wins over a block rule");
    Expect(Blocks(engine, "https://evil.example/x", "https://a.example/"),
           "but $important wins over an exception, which is what it is for");
  });

  AddTest(tests, "Blocking/ResourceTypeOptionsNarrowARule", [] {
    BlockingEngine engine;
    engine.AddRules("||cdn.example^$script,image");

    Expect(Blocks(engine, "https://cdn.example/a", "https://a.example/", ResourceType::Script),
           "a named type matches");
    Expect(Blocks(engine, "https://cdn.example/a", "https://a.example/", ResourceType::Image),
           "as does the other named type");
    Expect(!Blocks(engine, "https://cdn.example/a", "https://a.example/", ResourceType::Font),
           "but a type the rule did not name does not");
  });

  AddTest(tests, "Blocking/NegatedResourceTypeOptionsExcludeTypes", [] {
    BlockingEngine engine;
    engine.AddRules(
        "||cdn.example^$~image\n"
        "||media.example^$script,image,~image");

    Expect(Blocks(engine, "https://cdn.example/a", "https://a.example/", ResourceType::Script),
           "a negated type alone means every type except that one");
    Expect(!Blocks(engine, "https://cdn.example/a", "https://a.example/", ResourceType::Image),
           "so images are excluded");
    Expect(Blocks(engine, "https://media.example/a", "https://a.example/", ResourceType::Script),
           "a negated type can also narrow a positive type list");
    Expect(!Blocks(engine, "https://media.example/a", "https://a.example/", ResourceType::Image),
           "and removes that type from it");
  });

  AddTest(tests, "Blocking/PartyOptionsUseSiteNotOrigin", [] {
    BlockingEngine engine;
    engine.AddRules("||widget.example^$third-party");

    Expect(Blocks(engine, "https://widget.example/w.js", "https://other.example/"),
           "third-party is blocked");
    Expect(!Blocks(engine, "https://widget.example/w.js", "https://widget.example/"),
           "first-party is not");
    Expect(!Blocks(engine, "https://widget.example/w.js", "https://sub.widget.example/"),
           "and a different subdomain of the same site is still first-party, which is what "
           "filter lists mean by party and what same-site means everywhere else");
  });

  AddTest(tests, "Blocking/DomainOptionScopesARuleToTheTopLevelSite", [] {
    BlockingEngine engine;
    engine.AddRules(
        "||shared.example^$domain=one.example|two.example\n"
        "||other.example^$domain=~excluded.example\n"
        "||case.example^$domain=ONE.EXAMPLE.\n"
        "||dot.example^$domain=.two.example.\n"
        "||multi.example^$domain=one.example,domain=two.example");

    Expect(Blocks(engine, "https://shared.example/a", "https://one.example/"), "a listed domain");
    Expect(Blocks(engine, "https://shared.example/a", "https://sub.two.example/"),
           "including its subdomains");
    Expect(!Blocks(engine, "https://shared.example/a", "https://three.example/"),
           "but not an unlisted one");
    Expect(Blocks(engine, "https://other.example/a", "https://anywhere.example/"),
           "a negated list blocks everywhere");
    Expect(!Blocks(engine, "https://other.example/a", "https://excluded.example/"),
           "except where it is negated");
    Expect(Blocks(engine, "https://case.example/a", "https://one.example/"),
           "domain option entries are matched case-insensitively");
    Expect(Blocks(engine, "https://dot.example/a", "https://sub.two.example./"),
           "and leading or root dots in list and URL spelling do not change the site");
    Expect(Blocks(engine, "https://multi.example/a", "https://two.example/"),
           "multiple domain options keep one coherent domain arena range");
  });

  // A partially-understood filter fails open on the request it was written to
  // block, silently. That is worse than having no rule at all.
  AddTest(tests, "Blocking/UnrecognizedRulesAreSkippedAndCountedRatherThanGuessedAt", [] {
    BlockingEngine engine;
    engine.AddRules(
        "! a comment\n"
        "[Adblock Plus 2.0]\n"
        "\n"
        "||known.example^\n"
        "||unknown.example^$somethingnobodyimplemented\n"
        "example.com##.ad\n"
        "/regex.*pattern/");

    ExpectEqInt(static_cast<long long>(engine.RuleCount()), 1, "one rule was understood");
    ExpectEqInt(static_cast<long long>(engine.SkippedRuleCount()), 3,
                "and three were skipped: an unknown option, a cosmetic filter, and a regex");
    Expect(!Blocks(engine, "https://unknown.example/a", "https://a.example/"),
           "a rule with an option we do not implement must not be applied as though the "
           "option were absent — it might mean something far narrower");
  });

  AddTest(tests, "Blocking/PatternMatchingHandlesSeparatorsAndWildcards", [] {
    using privacy::PatternMatches;
    Expect(PatternMatches("ads^", "ads/x", false, false), "^ matches a separator");
    Expect(PatternMatches("ads^", "ads", false, false), "and the end of the text");
    Expect(!PatternMatches("ads^", "adsx", false, false), "but not an ordinary character");
    Expect(PatternMatches("a*c", "abbbc", false, false), "* spans anything");
    Expect(PatternMatches("a*c", "ac", false, false), "including nothing");
    Expect(!PatternMatches("a*c", "ca", false, false), "but the pieces must be in order");
    Expect(PatternMatches("abc", "xxabcxx", false, false), "unanchored matches anywhere");
    Expect(!PatternMatches("abc", "xxabcxx", true, false), "start-anchored does not");
    Expect(PatternMatches("abc", "abcxx", true, false), "unless it is at the start");
    Expect(PatternMatches("abc", "xxabc", false, true), "end-anchored matches at the end");
  });

  // --- The policy -----------------------------------------------------------

  AddTest(tests, "Policy/UpgradesHttpToHttps", [] {
    PrivacyPolicy policy;
    const Verdict verdict = policy.Decide(MakeRequest("http://example.com/a", "https://a.example/"));
    Expect(verdict.IsAllowed(), "an ordinary request is allowed");
    Expect(verdict.WasUpgraded(), "and upgraded");
    ExpectEqString(verdict.FinalUrl().Serialize(), "https://example.com/a",
                   "the final URL is the upgraded one, not the requested one");
  });

  AddTest(tests, "Policy/DoesNotUpgradeLoopback", [] {
    // A development server on localhost cannot be attacked from the network,
    // and upgrading it would make local development impossible without teaching
    // people to click through warnings.
    PrivacyPolicy policy;
    for (const std::string_view url : {"http://localhost:3000/x", "http://127.0.0.1:8080/x"}) {
      const Verdict verdict = policy.Decide(MakeRequest(url, "http://localhost:3000/"));
      Expect(verdict.IsAllowed(), "loopback is allowed");
      Expect(!verdict.WasUpgraded(), "and not upgraded");
    }
  });

  AddTest(tests, "Policy/AnExplicitlyAllowedHostIsNotUpgraded", [] {
    PrivacyPolicy policy;
    policy.AllowInsecureHost("legacy.example");
    const Verdict allowed =
        policy.Decide(MakeRequest("http://legacy.example/x", "https://a.example/"));
    Expect(!allowed.WasUpgraded(), "the user's per-host decision is honored");

    const Verdict other = policy.Decide(MakeRequest("http://other.example/x", "https://a.example/"));
    Expect(other.WasUpgraded(),
           "and applies to that host only — a per-host exception must not become a global "
           "switch");
  });

  AddTest(tests, "Policy/StripsTrackingParameters", [] {
    PrivacyPolicy policy;
    const Verdict verdict = policy.Decide(
        MakeRequest("https://shop.example/p?id=7&utm_source=news&fbclid=abc&q=x",
                    "https://shop.example/"));
    Expect(verdict.IsAllowed(), "allowed");
    Expect(verdict.WasSanitized(), "and sanitized");
    ExpectEqString(verdict.FinalUrl().Serialize(), "https://shop.example/p?id=7&q=x",
                   "identity-carrying parameters go, everything the server needs stays");
  });

  AddTest(tests, "Policy/AQueryThatBecomesEmptyLosesItsQuestionMark", [] {
    PrivacyPolicy policy;
    const Verdict verdict =
        policy.Decide(MakeRequest("https://a.example/p?utm_source=x", "https://a.example/"));
    ExpectEqString(verdict.FinalUrl().Serialize(), "https://a.example/p",
                   "otherwise the sanitized URL is a different cache key from the clean one a "
                   "user would have typed");
  });

  AddTest(tests, "Policy/RemoveParamRulesJoinTheSameSanitizationPass", [] {
    PrivacyPolicy policy;
    policy.Engine().AddRules("||shop.example^$removeparam=ref");
    const Verdict verdict = policy.Decide(
        MakeRequest("https://shop.example/p?ref=aff&id=7&utm_id=z", "https://shop.example/"));
    ExpectEqString(verdict.FinalUrl().Serialize(), "https://shop.example/p?id=7",
                   "a filter list's removeparam and the built-in list are one operation, not "
                   "two things that rewrite URLs and drift apart");
  });

  AddTest(tests, "Policy/BlocksBeforeUpgrading", [] {
    // A blocked request must never cause so much as a DNS lookup for the form
    // it would have been upgraded to.
    PrivacyPolicy policy;
    policy.Engine().AddRules("||tracker.example^");
    const Verdict verdict =
        policy.Decide(MakeRequest("http://tracker.example/pixel", "https://a.example/"));
    Expect(!verdict.IsAllowed(), "blocked");
    ExpectEqInt(static_cast<long long>(verdict.GetDecision()),
                static_cast<long long>(Verdict::Decision::Block), "by a filter rule");
  });

  AddTest(tests, "Policy/AVerdictCarriesThePartitionEveryPieceOfStateMustUse", [] {
    PrivacyPolicy policy;
    const Url tracker_url = MustParse("https://tracker.example/pixel");

    Request on_news;
    on_news.url = tracker_url;
    on_news.top_level_site = Site::FromUrl(MustParse("https://news.example/"));
    Request on_shop = on_news;
    on_shop.top_level_site = Site::FromUrl(MustParse("https://shop.example/"));

    const Verdict news = policy.Decide(on_news);
    const Verdict shop = policy.Decide(on_shop);
    Expect(news.IsAllowed() && shop.IsAllowed(), "both allowed");
    Expect(!(news.Partition() == shop.Partition()),
           "the same third party under two top-level sites gets two partitions, so its "
           "cookies and its connection cannot be shared between them");
  });

  AddTest(tests, "Policy/TrimsCrossOriginReferrersToTheOrigin", [] {
    PrivacyPolicy policy;
    const Url referrer = MustParse("https://news.example/secret/article?id=99");

    const Verdict cross =
        policy.Decide(MakeRequest("https://cdn.example/x.js", "https://news.example/"), &referrer);
    ExpectEqString(cross.Referrer(), "https://news.example/",
                   "a cross-origin referrer is trimmed to its origin; the full path of the "
                   "page a user came from is not the destination's business");

    const Verdict same = policy.Decide(
        MakeRequest("https://news.example/style.css", "https://news.example/"), &referrer);
    ExpectEqString(same.Referrer(), "https://news.example/secret/article?id=99",
                   "a same-origin referrer keeps its path");
  });

  AddTest(tests, "Policy/SendsNoReferrerOnADowngrade", [] {
    PrivacyPolicy policy;
    policy.AllowInsecureHost("legacy.example");
    const Url referrer = MustParse("https://news.example/article");
    const Verdict verdict =
        policy.Decide(MakeRequest("http://legacy.example/x", "https://news.example/"), &referrer);
    ExpectEqString(verdict.Referrer(), "",
                   "https to http must leak nothing, or a network attacker learns where the "
                   "user was");
  });

  AddTest(tests, "Policy/RejectsAnInvalidUrlWithoutTouchingTheNetwork", [] {
    PrivacyPolicy policy;
    Request request;
    request.top_level_site = Site::FromUrl(MustParse("https://a.example/"));
    Expect(!policy.Decide(request).IsAllowed(), "a request with no URL is not a request");
  });
}

}  // namespace microbrowser::tests
