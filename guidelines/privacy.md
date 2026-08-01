# Privacy Guide

Privacy is priority three, below correctness and security and above speed. It is a constraint on
every feature, not a feature area.

**Privacy is about what leaves the machine. Security is about what a page can reach.** The two are
defended differently and fail differently, which is why they are separate documents — see
`guidelines/security.md`. They meet in one place worth remembering: a browser with perfect privacy
defaults and a heap overflow in its image decoder has no privacy at all, because the attacker is
running inside the process that was enforcing it.

## The Contract

**No network request the user did not cause.** No telemetry, no crash reporting, no remote
configuration, no update pings, no safe-browsing lookups, no search suggestions, no link prefetch,
no speculative connections, no favicon fetches to third parties. If you cannot point at the user
action that caused a packet, it must not be sent.

**Every request passes `src/privacy` before `src/net` sees it.** `net::Fetch` will take a
`privacy::Verdict` by value, and there will be no overload without one. That is deliberately not a
convention: it is a signature that cannot be bypassed without editing the signature.

**Everything is partitioned by `(top-level site, origin)`.** Cookies, cache entries, storage keys,
connection pool entries, DNS cache entries. Total Cookie Protection by construction rather than by
policy flag, because a flag can be off and a data structure cannot.

**HTTPS-only by default.** Downgrading is an explicit per-site act with an interstitial, not a
silent fallback.

**Nothing persists to disk unless the user opted in.** The HTTP cache is memory-only by default.
`platform::AppDirectories` is the only place that decides where anything is written, and it creates
its directories owner-only — a profile holds cookies and history, and the default 0755 makes both
world-readable on a shared machine.

**A feature that cannot be built without weakening one of these does not get built.**

## The LibreWolf Baseline

These are engine-level, not an extension:

- **Content blocking** — Adblock Plus / uBO filter syntax compiled to a matcher (hostname trie,
  tokenized substring index, regex fallback), plus cosmetic filters applied at style time. Ships
  with EasyList, EasyPrivacy, and uBO's own lists.
- **URL sanitization** — `utm_*`, `fbclid`, `gclid`, and the rest of a maintained parameter list,
  stripped on navigation and on copy-link.
- **Referrer trimming** — cross-origin referrers trimmed to origin, always.
- **Anti-fingerprinting** — standardized `navigator` surface, `Accept-Language: en-US` regardless of
  system locale, quantized timers, no WebGL, canvas readback gated.
- **No DRM, no password manager, no sync, no sponsored content.** Not "off by default" — absent.

## Where This Gets Lost

Privacy failures in browsers are almost never a decision to violate privacy. They are convenience
features whose network cost nobody costed:

- A favicon fetched from a third-party service instead of the origin.
- A "check for updates" call added for good reasons on a timer nobody remembers.
- A DNS prefetch on hover, which leaks every link you *considered* clicking.
- A connection opened speculatively to "warm the pool", which announces the visit before the click.
- An error page that fetches a stylesheet from a CDN.
- A crash handler that helpfully phones home.

Every one of these is defensible in isolation. The defense against all of them is the same:
**the user action must be nameable.** When adding anything that touches the network, write down
which user action causes it. If the answer is "a timer" or "page load, speculatively", it does not
ship.

## Threat Model

What this browser tries to protect a user's *information* from. The complementary list — what it
tries to protect the *machine* from — is in `guidelines/security.md`, and malicious page content
belongs there rather than here.

- **Cross-site tracking** — cookies, storage, cache timing, connection reuse, fingerprinting. The
  structural defense is partitioning: a data structure keyed by `(top-level site, origin)` cannot be
  switched off the way a policy flag can.
- **Passive network observation** — HTTPS-only, no plaintext fallback, no DNS leaks past a proxy.
- **The browser vendor** — which is to say, us. Zero telemetry means the project cannot learn
  anything about a user even if it wanted to. This is why "we would only collect anonymous
  aggregates" is not a conversation worth having: the guarantee is structural, and a structural
  guarantee with one exception is not a guarantee.

What it does not protect against, and should not claim to: **traffic analysis**, a **global passive
adversary**, or a determined fingerprinter with a novel surface nobody has standardized yet. Tor
Browser solves a different problem; do not imply this one does. Anti-fingerprinting here is
best-effort by nature — it is an arms race, and saying otherwise oversells it.

## Reviewing A Change

- Does it send a packet? What user action causes it?
- Does it write to disk? Did the user opt in? Is it in `AppDirectories`?
- Does it store per-site state? Is the key partitioned by top-level site?
- Does it add an observable difference between users — a font list, a timing signal, a screen
  metric, a locale? That is a fingerprinting surface.
- Does it weaken a default to make something work? Then it does not work yet.
