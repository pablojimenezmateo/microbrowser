# ADR 0029 — Canvas, WebGL, permissions, and what this browser is allowed to say about the user

**Status:** accepted · **Date:** 2026-08-04

## Context

Four things in the survey look unrelated and are one problem:

| | occurrences |
|---|---|
| `navigator.*` | 142 |
| `matchMedia` | 52 |
| `devicePixelRatio` | 28 |
| `WebGL` | 25 |
| `getContext('2d')` | 24 |
| `clipboard` | 31 |
| `Notification` | 13 |
| `crypto.getRandomValues` | 8 |
| `geolocation` | 5 |

They are one problem because each is a question a page asks about the machine, and the answers
compose. A browser that reports its exact viewport, its device pixel ratio, its installed fonts, its
platform, its language, its colour scheme, its timezone and a canvas rendering is uniquely
identifiable without a cookie — and none of the individual answers looked like a leak when it was
implemented.

ADR 0012 has one line about this and it is the right line, but it is only a line:

> **Anything that reports on the user.** Where a capability's honest answer is a fingerprinting
> surface, the decision is made in `guidelines/privacy.md` terms and not by what a page expects.

`:visited` matching nothing is the existing precedent, and it is a good one — a decision, written
where the code is, with the mechanism explained. This ADR generalises it, because the number of
places that will need it is now large enough that deciding each one locally guarantees an
inconsistent answer.

There is also a real feature question mixed in. `getContext('2d')` at 24 and WebGL at 25 are close
enough that "implement the popular one" does not decide anything, and they have wildly different
costs.

## Decision

### 1. The governing rule: every copy of this browser answers identically

**Where a page asks about the machine, the answer must be one that every copy of this browser would
give.** Not randomised, not perturbed — *constant*.

That is already the decision `src/util/UserAgent.h` records for the user-agent string ("every copy of
this browser sends the identical bytes, so the header carries zero bits about the user") and the one
`Accept-Language: en-US` records for language. This ADR makes it the rule rather than two instances
of a pattern.

Randomisation is explicitly rejected as the mechanism, and the reason is worth keeping: a jittered
answer is still an answer, it is *distinguishable as jittered*, and repeated sampling averages it
away. Meanwhile it breaks every honest consumer — ADR 0015 makes the same argument for geometry, and
891 call sites is what breaking it costs.

Where a constant answer is impossible because the truth is genuinely needed for rendering — the
viewport size, the device pixel ratio — the mitigation moves to **reducing the entropy of the truth**:
quantise the reported viewport, and pin the device pixel ratio to a small set. That is a real cost in
sharpness on unusual displays and it is the trade this project's priority order selects.

### 2. Canvas 2D: implemented, and it is nearly free

`getContext('2d')`: paths, fills, strokes, transforms, clipping, `drawImage`, `fillText`, and
compositing operators.

The reason it is on the list ahead of things with higher counts is that **`src/gfx` already is a 2D
canvas.** There is an analytic-AA rasterizer, a stroker, a path implementation, affine transforms,
image scaling and a text shaper. Canvas 2D is a binding over machinery this project built for its own
painting, which makes it one of the cheapest capabilities per unit of page compatibility on the whole
roadmap.

It also lands `OffscreenCanvas`'s reason for existing nowhere near it, and `ImageData` with
`getImageData` / `putImageData`, which is where the fingerprinting comes in.

**Canvas readback is the classic fingerprinting vector**, because text rendered to a canvas and read
back differs by font, rasterizer and platform. Under the rule in §1 the answer here is unusually
comfortable: this browser has **one rasterizer, one text shaper, and a font stack it controls**, so
two copies of it on different machines produce the same pixels for the same input — *provided* the
fonts available are the same. That makes §3 a prerequisite for this being true rather than a separate
concern, and it is why they are in one ADR.

`getImageData` on a canvas that has drawn a cross-origin image without CORS taints it and throws, per
the specification. That check is a security boundary, not a privacy one, and it is enforced at the
draw rather than at the read.

### 3. Fonts are the entropy that matters most, and the answer is the shipped set

The installed font list is among the highest-entropy signals available to a page, and it is readable
three ways: by measuring text (ADR 0015), by canvas readback (§2), and directly if a font-enumeration
API exists.

**The Local Font Access API is not implemented.** `navigator.fonts` is absent.

And the deeper answer, which ADR 0024 gestured at: **a page can only observe the fonts this browser
offers it**, which is the shipped set plus whatever the page itself supplies via `@font-face`. A
font-family that names a system font not in the shipped set does not match. That is a rendering
difference from other browsers — a page asking for a locally-installed corporate font gets a
fallback — and it converts the highest-entropy signal on the platform into a constant.

Whether the shipped set is bundled with the browser or selected from the system database at build
time is an implementation question; what this ADR fixes is that **what a page can see is not a
function of what the user has installed.**

### 4. WebGL: refused, and it is a close call stated as one

`WebGLRenderingContext` and `WebGL2RenderingContext` are not defined. 25 occurrences, and every one
of them is behind a capability check.

Three reasons, and the honest admission that the first is the decisive one:

- **There is no GPU requirement.** `AGENTS.md`'s mission is a browser with no GPU requirement and a
  software rasterizer. Implementing WebGL means either a GPU dependency — which is a much larger
  decision than this ADR — or a software GL implementation, which is a rasterizer of a completely
  different shape from the one that exists.
- **Attack surface.** WebGL is the most productive source of GPU-driver vulnerabilities in browsers,
  because it hands shader source and buffer geometry from a web page to a driver written in C.
- **Fingerprinting.** `WEBGL_debug_renderer_info` returns the GPU model. Even without it, rendering
  differences between drivers are a strong signal.

The close-call part: WebGL is used for genuinely useful things — maps, visualisations, games — and
refusing it excludes a real category of site permanently. It is refused anyway, and the condition
under which it should be revisited is explicit: **if this project ever takes a GPU dependency for
compositing, WebGL becomes a question about attack surface alone rather than about architecture, and
the answer might change.**

`WebGPU` is refused on the same reasoning, more easily.

### 5. Permission-gated capabilities: default deny, no prompt

`Notification`, `geolocation`, `clipboard` read, `getUserMedia`, and the Permissions API over them.

**The default is deny, the permission state is reported honestly, and there is no prompt.**

Prompting is rejected as a mechanism, not just deferred. A prompt on a capability the user did not
ask for is a decision the user is unequipped to make in the moment, and every study of them says the
answer is fatigue. `guidelines/privacy.md`'s stance and `AGENTS.md`'s "no feature that cannot be built
without weakening one of these" both point the same way.

What that means per API:

- **`Notification.permission === "denied"`**, `requestPermission()` resolves `"denied"`. The
  constructor exists and does nothing visible. This is the one place this ADR ships something that
  looks like a stub, and it is defensible precisely because the *specification* defines a denied
  state with exactly this behaviour — the page is not being misled, it is being told no in the
  vocabulary the API has for no.
- **`navigator.geolocation` is absent entirely.** There is no location to report and no denied-state
  that a page handles better than absence.
- **Clipboard *write* on a user gesture is allowed** — copy buttons are common and the user pressed
  one, so `isTrusted` (ADR 0017) is the gate. **Clipboard read is refused**: it is a page reading
  data the user copied from somewhere else.
- **`getUserMedia` is absent.** A browser that cannot play video is not a browser that should be
  capturing it.
- **The Permissions API** reports these states truthfully, which makes it useful rather than
  decorative.

### 6. The specific answers, written down so they are consistent

| What a page asks | What it gets |
|---|---|
| `navigator.userAgent`, `appVersion`, `platform`, `vendor` | the one constant from `util::UserAgent` |
| `navigator.language`, `languages` | `en-US`, always |
| `navigator.hardwareConcurrency` | a constant, not the core count |
| `navigator.deviceMemory` | absent |
| `navigator.plugins`, `mimeTypes` | empty |
| `navigator.doNotTrack`, `globalPrivacyControl` | absent — a preference signal is itself a bit |
| `navigator.connection` | absent |
| `navigator.getBattery` | absent |
| `devicePixelRatio` | quantised to a small set |
| `screen.*` | the viewport, not the display |
| viewport size in `matchMedia` and geometry | quantised |
| `matchMedia('(prefers-color-scheme)')` | the user's setting — one bit, and worth it |
| `matchMedia('(prefers-reduced-motion)')` | the user's setting — one bit, accessibility |
| `Intl.DateTimeFormat().resolvedOptions().timeZone` | UTC unless the user chooses otherwise |
| `Date` and `performance.now()` | real, at reduced resolution |
| `crypto.getRandomValues` | real randomness, from the system |

The two `prefers-*` rows are the deliberate exceptions and they show the shape of the trade: each is
one bit of entropy, each materially changes whether a page is usable or comfortable, and paying one
bit for that is a better deal than paying it for `deviceMemory`.

`performance.now()` at reduced resolution is a security measure rather than a privacy one — high
resolution timers are what turn cache and speculative-execution side channels into practical
attacks — and it belongs on the same table because the mechanism is the same.

## Consequences

- **A table of answers exists and must be kept consistent.** The failure mode of this ADR is a new
  binding added next year that reports the truth because nobody looked here. It is worth a lint: a
  binding that reads platform state and is not on this table fails the build.
- **Canvas 2D arrives cheaply and unlocks a category of page** — charts, image editors, games —
  disproportionate to its 24 occurrences.
- **Refusing WebGL is permanent-looking and is not.** The revisit condition is written above.
- **Quantising the viewport is a visible rendering compromise.** A window of an unusual width lays
  out as if it were a common one, which can mean a scrollbar or a letterbox. That is a real cost and
  it should be measured on a real page before the quantisation step is chosen.
- **This browser is still fingerprintable**, and claiming otherwise would be the marketing language
  `AGENTS.md` forbids. Timing, feature support, and the mere fact of being this browser are all
  signals. The goal is that the *user* is not distinguishable from other users of this browser, which
  is achievable, rather than that the browser is invisible, which is not.

## Alternatives considered

**Report the truth everywhere and treat fingerprinting as out of scope.** Rejected on the privacy
contract, which ranks third overall and above speed. It is also the path of least resistance and
therefore the one that happens by default if this ADR does not exist.

**Randomise the answers — a different canvas hash per session, jittered timings, noise on
geometry.** Rejected in §1. It breaks honest consumers, it is detectable, and repeated sampling
defeats it. It is the most commonly deployed anti-fingerprinting technique and this ADR declines it
deliberately.

**Implement WebGL over the software rasterizer.** Rejected on scope. A conformant software GL
implementation is a project of comparable size to this browser's rasterizer, for a capability 25
sites reference behind a feature check.

**Prompt for notifications and geolocation, like every other browser.** Rejected on prompt fatigue,
and because it puts a security decision in front of a user at the moment they are least equipped to
make it. A user who genuinely wants a site to notify them is better served by a per-site setting on
the same shelf as ADR 0021's persistence — which is a mechanism this project has now chosen twice and
should keep choosing.

**Ship `doNotTrack: "1"`, since this browser does mean it.** Rejected, and it is a nice illustration
of the rule: a browser that sends a privacy preference is distinguishable by that preference. The
preference is expressed by not sending the data, which needs no header.
