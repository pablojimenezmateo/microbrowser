#include "url/PartitionKey.h"

#include <atomic>

#include "url/PublicSuffixList.h"

namespace microbrowser::url {

namespace {

std::uint64_t NextOpaqueNonce() {
  static std::atomic<std::uint64_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

Site Site::FromUrl(const Url& url) {
  return FromOrigin(Origin::FromUrl(url));
}

Site Site::FromOrigin(const Origin& origin) {
  Site site;
  if (origin.IsOpaque()) {
    // An opaque origin's site is opaque and unique. Collapsing them into one
    // "no site" value would make every sandboxed frame same-site with every
    // other, which is the same bug as making them same-origin.
    site.nonce_ = NextOpaqueNonce();
    return site;
  }

  site.opaque_ = false;
  site.scheme_ = origin.Scheme();
  const std::string registrable = url::RegistrableDomain(origin.Host());
  // An IP address or a bare public suffix has nothing registrable under it, and
  // in both cases the host itself is the finest boundary available. Falling
  // back to the host is right; falling back to *empty* would put every such
  // host in one shared bucket.
  site.domain_ = registrable.empty() ? origin.Host() : registrable;
  return site;
}

std::string Site::Serialize() const {
  if (opaque_) {
    return "null";
  }
  return scheme_ + "://" + domain_;
}

PartitionKey PartitionKey::ForTopLevel(ContainerId container, const Url& url) {
  Origin origin = Origin::FromUrl(url);
  Site site = Site::FromOrigin(origin);
  return PartitionKey(container, std::move(site), std::move(origin));
}

PartitionKey PartitionKey::ForEmbedded(ContainerId container, const Site& top_level_site,
                                       const Url& url) {
  return PartitionKey(container, top_level_site, Origin::FromUrl(url));
}

bool PartitionKey::IsFirstParty() const {
  return Site::FromOrigin(origin_) == top_level_site_;
}

std::string PartitionKey::Serialize() const {
  std::string out = "(";
  out += container_.IsEphemeral() ? "ephemeral:" : "container:";
  out += std::to_string(container_.Value());
  out += ", ";
  out += top_level_site_.Serialize();
  out += ", ";
  out += origin_.Serialize();
  out.push_back(')');
  return out;
}

}  // namespace microbrowser::url
