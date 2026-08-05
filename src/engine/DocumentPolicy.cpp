#include "engine/DocumentPolicy.h"

#include <utility>

#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

std::string_view NameOf(csp::Directive directive) {
  switch (directive) {
    case csp::Directive::Default:
      return "default-src";
    case csp::Directive::Script:
      return "script-src";
    case csp::Directive::Style:
      return "style-src";
    case csp::Directive::Img:
      return "img-src";
    case csp::Directive::Connect:
      return "connect-src";
    case csp::Directive::FormAction:
      return "form-action";
    case csp::Directive::BaseUri:
      return "base-uri";
  }
  return "?";
}

// A violation line is bounded, and the bound is here rather than at the caller
// because every string in one comes from a page: the URL it asked for, and a
// directive name that does not. A page that refuses a thousand resources must
// not be able to grow this list without limit.
constexpr std::size_t kMaxViolations = 64;
constexpr std::size_t kMaxViolationUrlLength = 256;

}  // namespace

void DocumentPolicy::Reset(csp::PolicyList policies, std::string_view document_url) {
  policies_ = std::move(policies);
  base_ = url::Url::Parse(document_url);
  base_from_element_ = false;
  self_ = base_.has_value() ? url::Origin::FromUrl(*base_) : url::Origin{};
  violations_.clear();
}

void DocumentPolicy::AddFromMeta(std::string_view value) { policies_.AddFromMeta(value); }

bool DocumentPolicy::SetBase(std::string_view href) {
  if (!base_.has_value()) {
    // Nothing to resolve against. A `<base href>` in a `data:` document names
    // nothing, which is what every browser does with one.
    return false;
  }
  if (!AllowsUrl(csp::Directive::BaseUri, href)) {
    return false;
  }
  std::optional<url::Url> resolved = url::Url::Parse(href, *base_);
  if (!resolved.has_value()) {
    return false;
  }
  base_ = std::move(resolved);
  base_from_element_ = true;
  return true;
}

void DocumentPolicy::UpdateDocumentUrl(std::string_view url) {
  if (base_from_element_) {
    return;
  }
  if (std::optional<url::Url> parsed = url::Url::Parse(url)) {
    base_ = std::move(parsed);
  }
}

bool DocumentPolicy::AllowsUrl(csp::Directive directive, std::string_view written_url,
                               std::string_view nonce) const {
  if (!policies_.Governs(directive)) {
    // The common case, and it must cost a bool test rather than a URL parse: a
    // page with no policy is most pages, and every image on it comes through
    // here.
    return true;
  }
  const std::optional<url::Url> target =
      base_.has_value() ? url::Url::Parse(written_url, *base_) : url::Url::Parse(written_url);
  if (!target.has_value()) {
    Record(directive, written_url);
    return false;
  }
  if (policies_.AllowsUrl(directive, *target, self_, nonce)) {
    return true;
  }
  Record(directive, target->Serialize(true));
  return false;
}

void DocumentPolicy::Record(csp::Directive directive, std::string_view what) const {
  AddPerformanceCounter(PerfCounterId::CspViolations);
  if (violations_.size() >= kMaxViolations) {
    return;
  }
  std::string line("Refused by ");
  line += NameOf(directive);
  line += ": ";
  line += what.substr(0, kMaxViolationUrlLength);
  violations_.push_back(std::move(line));
}

}  // namespace microbrowser::engine
