# Roadmap — from Hacker News to any page

**Written 2026-08-04**, against `docs/surveys/2026-08-04-reddit-youtube-plex.md` and ADRs 0015–0030.

`README.md`'s milestones say what to *build*. ADR 0007 names five sites that must *work*. This
document is the third thing: **the order to do it in, sized into sessions, with the check that says
each one landed.**

## How to read this

- **A session is one sitting's worth of work** — a coherent feature, its tests, and a commit or
  several. The counts are estimates and the ones late in the document are estimates about work
  nobody has scoped. Treat the *order* as the decision and the *count* as a guess.
- **Every session ends with a check that a person can run**, usually `microbrowser_snapshot` against
  a real URL. `CLAUDE.md`'s strongest lesson is that looking at a rendered page finds what tests do
  not, and every session here is built so that looking is possible.
- **Where a session's order disagrees with a milestone number, this wins**, for the reason ADR 0014
  already gave: the milestones describe what to build, the measurements describe what to build first.
- **The ADR column is where the reasoning lives.** This document does not re-argue decisions; if a
  session looks wrong, the argument is in the ADR.

---

## The phases

| Phase | Sessions | Gate — the thing that is true when it ends |
|---|---|---|
| **A. reddit's front door** | 1–7 | `microbrowser https://www.reddit.com/` gets past the challenge and paints a styled page with photographs on it |
| **B. reddit works** | 8–15 | the feed loads, the menus open, links navigate, scrolling feels right |
| **C. youtube renders** | 16–21 | youtube.com's home page appears, with its own fonts, and can be scrolled and clicked |
| **D. Plex plays** | 22–29 | sign in, browse a library, play a video from a user's own server, with sound |
| **E. any page** | 30–40+ | encodings, bidi, iframes, the process split, incremental paint |

Phases A and B are the shortest and deliver the most, which is the opposite of what the milestone
numbering implied. That is the survey's main finding.

---

## Phase A — reddit's front door

The measurement that sets this phase: **`GET https://www.reddit.com/` returns an 8KB JavaScript
challenge, not a page.** Getting past it needs seven small bindings, none of which was on any
roadmap. Everything else in this phase is what makes the page behind it legible.

### Session 1 — the door · ADR 0026 §4, ADR 0017 §2

`DOMContentLoaded` and the document lifecycle (`readyState`, `load`), `document.forms`,
`form.elements` with `namedItem()`, **`form.requestSubmit()` distinct from `submit()`**,
`URLSearchParams`, `location.search`, and the navigation a GET form submission causes — carrying the
cookie from the first response.

> `requestSubmit()` fires `submit` and runs validation; `submit()` does neither. Reddit's handler
> appends the query parameters in `onsubmit`. Aliasing the two submits the form without them, and
> the challenge fails silently.

**Check:** `microbrowser_snapshot https://www.reddit.com/ -o out.ppm` produces a page whose title is
"Reddit - The heart of the internet", not "Please wait for verification".

### Session 2 — transport · ADR 0010 §1–2

`Accept-Encoding: gzip, deflate` with a **bounded inflate** — absolute output ceiling and maximum
expansion ratio, failing rather than truncating — plus a gzip fuzz target on the same commit. Then
connection reuse, **keyed by the ADR 0005 partition key**, with idle close through
`next_deadline_ms`.

Placed second because it is cheap, it is already decided, and every session after it is faster to
iterate on. reddit's stylesheet alone is 111KB uncompressed and 14.7KB on the wire.

**Check:** the counters show one connection per host per partition instead of one per resource, and
the byte count for reddit's front page drops by roughly the ratio in ADR 0010's table.

### Session 3 — the selectors that are silently not matching · ADR 0016 §1

`:not()`, `:is()`, `:where()`, and `:nth-child()`/`:nth-of-type()` with the full `An+B` grammar.
Specificity rules included — `:where()` contributes zero, and getting that wrong inverts the cascade.

> 188 of reddit's stylesheet's 212 misses are these four. They need no invalidation machinery: pure
> matcher work over a static tree, testable the day they land.

**Check:** a snapshot of reddit before and after, side by side. This is a colour-and-spacing session
and the diff should be obvious.

### Session 4 — `calc()`, `@supports`, `aspect-ratio` · ADR 0014 §2–3

`calc()` with lengths, percentages and the mixing rules; `@supports` answering **honestly** about
what the engine supports; `aspect-ratio`, which reddit uses 11 times for media boxes.

> `@supports` fails in the direction that produces a wrong page rather than a missing effect. A
> `@supports` that claims a property works because the parser accepted the token is the CSS version
> of ADR 0012's stub problem.

**Check:** `@supports (display: grid)` is false and `@supports (display: flex)` is true, and a
snapshot shows the fallback branch of reddit's stylesheet.

### Session 5 — JPEG · ADR 0023 §1–2

A baseline and progressive JPEG decoder, written here, with saturating size arithmetic, a maximum
decoded pixel count enforced **before** allocation, and a fuzz target on the same commit.

**Check:** reddit's front page renders its 8 JPEG thumbnails. Previously: empty boxes.

### Session 6 — picking the right image · ADR 0023 §4

`srcset`, `sizes`, `<picture>`. `loading="lazy"` waits for session 12.

**Check:** a high-density snapshot picks the 2x candidate.

### Session 7 — geometry · ADR 0015

The `GeometrySource` seam on `src/engine`; `getBoundingClientRect`, `offsetWidth`/`offsetHeight`,
`clientWidth`/`clientHeight`, and `getComputedStyle` split into the used-value and computed-value
sets. A layout-clean flag, synchronous layout on a dirty query, and the `layout.forced_by_script`
counter.

> **891 measured occurrences**, the largest single category in the survey, and structurally blocked:
> `src/bindings` may see `js` and `dom` and not `layout`. Values out, never pointers.

**Check:** a page that mutates a style and immediately reads a rect gets the post-mutation rect, and
`MICROBROWSER_PERF_COUNTERS=1` shows the forced layout.

> ### Gate A
> reddit.com loads past its challenge and paints a recognisable, styled, photographed page.
> It is not usable yet — the feed is three posts and the menus do nothing.

---

## Phase B — reddit works

The measurement that sets this phase: **reddit server-renders three posts and names 31
`/svc/shreddit/…` HTML-fragment endpoints.** Everything else arrives by `fetch`. Rendering reddit and
running reddit are the same task.

### Session 8 — the scroll model · ADR 0018 §1–4

A scroll offset per scrolling box and on the viewport; `scrollTop`/`scrollLeft`,
`scrollWidth`/`scrollHeight`, `scrollTo`/`scrollBy`/`scrollIntoView`; the **scroll blit** in the
presenter; frame-throttled `scroll` events; wheel routing with ancestor chaining; `position: sticky`
stops being a lie.

> `scrollTop` at 254 occurrences outranks `getBoundingClientRect` at 152. **A scroll is a paint, not
> a layout** — everything about its cost follows from that sentence.

**Check:** `MICROBROWSER_TRACE_REDRAW=1` shows a scroll damaging the exposed band, not the window.
A sticky header sticks.

### Sessions 9–10 — input, events and focus · ADR 0017

`PointerInputMessage` and `KeyInputMessage` replacing the three current input messages; the full
dispatch algorithm — capture, at-target, bubble, `stopPropagation`,
`stopImmediatePropagation`, `passive`, `once` — used by every event with no second path; default
actions as a separate post-dispatch step; `isTrusted` set at construction and unsettable; a focus
model with `activeElement`, `focus()`, `blur()`, Tab order, and `:focus-visible`.

Two sessions because the message-set change touches `src/ipc`, `src/app`, `src/engine` and `src/ui`,
and because `src/app` becomes the module that decides chrome-or-page for every key — which is a
security boundary and gets tests that say so.

**Check:** typing in reddit's search box works; Escape closes a menu; a page cannot type into the
omnibox.

### Session 11 — dynamic pseudo-classes and the invalidation index · ADR 0016 §2–3

`:hover`, `:active`, `:focus`, `:focus-within`, `:checked`, `:disabled`, `:target` as **element state
bits** set by the engine; the per-stylesheet invalidation index; the layout-affecting versus
paint-affecting property table.

**Check:** the property that decays silently — **a page with no `:hover` rules does not restyle, does
not relayout and does not repaint when the pointer crosses it.** That test belongs next to
`IdleWaitStrategyTests`.

### Session 12 — `IntersectionObserver`, `ResizeObserver`, lazy images · ADR 0018 §5

Both observers sampled once per frame against the scrollport, delivered as a task at end of frame,
never synchronously from a scroll. `loading="lazy"` on top.

**Check:** reddit's front page fetches the images on screen, not all 33.

### Sessions 13–14 — `fetch`, fragment parsing, CORS · ADR 0020 §1, §2, §6

`Request`/`Response`/`Headers`/`Body`, `AbortController`, all over `net::RequestQueue` with a
`privacy::Verdict` and no overload without one. **CORS enforced at the `net`/`engine` seam** — where
the network process will be — with the response *discarded*, not marked; preflight cache keyed by
partition key; opaque responses as a real type. Then the HTML **fragment parsing algorithm** with a
context element, reached by `innerHTML`, `insertAdjacentHTML`, `DOMParser` and `<template>.content`,
with a fuzz target that varies the context element.

> Fragment parsing is the highest-risk item on this roadmap: the tree builder, reachable from script,
> with attacker-chosen context.

**Check:** reddit's feed fills in past the three server-rendered posts. A menu opens.

### Session 15 — CSP, SRI, XHR · ADR 0020 §3–4

`default-src`, `script-src`, `style-src`, `img-src`, `connect-src`, `form-action`, `base-uri`,
nonces, `'self'`/`'none'`/`'unsafe-inline'`, host sources — **enforced, not logged**, and
`report-uri`/`report-to`/NEL **not sent**, because a violation report is an outbound request the user
did not cause. Subresource Integrity over the TLS stack's SHA-2. `XMLHttpRequest` over the session-13
machinery.

**Check:** reddit's nonced inline scripts run and an injected one does not. Plex's four
`integrity=`-marked resources verify.

> ### Gate B
> **reddit.com works.** The feed loads, menus open, links navigate, search accepts typing, hover
> states respond, scrolling is smooth, and the images are the right ones at the right size.
> This is ADR 0007's target 2, reached — and reached without `old.`.

---

## Phase C — youtube renders

The measurement that sets this phase: **youtube.com's 909KB document contains 421 tags, of which two
are custom elements.** The page is constructed by script into shadow trees.

### Session 16 — history and the SPA URL · ADR 0026 §1–3, §5

Session history moves from `src/ui` into `src/engine`; `pushState`/`replaceState` with the
**same-origin check** as the load-bearing line, `popstate`, `hashchange`, `history.state` stored as
structured-clone bytes; the navigation algorithm with its cancellation points and its teardown order;
then `window.navigation` over it.

> `pushState` changes what the URL bar shows without loading anything. The origin check is the only
> thing between that and a perfect address-bar spoof, and the browser process — not the renderer —
> decides what the bar displays.

**Check:** reddit's route changes update the URL bar and Back returns; the origin tests reject a
different port, a `data:` URL and a `javascript:` URL.

### Sessions 17–18 — Shadow DOM · ADR 0019

Session 17: `attachShadow` (open and closed), `shadowRoot`, the **flat-tree traversal** used by
layout and the cascade — a traversal, not a materialised second tree — then `<slot>`, assignment,
`assignedNodes`, `slotchange`, and event retargeting with `composedPath` inside the session-9
dispatch algorithm.

Session 18: the **scoped cascade** — `:host`, `:host(sel)`, `::slotted()`, inheritance crossing
boundaries while matching does not — plus `adoptedStyleSheets` and constructable stylesheets held
**by reference**, then `::part` and declarative shadow DOM.

> reddit contributes zero `attachShadow` calls and writes `slot="…"` 70 times. Both sites need this.

**Check:** youtube.com's home page shows video thumbnails and a masthead instead of two empty
elements.

### Sessions 19–20 — web fonts · ADR 0024

Session 19: `@font-face`, `src` with `format()` and fallbacks, the weight/style/stretch descriptors,
`unicode-range`, `font-display` — with `block` capped and `swap` honoured — against plain
TrueType/OpenType. Font-face matching alongside the system database.

Session 20: **brotli's decoder as a sanctioned dependency** with ADR 0010's bomb bounds and a fuzz
target, the WOFF2 container written here with its own fuzz target, container validation before
FreeType sees a byte, **hinting disabled for downloaded fonts**, and `Accept-Encoding: br` — which
closes out ADR 0010 in one line.

**Check:** youtube renders in Roboto. A `unicode-range` page fetches only the subsets its text needs.

### Session 21 — `transform` and stacking contexts · ADR 0014 §4

The property, the computed value, the display-list command over the existing `AffineTransform`, and
the stacking contexts `transform` creates — which is M6's remainder arriving because a page used it.

**Check:** youtube's hover and menu transforms position correctly; a `z-index` snapshot orders
correctly.

> ### Gate C
> **youtube.com renders.** Thumbnails, masthead, sidebar, in its own fonts, scrollable and
> clickable. No video plays.

---

## Phase D — Plex plays

The measurement that sets this phase: **`requestMediaKeySystemAccess` appears twice in the survey,
both in Plex, and nowhere else.** Plex is the MSE site and the DRM site; a user's own library is not
encrypted and plays.

### Session 22 — storage · ADR 0021 §1–4

`sessionStorage`, then `localStorage`, each taking an ADR 0005 `PartitionKey` with no overload
without one and each extending the architecture lint on its own commit. Memory by default;
**persistence is a per-site user act**, not `navigator.storage.persist()`; per-key quota with
`QuotaExceededError`; the `storage` event.

**Check:** Plex's splash-screen script finds `sessionStorage`. Signing in survives a reload within a
session. `example.com` under `a.com` and under `b.com` have different `localStorage`.

### Session 23 — `WebSocket` and `EventSource` · ADR 0020 §5

The first long-lived connections. Descriptors handed to `IdleWaitState`; **ping answered, never
originated**; both subject to `connect-src` and the partition key; both dying with the page.
`EventSource` reconnect bounded with backoff and a cap.

**Check:** Plex's UI updates when something starts playing elsewhere. An idle page with an open
socket wakes zero times.

### Session 24 — audio out · ADR 0028 §4, ADR 0013

The audio device, the ring buffer, the playback clock, and **the first thread in the engine** — with
its ownership statement written before its code: what it owns, what it borrows, who joins it. No
device when nothing is playing.

**Check:** an `<audio src="…mp3">` plays. The browser with no media has no audio thread.

### Sessions 25–26 — the media element and the demuxers · ADR 0028 §1–2

`HTMLMediaElement` with the readiness and network state machines implemented rather than
approximated, `play()` returning a promise, autoplay refused without user activation, default
controls as user-agent boxes. Then fragmented MP4, then WebM/Matroska, each with a fuzz target and
saturating size arithmetic.

**Check:** `<video src="…mp4">` plays with sound, seeks, and fires the right events in the right
order.

### Session 27 — the codec decision, and the sandbox · ADR 0013 follow-up, ADR 0028 §6

ADR 0013 deferred the codec choice until the media stack said what it needed. It now does: H.264,
VP9, AV1, AAC, Opus. **Write that ADR**, then land the chosen decoder in a sandboxed process with a
narrow message interface — never linked into the engine — and build ADR 0013's **video surface**: a
hole in the display list, composited by the presenter, never diffed.

**Check:** a 1080p video plays without the display-list diff seeing a changed command.

### Session 28 — MSE · ADR 0028 §5

`MediaSource`, `SourceBuffer`, `appendBuffer` with the real coded-frame-processing algorithm,
`buffered` that tells the truth because adaptive bitrate reads it, `endOfStream`, a quota, and the
object URL registry.

**Check:** Plex direct-plays a video from a user's own server.

### Session 29 — HLS · ADR 0028 §2

`.m3u8` playlist parsing, then MPEG-TS if Plex's transcode path needs it. reddit's front page also
references two `.m3u8` playlists, so this is not Plex-only.

**Check:** a Plex transcoded stream plays, and reddit's videos do.

> ### Gate D
> **Plex works for a user's own library.** Sign in, browse, play, with sound and seeking.
> Studio-licensed content does not play, because EME is refused — ADR 0028 §5 says why at length.

---

## Phase E — any page

Everything past here is not required by the five targets. It is what "any page" means.

| # | Session | ADR | Why |
|---|---|---|---|
| 30 | Character encodings — the WHATWG sniffing algorithm, UTF-8 with correct U+FFFD substitution, windows-1252, ISO-8859-x, UTF-16 | 0025 §2 | encoding confusion is an XSS class, not only mojibake |
| 31 | The Unicode table generator, then UAX #14 line breaking | 0025 §1, §4 | CJK text currently overflows its box; also unblocks `normalize` and `\p{…}` in `src/js` |
| 32 | The legacy multi-byte decoders — Shift_JIS, EUC-JP, GB18030, Big5, EUC-KR | 0025 §2 | |
| 33–34 | UAX #9 bidi — paragraph levels, run reordering before shaping, mirroring, the two-position caret, `dir="auto"` | 0025 §3 | right-to-left text is currently drawn backwards |
| 35 | `transition`, `@keyframes`, and the Web Animations API | 0014 §5 | after `transform`, because animating a property that does not apply gains nothing. **The loop wakes while animating and not one frame after everything settles.** |
| 36 | Canvas 2D over the existing rasterizer, with tainting on cross-origin draws | 0029 §2 | the cheapest capability per unit of compatibility on this roadmap |
| 37 | The fingerprinting answer table, permissions default-deny, and the lint that fails a new binding not on the table | 0029 §1, §5–6 | the failure mode is a binding added next year that reports the truth because nobody looked |
| 38 | Dedicated workers, structured clone, `structuredClone()` | 0022 | with the thread ownership statement; **service workers stay refused** |
| 39 | Grid | 0014 §6 | last of the layout features, on the measurement, and still real |
| 40 | Same-origin iframes — the browsing-context tree, nested layout, nested display lists, nested hit testing | 0027 §1, §6 | where every structural bug will be found, with no new security surface |
| 41 | Cross-origin iframes — `WindowProxy`, `postMessage`, the origin checks, `X-Frame-Options`, `frame-ancestors`, `sandbox`, Permissions Policy | 0027 §2–3 | |
| 42–45 | **The process split** — ADR 0004, finally: a process per site, the network process holding CORS and cookies, the sandboxed decoder process holding images and fonts and video | 0004, 0027 §5 | iframes are the client that makes it necessary rather than principled |
| 46 | Incremental parsing and first paint | 0030 | the determinism test — one chunk, byte-by-byte, random boundaries, identical display list — lands on the first commit |
| 47 | HTTP/2, with HPACK's dynamic table bounded on the decode side regardless of what the peer claims | 0010 §3 | google.com effectively requires it |
| 48 | IndexedDB | 0021 §5 | 7 occurrences and the largest implementation on the storage list; it is here because "any page" eventually means one that uses it |
| 49+ | Tabs, downloads, file upload behind a browser-process dialog returning a descriptor, printing | — | product surface, not engine |

---

## What "any page" will still not do

Written here so that the gaps are decisions rather than discoveries. Each has an ADR that argues it.

| Not supported | Why | ADR |
|---|---|---|
| **DRM video** — Netflix, Disney+, Spotify, Plex's licensed catalogue | a CDM is an unauditable binary with a device identifier | 0028 §5 |
| **Service workers**, and offline-first sites built on them | a background wakeup with a page-controlled trigger, and a supercookie | 0022 §2 |
| **WebGL / WebGPU** | no GPU requirement; revisit if that ever changes | 0029 §4 |
| **Web Audio (`AudioContext`)** | large, and not what makes a video play | 0028 §4 |
| **Federated sign-in through third-party frames and storage** | the partition key, working as designed | 0021 §1, 0027 §4 |
| **`document.write`, `eval`, `Function(source)`** | tokenizer re-entrancy; and a test says so | 0011, 0012 |
| **`:visited`** | every read-back mechanism is a history leak | existing |
| **`writing-mode`** (vertical text) | a second axis through all of layout | 0025 §3 |
| **Thai/Khmer/Lao/Burmese line breaking** | needs dictionary segmentation | 0025 §4 |
| **`Intl`** | where ICU actually earns its size | 0025 §1 |
| **Geolocation, camera, microphone, clipboard read** | default deny, no prompt | 0029 §5 |
| **Back/forward cache** | a lifetime model nobody can hold in their head, for a second-visit win | 0026 §3 |
| **`SharedWorker`, `SharedArrayBuffer`, `Atomics`** | shared mutable state; needs cross-origin isolation | 0022 §1, 0012 |

## Three things that would prove this roadmap wrong

Written so that being wrong is detectable rather than arguable.

1. **A gate is reached and the site still does not work.** The gates are stated as user-visible
   outcomes for exactly this reason. If Gate B lands and reddit's feed is still empty, the survey
   missed something and the next step is another survey rather than another session.
2. **A session in Phase A or B turns out to be a phase.** The early sessions are sized from reading
   the code and the specifications, not from doing the work. The geometry seam (session 7) and the
   input refactor (sessions 9–10) are the two most likely to be underestimated.
3. **The counts were measuring the wrong thing.** The survey counts textual occurrences in minified
   bundles, which over- and under-counts in ways it documents. If `:not()` lands and reddit looks
   unchanged, that method is worth less than ADR 0014's experience suggested, and every ordering
   decision downstream of a count should be re-examined.

## Related

- `docs/surveys/2026-08-04-reddit-youtube-plex.md` — every number cited here
- `docs/adr/0007-compatibility-targets.md` — why these five sites
- `docs/adr/0015` … `0030` — the decisions this document sequences
- `AGENTS.md` — the priority order and the invariants every session is held to
