#pragma once

#include <cstdint>
#include <string>

#include "url/Origin.h"
#include "url/Url.h"

namespace microbrowser::url {

// A site: scheme plus registrable domain. The unit of "same site", and the unit
// of process isolation — ADR 0004.
//
// Not the same as an origin. `a.example.com` and `b.example.com` are different
// origins and the same site, which is why one of them can set a cookie the
// other reads and why they may share a renderer process.
class Site {
 public:
  Site() = default;
  static Site FromUrl(const Url& url);
  static Site FromOrigin(const Origin& origin);

  bool IsOpaque() const { return opaque_; }
  const std::string& Scheme() const { return scheme_; }
  // The registrable domain, or the host itself when nothing is registrable
  // under it (an IP address, or a bare public suffix).
  const std::string& RegistrableDomain() const { return domain_; }

  std::string Serialize() const;

  friend bool operator==(const Site&, const Site&) = default;

 private:
  bool opaque_ = true;
  std::string scheme_;
  std::string domain_;
  std::uint64_t nonce_ = 0;
};

// A contextual identity: the user's own choice of which persona a tab browses
// as. Firefox calls them containers.
//
// `Default()` is an ordinary value rather than an absent one. That is the
// point: with no "no container" case there is no code path that skips the
// partitioning, which is how the feature stays a boundary rather than a
// preference. An ephemeral container is the same mechanism with persistence
// off, which is also all a private window is.
class ContainerId {
 public:
  constexpr ContainerId() = default;
  explicit constexpr ContainerId(std::uint32_t value, bool ephemeral = false)
      : value_(value), ephemeral_(ephemeral) {}

  static constexpr ContainerId Default() { return ContainerId{0}; }

  constexpr std::uint32_t Value() const { return value_; }
  constexpr bool IsDefault() const { return value_ == 0 && !ephemeral_; }
  // State belonging to an ephemeral container is never written to disk.
  constexpr bool IsEphemeral() const { return ephemeral_; }

  friend constexpr bool operator==(ContainerId, ContainerId) = default;

 private:
  std::uint32_t value_ = 0;
  bool ephemeral_ = false;
};

// The key every piece of per-site state is stored under: cookies, storage,
// cache entries, connection pool entries, DNS entries, TLS session tickets,
// HSTS state, permission grants.
//
// One type rather than three parameters, because the failure this prevents is
// a call site that passes two of the three and looks correct. ADR 0005 has the
// full table and the reasoning; the short version is that a partition key which
// is sometimes partial is not a partition key.
class PartitionKey {
 public:
  PartitionKey() = default;
  PartitionKey(ContainerId container, Site top_level_site, Origin origin)
      : container_(container),
        top_level_site_(std::move(top_level_site)),
        origin_(std::move(origin)) {}

  // The common case: a document at the top level, in some container.
  static PartitionKey ForTopLevel(ContainerId container, const Url& url);

  // A third party embedded in a top-level document. The distinction between
  // this and ForTopLevel is Total Cookie Protection in one function signature:
  // the same third-party origin under two different top-level sites produces
  // two different keys, and therefore two different cookie jars.
  static PartitionKey ForEmbedded(ContainerId container, const Site& top_level_site,
                                  const Url& url);

  ContainerId Container() const { return container_; }
  const Site& TopLevelSite() const { return top_level_site_; }
  const Origin& GetOrigin() const { return origin_; }

  // True when the origin's own site is the top-level site — a first-party
  // context.
  bool IsFirstParty() const;

  // Never written to disk when true, whatever the storage layer would
  // otherwise do.
  bool IsEphemeral() const { return container_.IsEphemeral(); }

  std::string Serialize() const;

  friend bool operator==(const PartitionKey&, const PartitionKey&) = default;

 private:
  ContainerId container_;
  Site top_level_site_;
  Origin origin_;
};

}  // namespace microbrowser::url
