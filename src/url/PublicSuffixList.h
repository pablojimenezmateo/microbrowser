#pragma once

#include <string>
#include <string_view>

namespace microbrowser::url {

// The Public Suffix List: which part of a hostname is a registry boundary.
//
// `example.co.uk` and `example.com` both have a two-label suffix and a one-label
// name, but `co.uk` is a registry and `com` is too, while `example.co.uk`'s
// owner is not the owner of `other.co.uk`. Nothing in DNS says so; only this
// list does. Every cookie scope, every same-site decision and every partition
// key depends on getting it right.
//
// **The list is compiled into the binary and is never fetched.** A list
// downloaded at startup would be both a network request the user did not cause
// — which `guidelines/privacy.md` forbids outright — and a remote input to a
// security decision, which is worse: whoever serves it decides whether
// `evil.example` and `bank.example` are the same site.
//
// The algorithm is the full one: ordinary rules, `*` wildcards, and `!`
// exceptions, longest match wins. The *data* here is a curated subset rather
// than the complete list — the real one is nine thousand entries and belongs in
// a generated file. Growing it is a data change, not a code change, and
// `PublicSuffixListSize()` exists so a test can notice when it happens.

// Host spelling used for public-suffix and site-key decisions. A final root
// dot marks an absolute DNS name and must not become an extra label.
std::string_view HostWithoutTrailingRootDot(std::string_view host);

// Length in labels of the public suffix of `host`, or 0 when the host is itself
// a public suffix with nothing registered under it.
std::size_t PublicSuffixLabelCount(std::string_view host);

// The registrable domain: the public suffix plus one more label. Empty when
// `host` is itself a public suffix, is an IP address, or has no suffix — all
// three mean "nothing is registrable here", and returning the host would let a
// caller treat a whole registry as one site.
std::string RegistrableDomain(std::string_view host);

// True when `host` is exactly a public suffix, such as "com" or "co.uk".
bool IsPublicSuffix(std::string_view host);

std::size_t PublicSuffixListSize();

}  // namespace microbrowser::url
