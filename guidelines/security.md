# Security Guide

Security is priority two, above privacy and speed. A browser is a program whose entire job is to
download code written by strangers and run it on your machine. That is the threat model; everything
here follows from it.

Privacy and security are related but not the same, and conflating them loses both. **Privacy is
about what leaves the machine. Security is about what a page can reach.** A browser with perfect
privacy defaults and a heap overflow in its JPEG decoder has no privacy at all, because the attacker
is inside the process that was supposed to be enforcing it. See `guidelines/privacy.md` for the
other half.

## Quick Scan

- Every byte from the network is attacker-controlled. So is every message from a renderer process.
- The trust boundary is the IPC seam. The UI side validates; the engine side is assumed compromised.
- Isolation is per **site**, not per tab. A tab hosts many origins; a process must host one site.
- C++ is memory-unsafe. Sandboxing is what makes that survivable, not care.
- A security fix outranks a performance regression. Say so in the commit; do not quietly trade.
- No feature ships that requires disabling a default. That is not a tradeoff, it is a removal.

## Trust Boundaries

There are exactly three, and knowing which one you are writing code against is most of the job.

```
     ┌─────────────────────────────────────────────┐
     │  UI / browser process        TRUSTED        │   the broker: window, input,
     │  src/app  src/ui  src/platform              │   profile, network policy
     └──────────────────┬──────────────────────────┘
                        │  ── BOUNDARY 1: the IPC seam ──
                        │     everything crossing is untrusted data
     ┌──────────────────┴──────────────────────────┐
     │  WebContent process (one per site instance) │   UNTRUSTED. Assume it is
     │  src/engine  html dom css layout paint js   │   already running the
     └──────────────────┬──────────────────────────┘   attacker's code.
                        │  ── BOUNDARY 2: same-origin policy ──
                        │     enforced *inside* the renderer, so it is
                        │     defense in depth, never the only defense
     ┌──────────────────┴──────────────────────────┐
     │  Documents from different origins            │
     └─────────────────────────────────────────────┘

     ── BOUNDARY 3: the parser ──
     src/net response bytes, image bytes, font bytes, filter lists, the profile
     on disk. Hostile even inside a trusted process.
```

**Boundary 1 is the one that matters.** The renderer is where untrusted code runs, so the renderer
is where the attacker eventually is. Every design question reduces to: *what does the attacker get
when they own the renderer?* The answer must be "that site's data, and nothing else".

**Boundary 2 is not a security boundary on its own.** The same-origin policy is enforced by the
renderer, and a compromised renderer enforces nothing. It stops a bug from becoming a cross-origin
read; it does not stop an exploit. That is exactly why boundary 1 has to carry the weight — this is
the lesson Chrome learned with UXSS and Firefox learned with Fission, and it is why site isolation
exists at all.

**Boundary 3 is everywhere.** A parser in the trusted process is a parser that grants the attacker
the trusted process. See "Hostile Input" below.

## The Process Model

Decided in `docs/adr/0004-process-model-and-site-isolation.md`. The short version:

| Process | Runs | Trusted? | Sandbox |
|---|---|---|---|
| **Browser** | `app`, `ui`, `platform` — window, input, profile, policy | yes | none; it is the broker |
| **WebContent** | `engine`, `html`, `dom`, `css`, `layout`, `paint`, `js` | **no** | maximal |
| **Network** | `net`, `privacy` — DNS, TLS, HTTP, cookies, cache, filters | partially | restricted |
| **ImageDecoder** | image format decoding only | **no** | maximal, and no filesystem at all |

Today all four are one process, with the boundary between the first two already defined as a
serialized message protocol (`docs/adr/0003-ipc-seam-before-the-process-split.md`). The split is
scheduled, not hypothetical, and the rules below are written against the split so that nothing has
to be retrofitted.

### Isolation is per site, not per tab

"One process per tab" is the intuitive design and it is not sufficient. A tab is a window; a
document tree is not. A single tab hosting `news.example` with an embedded `ads.example` iframe puts
two origins' data in one address space, and the iframe is exactly the part the attacker controls.
Per-tab isolation would leave the interesting case unprotected while sounding like it was solved.

So the unit is a **site instance**:

- A **site** is `scheme` + registrable domain, computed against a baked-in Public Suffix List. So
  `https://a.example.com` and `https://b.example.com` are the same site; `http://example.com` and
  `https://example.com` are not.
- Site, not origin, because `document.domain` can still widen an origin to its registrable domain.
  When that legacy behavior is finally removed, this tightens to origin and the type changes in one
  place.
- A **site instance** is a site plus a browsing-context group, so two unrelated tabs on the same site
  do not share memory just because they share a domain.
- Cross-site iframes get their own process (out-of-process iframes). This is the expensive part and
  it is the whole point.

The PSL is **compiled in, never fetched.** A list downloaded at runtime is a network request the
user did not cause (a privacy violation) and a remote input to a security decision (a security
violation). It updates when the browser updates.

### The memory cost is real, and is the tradeoff we accept

Process-per-site-instance costs tens of megabytes per process, against a project whose stated goal
is a low footprint. The resolution is a cap plus consolidation, not an exception:

- Above a process limit derived from system RAM, same-site instances are consolidated into an
  existing process rather than a new one. **Never cross-site.** The cap trades away performance
  isolation, never security isolation.
- Processes for backgrounded site instances are candidates for eviction, and the tab restores from
  session state.
- The measurement to watch is per-process baseline RSS. Every megabyte in a WebContent process is
  multiplied by the number of sites open, so the object-size budgets in `docs/adr/0002` are
  load-bearing here too.

### What a compromised renderer must not be able to do

This is the checklist a sandbox is designed against, and the thing to check any new IPC message
against:

- **Read another site's data.** No cookies, no storage, no cache, no rendered pixels. Enforced by
  the browser process handing a renderer only what its site is entitled to — never by asking the
  renderer which site it is.
- **Read or write the filesystem.** No profile, no downloads directory, no `/etc`, no `/proc/self`.
  File uploads arrive as a descriptor the browser process opened after the user picked the file in a
  browser-process dialog. The renderer never sees a path.
- **Open a socket, resolve a name, or reach the network.** All of it goes through the network
  process, which applies the privacy layer to a request it does not trust.
- **Spawn a process, load a library, or make an unexpected syscall.** Seccomp policy denies by
  default.
- **Talk to the window system.** No X11 or Wayland socket, which is a notorious escape route: an X11
  client can read every other window's keystrokes. Rendering output crosses as pixels in shared
  memory, and input crosses as messages.
- **Claim to be something it is not.** The browser process knows which site each connection belongs
  to from having created it, and never takes the renderer's word for it. **A message field that
  names an origin, a site, a partition key, or a file path is an attacker-controlled string.**

### The sandbox layers (Linux, first target)

Applied in the child between `fork` and the first line of untrusted work, all before any content is
loaded:

1. `PR_SET_NO_NEW_PRIVS` — no setuid binary can regain privileges.
2. User + mount + PID + network + IPC + UTS namespaces — an empty filesystem view, no other
   processes visible, and no network namespace at all, so a socket cannot even be created.
3. `seccomp-bpf`, allowlist. Deny by default and `SIGSYS` on violation, so an attempt is a crash
   with a syscall number in the log rather than a silent success.
4. `RLIMIT_NOFILE`, `RLIMIT_NPROC`, `RLIMIT_FSIZE` as a backstop.
5. Descriptors: the child inherits exactly one — its end of the socketpair. Everything else is
   created `O_CLOEXEC`/`SOCK_CLOEXEC` **on the creating call**, never with a follow-up `fcntl`,
   because the window between the two is a `fork` on another thread away from leaking.

A sandbox that is applied late is not a sandbox. The ordering rule is: privileges are dropped before
the first byte of content is parsed, and there is no path that re-acquires them.

## Hostile Input

Rules in `guidelines/cpp.md` under "Hostile Input"; the reasoning is here.

Every parser is an attack surface, and the ones that matter most are the ones nobody thinks of as
parsers: image decoders, font tables, compression streams, filter lists, the session file. Historic
browser RCE lands in decoders far more often than in the JavaScript engine, because decoders are
old, fast, written in C, and reached before any policy check runs.

- **Bounds-check every read; never trust a length prefix.** `ipc::ByteReader` validates a claimed
  length against the bytes that actually remain *before* allocating. A frame claiming a 4 GiB string
  with four bytes left must fail, not attempt a 4 GiB reserve.
- **Sticky failure, total decoding.** A decoder runs straight through and is checked once at the
  end. A malformed input yields `nullopt`, never a half-populated object — a half-populated object
  is how a validated field ends up next to an unvalidated one.
- **Reject, do not ignore.** Trailing bytes mean the two ends disagree about the payload shape.
- **Integer overflow is the bug under the bug.** `width * height * 4` in `int` is the classic image
  decoder heap overflow: the multiply wraps, a small buffer is allocated, and a large copy follows.
  Use `util/SaturatingMath.h` and 64-bit intermediates for any size computed from input.
- **Every parser that touches network bytes gets a libFuzzer target on the commit it lands.** Not
  the commit after. See `guidelines/testing.md`.
- **Decode untrusted media in the ImageDecoder process.** The interface is small — bytes in, a
  bitmap and its dimensions out — which makes the isolation cheap, and the bytes are the most
  dangerous input the browser handles.

## Memory Safety

C++ was chosen for this project with the tradeoff understood: **the language does not give us
memory safety, so the architecture has to.**

Layer 1 — do not write the bug:
- RAII and value semantics. No owning raw pointers, no `malloc`/`free`. Linted
  (`NoManualHeapOwnership`).
- The banned C function list. Linted (`NoBannedCFunctions`).
- `std::span` and container iteration at boundaries; never a pointer plus a separately-carried
  length, which is two facts that can disagree.
- Saturating arithmetic on anything derived from input.

Layer 2 — find the bug:
- ASan, UBSan, and TSan run clean and stay that way (`tools/run-checks.sh`).
- Fuzzing on every parser, with the corpus committed.
- Pixel reference tests, which catch the class of decoder bug that produces wrong output before it
  produces corruption.

Layer 3 — survive the bug:
- The sandbox. This is the layer that actually holds, and it is the reason a memory-safety bug is a
  contained crash rather than a compromise. Layers 1 and 2 reduce how often layer 3 is tested.

**Use-after-free is the bug class to be most afraid of**, because it is the most reliably
exploitable and the least visible in review. It shows up when a node is removed during event
dispatch, when a layout box outlives the DOM node it points at, or when a callback holds a reference
across a task boundary. When the DOM lands, ownership between the DOM, layout, and paint trees is a
design decision to be written down in an ADR, not an emergent property of who wrote which tree first.

### Concurrency and shutdown

Threads are a security surface, not just a performance one: a data race on a pointer is a
use-after-free with extra steps.

- **Every worker thread is joined before `main` returns.** Static destructors run after that, and
  code that disarms itself at exit (`util::TraceChannel` does) is ordering the exiting thread
  against itself, not synchronizing with a worker. A worker still running at that point can read a
  flag one instruction before it is cleared and then touch freed memory.
- A new thread arrives with its ownership story written down: what it owns, what it borrows, and who
  joins it.

## Web Platform Security

The engine-level defaults. These are not settings.

- **Same-origin policy** on DOM access, storage, and network reads. Defense in depth inside a
  renderer that is itself isolated by site.
- **HTTPS-only.** Downgrade is an explicit per-site act behind an interstitial, never a silent
  fallback. Mixed content is blocked, not warned about.
- **Content-Security-Policy** parsed and enforced, including `frame-ancestors` for clickjacking.
- **Cookies:** `SameSite=Lax` when unspecified, `Secure` required for `SameSite=None`, `HttpOnly`
  honored, and partitioned by top-level site regardless of what the cookie asks for.
- **`X-Frame-Options` / `frame-ancestors`, `X-Content-Type-Options: nosniff`.** MIME sniffing exists
  and is bounded by the spec's algorithm, never extended for compatibility.
- **CORS** enforced on the network side, so a compromised renderer cannot request a cross-origin
  response and read it. This is the practical reason CORS lives in the network process: the check
  has to happen where the attacker is not.
- **`sandbox` iframes, COOP, COEP.** Cross-origin isolation is what gates high-resolution timers and
  `SharedArrayBuffer`, because Spectre makes a precise clock a cross-origin read primitive. Timer
  quantization and jitter are on by default and are also a fingerprinting defense — see
  `guidelines/privacy.md`.
- **No plugins, no extensions, no DRM, no native messaging.** Whole categories of vulnerability that
  are absent rather than mitigated.

## Supply Chain

- Dependencies are sanctioned individually by ADR (`docs/adr/0001-third-party-dependencies.md`) and
  named by exactly one module. The short list is a security property, not just a build one.
- No package manager, no build-time downloads, no post-install scripts. The build reads the source
  tree and the system package manager's headers, and nothing else.
- No auto-update, no remote configuration, no telemetry endpoint. There is no channel from us to a
  running browser, which means there is no channel for someone who compromises us either.

## What This Does Not Protect Against

Stated so it is never implied otherwise:

- A compromised operating system, or a user running the browser as root.
- A malicious extension — there are none, which is the mitigation.
- Physical access to an unlocked machine.
- Traffic analysis, or a global passive adversary. Tor Browser solves a different problem.
- A user who is convincingly told to type their password into an attacker's page. Phishing is a UI
  problem, and the UI answer is an omnibox that shows the origin accurately and cannot be occluded
  by page content.

## Reviewing A Change

- **Does it add a message across the IPC seam?** Then the payload is attacker-controlled. Which
  field is a path, an origin, a length, or an index — and what happens when each is a lie?
- **Does it move a decision into the renderer?** Then it is no longer enforced. Policy belongs in
  the browser or network process; the renderer gets the outcome.
- **Does it parse bytes?** Where do the bytes come from, is there a fuzz target, and does every
  size computed from them saturate?
- **Does it allocate based on an input number?** Bound it against what is actually available before
  allocating, not after.
- **Does it open a descriptor, a socket, or a file?** Is it close-on-exec on the creating call, and
  should the renderer have been able to ask for it at all?
- **Does it add a thread?** Who joins it, and before what?
- **Does it weaken a default so something works?** Then it does not work yet.

## Enforced Today, And What Lands When

Honest status, so nothing here reads as shipped when it is not:

| Rule | State |
|---|---|
| Banned C functions | **linted** (`NoBannedCFunctions`) |
| No manual heap ownership | **linted** (`NoManualHeapOwnership`) |
| No mutable state at namespace scope | **linted** |
| Non-throwing, locale-independent parses | **linted** |
| IPC wire format is hostile-input hardened | **shipped** (`ipc::ByteReader`, round-tripped every build) |
| Module boundaries keep the engine free of window/OS handles | **linted** |
| ASan / UBSan / TSan clean | **shipped** |
| Close-on-exec on the creating call | lint lands with `src/net` (M2) |
| No fetch without a `privacy::Verdict` | signature lands with `src/privacy` (M2) |
| Fuzz target per network-facing parser | lands per parser, M2 onward |
| Site computation against a baked-in PSL | M2 |
| Process split, sandbox, site isolation | M7 target; see ADR 0004 |

The rules that are not yet lints are deliberately not written as vacuous ones. A rule with zero call
sites passes while checking nothing, which is indistinguishable from having no rule — the failure
mode `guidelines/testing.md` exists to prevent.
