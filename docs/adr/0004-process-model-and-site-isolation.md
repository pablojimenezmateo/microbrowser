# ADR 0004 — Process Model and Site Isolation

**Status:** accepted · **Date:** 2026-08-01

## Context

ADR 0003 decided to define the UI/Engine boundary as a serialized message protocol before there were
two processes, so the split would stay a scheduling decision. It deliberately did not decide *what*
the processes are, how many there are, or what each is allowed to do. This ADR does.

The decision has to be made now, before `src/net` and `src/html` exist, because it constrains their
interfaces. "Which process does CORS run in" is not a question you can answer after writing the
network stack — the answer determines whether the network stack takes a request or takes a request
plus the identity of whoever asked, and retrofitting the second onto the first means touching every
call site.

Three forces pull against each other:

- **Security wants many processes.** C++ gives no memory safety; containment is what makes a
  memory-safety bug a crash instead of a compromise.
- **Low footprint wants few processes.** Every process has a baseline cost, and this project's
  stated goal is to be small. Chrome's process-per-site-instance is a large part of why it is
  remembered as a memory hog.
- **Correctness wants few processes.** Out-of-process iframes make synchronous same-document script
  access across frames genuinely hard, and that is where Firefox's Fission spent most of its years.

## Decision

### Four process kinds

| Process | Contains | Trusted | Sandbox |
|---|---|---|---|
| **Browser** | `app`, `ui`, `platform` | yes — it is the broker | none |
| **WebContent** | `engine`, `html`, `dom`, `css`, `layout`, `paint`, `js` | no | maximal |
| **Network** | `net`, `privacy` | partially | no filesystem beyond the profile, no window system |
| **ImageDecoder** | image decoding only | no | maximal, no filesystem, no network |

This is close to Ladybird's shape (`WebContent`, `RequestServer`, `ImageDecoder`), and for the same
reasons. The ImageDecoder split in particular is the highest security value per line of work in the
whole model: image decoders are old, fast, reached before any policy check, and historically the
single most productive source of browser RCE — and their interface is small enough (bytes in, a
bitmap out) that isolating them costs almost nothing.

### Isolation is per site instance

- A **site** is `scheme` + registrable domain, computed against a Public Suffix List **compiled into
  the binary**. Not per tab: a tab hosts cross-origin iframes, and the iframe is the part an attacker
  controls, so per-tab isolation leaves the interesting case unprotected while sounding solved.
- Site rather than origin because `document.domain` can still widen an origin to its registrable
  domain. When that is removed from the platform this tightens to origin, and it is one type.
- A **site instance** is a site plus a browsing-context group, so two unrelated tabs on the same site
  do not share an address space merely for sharing a domain.
- Cross-site iframes are out-of-process. This is the expensive part and it is the point.

### The browser process never takes the renderer's word for anything

A WebContent process is assumed to be running the attacker's code. It follows that:

- The browser process knows which site instance a connection belongs to **from having created it**.
  Site identity is never a message field.
- A message field naming an origin, a path, a length, or an index is an attacker-controlled string.
- Policy runs where the attacker is not. CORS is checked in the network process, cookie access is
  resolved in the browser process, and file access is mediated by a browser-process dialog that
  passes back a descriptor, never a path.

### The memory cost is capped, never the isolation

Above a process limit derived from system RAM, a new same-site instance is consolidated into an
existing process instead of getting its own. **Cross-site consolidation never happens.** The cap
gives up performance isolation and crash isolation, which are recoverable; it does not give up the
security boundary, which is not. Backgrounded site instances are eviction candidates and restore
from session state.

### Sequencing

The split lands with M7, when there is a browser worth sandboxing. Until then everything runs in one
process behind the boundary ADR 0003 already established. Two things happen earlier because they
are cheap now and expensive later:

- M2: the site key type, the baked-in PSL, and partitioning by `(top-level site, origin)` — the data
  structures the isolation model needs, in the module that owns them.
- M2: close-on-exec on every descriptor-creating call, linted, so an inherited descriptor cannot
  already be a habit by the time there is a child to inherit it.

## Consequences

- **`net::Fetch` takes the requesting site instance, not just a URL.** Partitioned connection pools,
  partitioned DNS cache, and CORS enforcement all need it, and adding the parameter later would
  touch every call site. It is in the signature from the first commit.
- **`src/engine` never gains a filesystem or socket API.** It already cannot include `platform`; that
  restriction now has a security reason on top of the architectural one.
- **Rendered output crosses as pixels in shared memory.** `PaintFrameMessage` becomes a handle plus
  damage rects when display lists get large, which was already anticipated in ADR 0003 as a change to
  one message.
- **Debugging spans processes.** The observability tooling has to attribute scopes and counters per
  process; `guidelines/observability.md` gains a process column when the split lands.
- **Startup cost per site instance becomes a tracked number.** Process launch is on the navigation
  path, so it competes directly with first paint. It needs a perf-harness scenario before M7, not
  after.
- **A single-process mode stays available for development and tests**, and is never a supported
  configuration for browsing. The tests drive the engine in-process today and will continue to.

## Alternatives Considered

**One process, no sandbox.** What the project has today, appropriate while there is no HTML parser.
Not defensible once there is one: it makes every parser bug a full compromise, and the parsers are
the next thing being written.

**Process per tab.** The intuitive design, and the one most people mean by "tab isolation". Cheaper,
and stops a crash in one tab from taking the window down. Rejected as a *security* boundary because a
tab is not an origin: it hosts cross-origin iframes, and those are attacker-controlled. It would
give the appearance of isolation with the actual attack unaddressed.

**Process per origin.** Stronger than per site, and where the platform is heading. Rejected for now
only because `document.domain` still exists, which lets two origins in the same registrable domain
become script-accessible to each other — an origin boundary that the platform itself can widen is not
a boundary. Revisit when it is removed.

**A separate process for JavaScript.** Attractive, and not workable: script has synchronous access to
the DOM by specification. The seam would be a synchronous IPC per property access.

**Threads instead of processes.** No. A thread shares the address space, which is the entire thing
being isolated.

## Open

**Windows and macOS sandboxing.** The Linux layers (namespaces, seccomp-bpf, `no_new_privs`) are
decided; the equivalents (`AppContainer`, Seatbelt) are not designed. Neither blocks M7 on Linux, and
designing all three before any one exists would be speculation.

**Where compositing runs.** Today the browser process rasterizes the display list the engine
produced. With out-of-process iframes there are several display lists to composite, from several
untrusted processes, and the compositor becomes a thing that consumes untrusted data in a trusted
process. That is a boundary-3 parser and should be treated as one. Revisit with M6.

**Whether the network process needs its own split.** TLS and HTTP parsing on hostile bytes sit next
to the cookie jar. The privacy layer arguably wants to be closer to the browser process than to the
socket. Not urgent, and not decidable without the code.
