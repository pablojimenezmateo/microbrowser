# Session log

Append-only. One entry per roadmap session, newest last. The commit history says what changed;
this says **what the next agent needs and could not derive from a diff** — what was tried and
rejected, what the check actually printed, what turned out to be wrong.

Written by `/next-session` (see `.claude/commands/next-session.md`). The state it reads is
`docs/roadmap-sessions.json`; the argument behind that state is `docs/roadmap-to-any-page.md`.

Template:

```
## Session <n> — <title> · <date>

**Status:** done | in_progress
**Check:** what you ran, and what it actually printed — not "passed".
**Landed:** the commits, by subject.
**Left:** what the next agent inherits.
**Found:** anything that contradicts the roadmap, an ADR, or CLAUDE.md.
```

---

## Session 0 — the harness itself · 2026-08-04

**Status:** done
**Check:** n/a — this session built the loop rather than a browser feature.
**Landed:** `docs/roadmap-sessions.json` (the 49 roadmap sessions as state),
`.claude/commands/next-session.md` (the per-session prompt), `tools/agent-loop.sh` (the
fresh-context driver), this file.
**Left:** Session 1 is `in_progress`. As of `b5f5c11`, `DOMContentLoaded`, `readyState`,
`URLSearchParams` and `location.search` have landed; `document.forms`, `form.elements`,
`namedItem()` and `requestSubmit()` have not.
**Found:** The ledger omits roadmap rows 43–45 (folded into 42, the process split) and 49+
(tabs, downloads, upload, printing — product surface, not engine). Phase E rows carry no `check`,
because the roadmap states none; writing one is the first deliverable of each of those sessions.

## Session 1 — the door · 2026-08-04

**Status:** done

**Check:** `./build/microbrowser/microbrowser_snapshot https://www.reddit.com/ -o out.ppm` printed

```
https://www.reddit.com/?solution=c5db4a57…&js_challenge=1&token=7afd7253…&jsc_orig_r=:
  333 commands, 101 runs, 4 fonts, 8 images, title "Reddit - The heart of the internet"
```

The URL in that line is the point: the interstitial's script filled in its own form and the
browser navigated to the answer. Before this session the same command produced
"Please wait for verification".

**Landed:**

- *Three percent-decoders become one, and the form serializer becomes correct*
- *location tells the truth about a URL, and URLSearchParams exists*
- *An assignment to a DOM property reaches the element it describes*
- *The door: a script fills in a form and the browser navigates*

**Left:** `www.reddit.com` renders but its own scripts stop on
`ReferenceError: PerformanceObserver is not defined`, thrown from the inline telemetry bundle.
Session 7 (geometry) is the next scheduled one; this is a smaller thing beside it.

**Found**, and none of it was in the roadmap or the survey:

- **`Object.assign` was writing slots, not invoking setters** — a bug in `src/js`, not in the
  bindings. `Object.assign(document.createElement('input'), {name, type, value})` is precisely the
  shape the challenge is written in, and it set three properties on the wrapper and produced an
  element with no attributes. It also meant a proxy's `set` trap was skipped by `assign`.
- **Every reflected DOM attribute was missing or getter-only.** `el.id = 'x'` was a silent no-op:
  the write succeeded, read back, and described nothing. A class set that way never reached the
  cascade.
- **`location.pathname` was everything after the host**, so `location.search` was `undefined` and
  `new URLSearchParams(undefined)` is an empty, useful-looking parameter set.
- **A bare `addEventListener('load', f)` registered on nothing.** No receiver, sloppy-mode `this`,
  and `call.self` arrives undefined — so half the pages that listen for `load` were never told.
- **`readyState` answering "complete" always was the wrong half of the trade.** It was chosen so a
  page would not wait for a `DOMContentLoaded` that had already happened; the cost was that the
  pages which *only* listen waited forever.
- **`document.forms` installed on the document's wrapper landed on an object nothing could reach.**
  `EnsureInterfaces` runs from the *first* `WrapperFor`, so asking for the document's wrapper inside
  it builds a second one and the outer call caches the first. Anything installed from
  `EnsureInterfaces` belongs on an interface prototype.
- **The click and Enter submission paths fired no `submit` event at all.** Not a session-1 item on
  paper; it is the same one line of the specification seen from the other side, and all three routes
  now go through one `Page::SubmitForm`.
- Three percent-decoders existed — `url`'s, `engine`'s, and the one `bindings` was one commit away
  from adding. They are one now, in `util`.

## Session 2 — transport · 2026-08-04

**Status:** done

**Check:** `MICROBROWSER_PERF_COUNTERS=1 ./build/microbrowser/microbrowser_snapshot
https://old.reddit.com/ -o /tmp/reddit.ppm` printed

```
[counters]             505859  net.bytes_coded
[counters]            2161404  net.bytes_decoded
[counters]             697254  net.bytes_received
[counters]                 20  net.connections
[counters]                 40  net.connections_pooled
[counters]                 20  net.connections_reused
[counters]                 40  net.fetches
[counters]                 20  net.tls_handshakes
```

Before this session the same page was **40 connections and 40 TLS handshakes for 40 fetches**,
with every byte uncompressed. The identity-equivalent transfer is
`bytes_received - bytes_coded + bytes_decoded` = 2,352,799 against 697,254 on the wire: **3.37x,
70% saved**, against the 79% in ADR 0010's table. The gap is that reddit's front page is partly
images, which were already compressed and gain nothing.

Sanitizers: `tools/run-checks.sh asan` and `ubsan` both 100% of 24 shards, after a real fix — see
below.

**Landed:**

- *gzip is a DEFLATE stream with a header, and the header is the hostile part*
- *Accept-Encoding stops lying, and a body may not grow without a bound*
- *A connection outlives the request that opened it, and belongs to one partition*
- *A test connection may now outlive the factory that made it, and says so*

**Left:** ADR 0010 §3 — HTTP/2 and ALPN — is untouched and still the ADR's own third priority.
Session 3 (the selectors that are silently not matching) is next on the ledger.

Two things a next agent should know before touching this code:

- **An idle pooled connection is not in the loop's wait.** It has a deadline and no descriptor, so
  a server that closes one while it sits idle is discovered by writing into it, not by being woken.
  That is what the one retry in `FetchRequest::MayRetryOnFreshConnection` is for. Putting idle
  sockets in the wait would let the browser notice sooner, at the cost of a wakeup per close.
- **`Engine::NextDeadlineMs` is no longer gated on `load_.active`.** It had to stop being: the case
  that matters for the idle timeout is precisely the one with nothing loading, and the old gate
  would have held a socket open until the next navigation happened along.

**Found:**

- **The check's own wording is not reachable, and the reason is the previous session.** "One
  connection per host per partition" cannot hold while six requests per partition run concurrently
  — six concurrent requests to one host need six connections under HTTP/1.1. 40 → 20 is what
  reuse is worth here; on Hacker News, a single host with five fetches, it is 5 → 4. Reuse pays
  where requests are *serialized* — a document then its subresources, a redirect chain, the click
  after a page — and concurrency is what limits it. Read the check as "far fewer than one per
  resource".
- **A gzip bomb can be refused before a single byte is produced.** The member declares its own
  uncompressed size in ISIZE, and a claim above the bound cannot be within it. This was not in the
  ADR, which describes the bound as something the decoder enforces while running. It is the one
  direction an attacker's number cannot be made useful in — lying the other way is caught by the
  checksum — and it is why `DecodeStatus::TooLarge` can mean something exact.
- **The decoder cannot say whether it stopped because of the bound.** It fails on the back
  reference that *would* exceed the ceiling, so its output length is short by an arbitrary amount
  and says nothing. The first version of this distinguished "too large" from "malformed" by
  comparing output size against the bound, and it was wrong for exactly that reason — the test
  that caught it was the one asserting a 132-byte bomb is refused. A status that depends on which
  byte a decoder happened to stop at is not a status.
- **ASan and UBSan both failed 18 of 24 shards on the pooling commit, at the same line, and the
  test suite was green.** `~ScriptedTransport` reaching into a `Factory` already destroyed: a
  pooled connection outlives the request that opened it, so in a test that declares
  `ScriptedFactory factory;` *after* the `Session` holding the engine, the factory dies first and
  the pool's idle connections die second. Production ordering was already right — `Loader` declares
  `sockets_` before `queue_` — so this was the test support, but a support class that works only
  when two locals are declared in the right order is a trap. It now detaches its connections when
  it dies. **The lesson is the sanitizers**: an ownership change that reaches past a lifetime is
  exactly what a green suite does not see.
- **`Connection: close` was hiding a second decision.** Removing it means a response's framing has
  to be exactly right, because the connection no longer ends the message — which is why
  `ResponseParser` grew `BodyWasSelfDelimiting`, `Leftover` and `NothingReceived`. A body delimited
  by the close cannot travel on a kept connection at all, and a byte past the end of a message is
  request smuggling with one browser playing both parsers.
- **`net.bytes_coded` alongside `net.bytes_decoded` is what makes the check reproducible from a
  single run.** Without the input side, "how much did compression save" needs a build of the
  previous commit to answer. It cost one line.

## Session 3 — the selectors that are silently not matching · 2026-08-05

**Status:** done

**Check:** the ledger asks for "a snapshot of reddit before and after, side by side… the diff
should be obvious." It is not obvious, and the reason is a finding rather than a failure — see
below. Three measurements, taken against the pre-session binary built from `9ee2a2b` in a git
worktree:

1. **Rules that now parse, deterministic.** Old reddit's ten stylesheets as served
   (`reddit.Gsb42QVNY6g.css` and nine others, 368KB total), parsed directly by
   `ParseStyleSheet` linked against each build:

   ```
   BEFORE: rules=3127 skipped=122
   AFTER : rules=3153 skipped=96
   ```

   +26 rules, −26 skipped, and **nothing lost**. On a wider sample — thirteen sheets from
   github, wikipedia, MDN and Hacker News — `233/72` became `250/55`, +17.

2. **A visible reddit diff, where the feature is actually used.**
   `https://old.reddit.com/r/television/comments/1vbtdij/…` renders the spoiler markup that
   `.md .md-spoiler-text:not(.revealed){background:#4f4f4f;cursor:pointer;color:transparent}`
   is for. Before: the spoiler text reads plainly. After: it is gone, as the author intended.
   A threshold difference mask over the two snapshots picks out exactly three words — the two
   `Spoiler`s and the `Television` inside a spoiler span — plus vote counts that drifted
   between the two fetches.

3. **A more obvious diff on a page that uses them heavily.** `en.wikipedia.org/wiki/CSS`:
   125,612 pixels differ, because the "Jump to content" skip link is now hidden until focused
   and the whole page moves up with it.

`tools/run-checks.sh tests`, `asan` and `ubsan`: 100% of 24 shards each. `css_fuzzer` over the
corpus plus five new seeds for this grammar: 774,879 runs in 91 seconds, no crash.

**Landed:** *A selector list may hold a selector list, and `:where()` is worth nothing.*

**Left:**

- `:has()` is the fifth selector of ADR 0016 §1 and is not implemented; it parses as a failure,
  so a rule using it is dropped exactly as before. The ADR prices it separately and behind a
  measurement, and this session did not take that measurement.
- `:nth-child(An+B of S)` is likewise not implemented and drops its rule. It appears zero times
  in the 23 stylesheets measured.
- **`background` on an inline box is not painted.** Found while looking at the spoiler fix: the
  rule applies — `color: transparent` took effect — but no `FillRect` with `#4f4f4f` appears in
  the display list, so a spoiler is invisible text on white rather than a grey bar. That is a
  paint gap, not a selector gap, and it is on no session's list.
- **`Selector::Matches` recurses once per compound and is not bounded.** Parsing is now bounded
  at eight levels of nesting, but a selector with a hundred thousand descendant combinators
  would still recurse a hundred thousand deep at *match* time. Pre-existing, and unreachable
  from `CssFuzzer` because the fuzzer parses without matching.

**Found:**

- **The 212 misses ADR 0016 counts are `www.reddit.com`'s stylesheet, not `old.reddit.com`'s.**
  The ADR's table (`:not(` 136, `:is(`/`:where(` 52) is from `styles-css-CSasxzfw.css`, the new
  reddit bundle, which is behind the JavaScript challenge that Phase A of the roadmap exists to
  get past. Counted directly, old reddit's sheets use these features **43 times**: 31 `:not(`,
  8 `:nth-child(`, 1 `:nth-of-type(`, and **zero** `:is(` or `:where(`. So the session's check
  as written could not have produced an obvious diff on the page it names, and the next agent
  should read a check that says "reddit" as "which reddit".
- **Most of the 43 gate a state the page is not in.** Collapsed comments, revealed spoilers,
  the traffic table, `body:not(.loggedin) .give-gold-button` (whose target is injected by
  script that never runs). +26 rules parsing is the honest measure; +26 rules *visibly firing*
  is not what happened.
- **Counter-based before/after on reddit is worthless, and it took two rounds to see why.**
  `css.rules_parsed` on the same comments page with the same binary gave 7,599 then 12,981;
  `css.tokens` gave 181,494 then 306,207. `css.sheets_parsed` was 90 both times. The cause is
  that `Tokenize` is also reached from `ParseSelectorList` and `ParseDeclarationList`, so every
  `querySelector` and every `style=""` assignment a script makes lands in those counters — and
  reddit's scripts die at a different point on every load (the masked `r is not defined`). The
  same comparison on Hacker News is bit-identical across runs. **A counter that a page's
  scripts can move is not a measurement of the parser.** Parsing the sheets directly, out of
  process, is.
- **`2n-1` and `n-1` are one token each, not three.** A CSS name may contain a hyphen and a
  digit, so `2n-1` tokenizes as a dimension whose *unit* is `n-1`. Every implementation splits
  that back apart by hand, and it is why `ParseNSuffix` exists. `2n- 1` is a third spelling
  again — unit `n-`, then a signless integer.
- **`Token` had thrown away the one bit this grammar needs.** `2n 1` is not a selector and
  `2n +1` is; the difference is an explicit `+` that the tokenizer discarded because the value
  is the same either way. One bool on `Token`, and the alternative was accepting an invalid
  selector — which applies a rule to elements nobody named.
- **The line cap pointed at a real missing module.** `StyleSheet.cpp` reached 1,224 lines and
  splitting it three ways was not busywork: `SelectorMatch.cpp` is now a pure function of
  (element, selector) that cannot see a token, which is exactly the shape ADR 0016 §2 needs
  when it puts `:hover` on the element rather than in the matcher.
- **`:not()` of an unimplemented pseudo-class is now true, and that is a decision.** It is right
  for `:hover`, `:active`, `:focus` and `:visited`, whose honest value in a static snapshot is
  false — reddit's video controls say `:not(:hover):not(:active)` and mean the resting state.
  It is wrong for `:checked` and `:disabled`, whose value is derivable from the DOM and is not
  being derived. Session 11 is where that stops being wrong. A test records it either way, so
  the day it changes, something says so.

## Session 4 — `calc()`, `@supports`, `aspect-ratio` · 2026-08-05

**Status:** done

**Check:** the first half ran verbatim and passed:

```
(display: grid)                                            -> false
(display: flex)                                            -> TRUE
((-webkit-mask-image:none) or (mask-image:none))           -> false
not (((-webkit-mask-image:none) or (mask-image:none)))     -> TRUE
(width:round(1.5px,1px))                                   -> false
(word-break:break-word)                                    -> false
not selector(:focus-visible)                               -> TRUE
(aspect-ratio:1 / 1)                                       -> TRUE
(text-decoration:underline dotted)                         -> false
```

Every one of those but the first two is a condition **wikipedia actually writes**, and every
answer is the honest one.

The second half — "a snapshot shows the fallback branch of reddit's stylesheet" — was
**amended, and the amendment is the session's main finding.** old.reddit.com's ten stylesheets
(318KB of them) contain **zero `@supports`, zero `aspect-ratio`, and one `calc(125%)`**. There is
no fallback branch there to show. The check was run against `en.wikipedia.org/wiki/CSS` instead,
whose main sheet has 76 `@supports`, 170 `calc(` and 2 `aspect-ratio`:

```
BEFORE (cf98a57): rules=959 skipped=226
AFTER           : rules=976 skipped=209
```

+17 rules, and a snapshot diff of **1517 pixels** across rows 125–160 — the "98 languages"
control, which wikipedia sizes inside 25 `@supports not ((-webkit-mask-image:none) or
(mask-image:none))` blocks. That is literally the fallback branch: this engine has no
`mask-image`, so the `not` side is the side that applies, and before this session neither side
did.

old.reddit.com is unchanged, and the way that was established matters: **two fetches with the
same binary differ by 1235 pixels; the two binaries differ by 912.** The page's own vote counts
move more than the session did. A pixel diff against a live page is not a measurement unless
you take the same-binary diff first.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. `css_fuzzer` over the corpus
plus fourteen new seeds for these three grammars: 245,744 runs in 121 seconds, no crash.

**Landed:**

- *A length may be two terms, and `calc()` is where the second one comes from*
- *`@supports` answers by trying the declaration, because a list would drift*
- *A box may state its shape before it has anything in it*
- *A `calc()` is one component of a shorthand, not one per space inside it*

**Left:**

- **`min()`, `max()` and `clamp()` are the only reason a wikipedia `calc` still fails.** 45 of
  its 54 `calc` declarations now apply; of the nine that do not, eight need `max()` and one
  needs `vh`. They are the obvious next hour of work on this and were not in the session's
  scope.
- **The viewport units do not exist** — `vw`, `vh`, `vmin`, `vmax`. They need a viewport size in
  the cascade, which is not there, and `@supports (width: 1vw)` honestly says no.
- **`aspect-ratio: auto <ratio>` is refused on purpose.** It means "the element's own ratio, and
  this one if it has none", and nothing here can ask an image for its ratio apart from its size.
- **`@supports` counts toward `StyleSheet::skipped` when its condition is false**, the same as a
  non-matching `@media`. That is consistent but it means `skipped` no longer measures only "we
  did not understand this" — a correctly-evaluated false condition is in there too.

**Found:**

- **ADR 0014's numbers are youtube's stylesheet, and the roadmap's check named reddit.** This is
  session 3's finding a second time, on a different feature: the ADR counts `calc(` 550 and
  `@supports` 425 in *youtube.com's 3.5MB sheet*, the roadmap turned that into a check about
  reddit, and reddit uses these features once between them. `www.reddit.com` still returns the
  8KB JavaScript challenge, so the sheet those numbers would apply to cannot be fetched to
  compare. **The next agent should read every remaining check that names a site as "which
  page, measured how".**
- **`ApplyDeclaration` returned `void`, and that was the whole difficulty of `@supports`.** A
  value it did not recognise was indistinguishable from one it applied. Making it return whether
  the declaration took is what makes the answer impossible to drift from the implementation —
  and it found two places where an invalid declaration had a side effect anyway: `text-align:
  bogus` reset `centers_block_children`, and `background-position: 5px nonsense` took the
  nonsense as zero.
- **The whitespace splitter was the real `calc()` blocker, not the evaluator.** There were two
  copies of it, one per translation unit, and neither knew what a parenthesis was — so
  `margin: calc(4px + 6px) 0` split into five components and every shorthand here rejects the
  wrong number. The evaluator worked on the first build; **38 of wikipedia's 54 `calc`
  declarations applied until the splitter was fixed, then 45.** The two copies also disagreed
  with each other: the flex one did not treat `\r` or `\f` as whitespace.
- **The `var()` substitution has to happen before you can measure `calc()` at all.** Counting
  `calc` declarations straight out of `ParseStyleSheet` reports 8 of 57 applying, because the
  values still read `calc(var(--font-size-medium,1rem) + 4px)`. Through `SubstituteVars` first,
  as the resolver does, it is 45 of 54. A measurement of the parser that skips the cascade's own
  first pass is measuring a string no element ever sees.
- **`calc(100% - 1em)` is refused, and that is a decision.** A `Length` carries one relative term
  plus an absolute offset; two relative terms would need a third float on a type four of which
  sit in every `Edges`. Rounding one term away would have been worse than dropping the
  declaration, so it is dropped. It did not appear once in the sheets measured.
- **`@supports` is where a wrong "yes" is worse than a wrong "no".** Wikipedia writes both
  branches of the mask-image test, so an engine that claimed `mask-image` would have taken the
  branch that assumes it works and drawn nothing. The `<general-enclosed>` rule — an unknown
  function form is unknown, and unknown reads as false — falls the same way on purpose.

## Session 5 — JPEG · 2026-08-05

**Status:** done
**Check:** "reddit's front page renders its 8 JPEG thumbnails. Previously: empty boxes."
Run against `https://old.reddit.com/` with `microbrowser_snapshot` and `MICROBROWSER_PERF_COUNTERS=1`:

```
BEFORE (7fc5531): engine.images_loaded 6    engine.images_failed 20
AFTER            : engine.images_loaded 24   engine.images_failed 1
                   gfx.jpeg_decodes 18       gfx.jpeg_decode_failures 0
                   gfx.jpeg_pixels_decoded 307440
```

Rendered both PPMs and looked at them. Before: every story is a bare title with an empty
left column. After: the thumbnails are photographs. The one remaining failure is the single
GIF on the page, which is the session after next.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. `jpeg_fuzzer` over the
seventeen checked-in seeds: **373,084 runs across three sessions totalling 21 minutes, no
crash**; the corpus grew to 414 inputs and 884 coverage edges and stopped finding new ones.

**Landed:**

- *A bound on decoded pixels belongs to the image, not to one decoder*
- *JPEG, baseline and progressive, and the fuzzer that lands with it*
- *Exactly one decoder is offered an image, and reddit's thumbnails arrive*
- *Two million signed differences do not add up inside an int*

**Left:**

- **GIF is the one image on reddit's front page that still fails**, and ADR 0023 §5 puts it
  third, after `srcset`. Nothing about this session changes that ordering.
- **`Accept` is still not sent at all.** ADR 0023's consequence says the browser should send
  `image/png, image/gif, image/jpeg, image/svg+xml, */*` and honestly omit `image/webp`.
  It cannot be written yet: `net::FetchOptions` carries no request destination, so one header
  builder serves documents, stylesheets, scripts and images alike, and an image `Accept` on a
  document request would be worse than none. Today's silence happens to have the intended
  effect — see Found.
- **Four-component (CMYK/YCCK) and 12-bit JPEG are refused, deliberately.** So is arithmetic
  coding. Each returns an error rather than a guess; ADR 0012's rule, applied to pixels.
- **EXIF orientation is not applied**, and belongs where an image is placed rather than where
  it is decoded.
- **The IDCT is the separable float definition with a zero-row shortcut**, not a fast integer
  form. It is the obvious thing to measure if a photographic page feels slow; nothing has been
  measured yet, because a 140-pixel thumbnail is 300 blocks.

**Found:**

- **ADR 0023's count is stale in the direction that mattered.** It says reddit's front page
  references 25 PNG and 8 JPEG. Today it is 26 `<img>` elements of which about twenty are
  JPEG, and **three of them are JPEG bytes at a URL ending `.png`** — reddit's preview service
  takes `format=jpg` in the query and leaves the extension alone. A decoder chosen by
  extension, or by the `.png` in the path, would have shown three empty boxes and no error.
  The magic-number rule ADR 0023 §2 states is not theoretical.
- **We get JPEG from reddit because we send no `Accept` header at all.** Every preview URL
  carries `auto=webp`, which means "WebP if the client says it takes it". Sending nothing is
  read as taking nothing special, so the JPEG arrives. That is the outcome ADR 0023 wanted
  from an honest `Accept`, reached by silence rather than by honesty, and it will stop being
  true the moment anything else needs an `Accept` header.
- **Nearest-neighbour chroma upsampling is what makes a decoder test worthless.** Measured
  against libjpeg on the same files it costs a mean of **4.3 levels per channel** and a worst
  case of 53. A tolerance loose enough to accept that is loose enough to accept a chroma plane
  that is off by an entire row, which is the bug a subsampled JPEG decoder actually has. With
  a triangle filter — the same one libjpeg uses, written as general bilinear with the sample
  centres half a pixel in — the difference falls to **0.44 mean and 3 worst**, and the test
  bound is now tight enough to be worth running. The filter was written to make the test
  possible, not the other way round.
- **A fixture with a discontinuity in it measures the filter, not the decoder.** The first
  subsampled fixtures used `(x * 17 + 8) % 256`, whose wrap is a 241-level cliff. A chroma
  plane at half resolution cannot represent a cliff, so every upsampling method disagrees
  there by the size of the cliff. `tools/make-jpeg-fixtures.py` now has two generators and
  says which is for which.
- **A restart interval that divides the MCU count exactly produces no restart markers.** The
  first "restarts" fixture was 32x24 at 4:2:0 — four MCUs, interval four — and contained zero
  RST markers. It passed, and it tested nothing. Counting the markers in the generated file
  is the only way to know a fixture is the fixture it claims to be.
- **The DC predictor was a signed overflow and neither the fixtures nor five minutes of
  fuzzing found it.** It accumulates one difference per block; each fits in sixteen bits and
  a component may hold two million blocks. Reaching it needs a file specifically built to,
  which is exactly the class of bug a decoder has to be read for rather than fuzzed for.
- **The module line cap was right about what it was pointing at.** JpegDecoder.cpp came in at
  950 lines against a cap of 800, and the honest split was not "the big functions" — it was
  the entropy-coded bit stream (no framing, ends wherever a 0xFF says) against the container
  of marked segments (lengths, tables, a frame description). They fail differently and read
  differently. Raising the cap would have hidden a seam that was already there.

## Session 6 — picking the right image · 2026-08-05

**Status:** done
**Check:** `tools/srcset-check.sh`, written this session because the roadmap's check ("a
high-density snapshot picks the 2x candidate") named no page and no page on the compatibility
list uses `srcset` in markup this browser can reach. It renders a `data:` document whose two
candidates are `data:` PNGs — a 20x20 red one at 1x and a 40x40 blue one at 2x — at both
densities, and reads the pixels back:

```
=== device pixel ratio 1 ===        === device pixel ratio 2 ===
  [  1] Image      0,0 20x20          [  1] Image      0,0 40x40
  [  2] Image     20,0 20x20          [  2] Image     40,20 20x20
  ok   at 1x the img picks the 1x candidate: (220, 20, 20)
  ok   at 2x the img picks the 2x candidate: (20, 20, 220)
  ok   at 1x the picture falls back to the img: (220, 20, 20)
  ok   at 2x the picture still declines the webp source: (220, 20, 20)
PASS
```

The corroborating measurement is a real page. `en.wikipedia.org/wiki/Main_Page` uses `srcset`
19 times and `<picture>` twice, and the same document decodes **420,240 JPEG pixels at 2x
against 228,500 at 1x**, and **175,081 PNG pixels against 22,321** — the larger candidates are
being fetched and decoded, not merely parsed. old.reddit.com is unchanged: 24 images loaded, 1
failed (the GIF), the same numbers session 5 recorded.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. `image_selection_fuzzer` over
its seven seeds: **2,355,500 runs in 301 seconds, no crash**, corpus grown to 4,755 inputs.

**Landed:**

- *A media query is a question about the viewport, and something now answers it*
- *An <img> may name several images, and the viewport decides which one arrives*

**Left:**

- **`@media` still does not use the evaluator this session wrote.** See Found — it is the
  single highest-value thing left in the CSS module and it is now four lines of wiring plus a
  session's worth of looking at what moved.
- **A selected candidate's density does not scale its intrinsic size, deliberately.** The spec
  says a 40x40-pixel image chosen at 2x is 20x20 CSS pixels. Doing that today would make it
  *paint* at 20x20 device pixels, because nothing in the paint path scales by the device ratio
  (see Found). The two wrongs currently cancel on screen and disagree in layout, which is the
  better half to be wrong in until the paint path is fixed.
- **Re-selection on a viewport change does not happen.** `Page::CollectImages` selects once, at
  load and whenever a stylesheet arrives. A resize afterwards keeps the old candidate, because
  the new one's bytes were never fetched and swapping the URL would produce an empty box where
  an image was. Session 12's lazy-image work is where a second fetch pass belongs.
- **`loading="lazy"` is untouched**, as ADR 0023 §4 and the roadmap both say: it is an
  `IntersectionObserver` against the scrollport and it waits for sessions 8 and 12.
- **`calc()` in a `sizes` value resolves to nothing** and the entry is skipped. `css::Calc.h` is
  private to the css module and a `sizes` length needs viewport units the cascade's `Length`
  cannot hold; the honest failure is to fall through to the next entry or the 100vw default.

**Found:**

- **`@media (min-width: 600px) { … }` has been dropping every rule inside it, on every page,
  for as long as `@media` has been parsed.** `MediaListItemMatches` in `css/StyleSheet.cpp`
  accepts a media list item only when it is exactly one Ident token — `screen`, `all` — so any
  prelude with a parenthesis in it is false and `ParseRuleList` is never called for the block.
  ADR 0014 counts `@media` at **791 occurrences** and marks it "yes". It is not yes. This
  session wrote the evaluator that fixes it and deliberately did not wire it in: turning it on
  changes which declarations apply to every page this browser has ever rendered, and that is a
  session with a snapshot on either side of it, not a footnote in an image session. **It is
  session 49 in the ledger.**
- **The device pixel ratio has no effect on painting at all.** `Engine::SetViewport` stores it,
  `LayoutAndPaint` divides the viewport width by it, and nothing else reads it: a display list
  is in CSS pixels and the canvas treats them as device pixels. So `-dpr 2` lays the page out
  at half the width and draws it in the top-left quarter of the canvas. That is why `-dpr` is
  documented as "which srcset candidate an `<img>` picks" rather than as a HiDPI switch, and it
  is why intrinsic-size scaling is in Left rather than in this diff.
- **A `srcset` is separated by whitespace, and the comma is the trap.** `a.png,b.png 2x` is
  *one* candidate whose URL contains a comma, and the spec is explicit about it because
  CDN URLs contain commas — reddit's preview service produces them, and so does wikimedia's
  thumbnailer. The obvious implementation (split on commas, then on whitespace) turns that into
  two candidates and fetches neither. Both forms have a test.
- **The compatibility targets do not exercise this feature in markup we can reach.**
  old.reddit.com has **zero** `srcset` and zero `<picture>`; `www.reddit.com` returns an 8KB
  challenge to curl, and ADR 0023's "6 uses of `srcset`" comes from the 405KB document behind
  it. The feature is right and it is on the roadmap for a real reason, but the page that
  proves it is wikipedia's, not reddit's, and the check had to be built rather than found.
- **A `<source type>` this browser cannot decode is a fork in the road, not a filter.** The
  list of decodable MIME types and the magic-number sniffing in `Engine::DecodePendingImages`
  are the same fact stated twice, and there is no way to derive one from the other — a MIME
  type is not a magic number. `ImageSelectionTests` asserts the list exhaustively so that
  landing GIF fails a test that points at the other copy, which is the cheapest honest version
  of keeping them in step.
- **GCC's `maybe-uninitialized` fires on `std::optional<float>` at -O2 and not at -O0**, so it
  appeared only in the asan log after the debug build and the whole test suite were green. The
  fix was to return a `bool` and an out-parameter from the two functions that get inlined into
  every feature comparison. Worth knowing before spending an hour on a diagnostic that names an
  offsetof into an optional's storage.

## Session 7 — geometry · 2026-08-05

**Status:** done

**Check:** the ledger asks that a page mutating a style and immediately reading a rect gets the
post-mutation rect, with `MICROBROWSER_PERF_COUNTERS=1` showing the forced layout. The page is a
100px red box, a script that widens it to 250px and reads the rectangle back with no frame in
between, and a second blue box whose width is set to whatever the rectangle said:

```
$ URL="data:text/html,$(python3 -c "import urllib.parse;print(urllib.parse.quote(open('/tmp/geom.html').read(),safe=''))")"
$ MICROBROWSER_PERF_COUNTERS=1 ./build/microbrowser/microbrowser_snapshot "$URL" -o /tmp/geom.ppm -v
[counters]                  1  layout.forced_by_script
[counters]                  2  layout.runs
  [  0] FillRect   0,0 1280x900 #FFFFFFFF
  [  1] FillPath   0.0,0.0 250.0x20.0 #FFCC0000
  [  2] FillPath   0.0,20.0 250.0x20.0 #FF0000CC
```

The blue probe is 250 wide, which it can only be if the read after the write returned 250. One
forced layout, not two: the read *before* the write cost nothing, because the layout was clean.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. old.reddit.com and Hacker News
render as session 6 left them — checked by looking at the images, not by counting commands.

**Landed:**

- *A tree records that it changed, at the five places it can*
- *Inserting before nothing is appending, and it need not read the whole list first*
- *A page may ask its own layout where a box is, and is answered in values*

**Left:**

- **`scrollTop` and the rest of ADR 0018 are session 8, and this session moved the one piece of
  them it had to.** The viewport's scroll offset now lives on `Page` rather than on `Engine`,
  because `getBoundingClientRect` is viewport-relative and painting subtracts the same number:
  two copies of one offset is the pair that drifts. There is still no per-box scroll offset, no
  `scrollWidth`/`scrollHeight`, and `bindings::BoxGeometry` deliberately has no `scroll_offset`
  field — an always-zero one would be a stub in ADR 0012's sense.
- **`offsetTop`/`offsetLeft` are absent**, because they are defined against `offsetParent` and
  `offsetParent` is a walk this box tree does not support yet. A missing name is the honest
  version; the four metrics that *are* here (`offsetWidth`/`offsetHeight`,
  `clientWidth`/`clientHeight`) need no parent.
- **The `inset` properties answer from the cascade, not from layout.** `getComputedStyle(el).top`
  reports `auto` or the length as written rather than the used offset, because the used value is
  a distance from a containing block that `layout::BoxGeometry` does not record. It is in the
  code and in the commit message; it is the one place `getComputedStyle` is not the resolved
  value.
- **`BoxFor` is a tree walk per query.** One `getBoundingClientRect` is O(boxes). A page that
  measures a thousand elements in a loop walks the tree a thousand times. The fix is a map from
  element to box built during layout, and it is deliberately not here: ADR 0015 says build the
  honest slow version and instrument it, and `layout.forced_by_script` is not the counter that
  would show this one. Add `layout.box_lookups` when it matters.
- **`IntersectionObserver`/`ResizeObserver` are now tractable** — both are this `QueryBox` driven
  by the frame deadline rather than by script — and they are session 12.

**Found:**

- **`dom::Node::InsertBefore` made every parse quadratic in its widest sibling list.** A null
  reference child is never found, so `std::find_if` scanned every existing child before falling
  through to the append it was always going to do — and the tree builder inserts *every* element
  in a document through that path with a null reference. 15,000 siblings under one parent took
  980ms, 30,000 took 3.8s, and 60,000 took **15 seconds**; all three are now 30–106ms. Nothing
  about the input is exotic: a flat list of `<div>` does it and so does a list of `<br>`. It is a
  denial of service with a 400KB payload and the multiplier is a number the document chooses.
  Found by measuring whether the *mutation version's* root walk was affordable — the walk turned
  out to cost 1.4ms in 99 and the thing it was being measured against cost 15 seconds.
- **ADR 0015's central sketch does not compile.** It says "the interface is a header the engine
  publishes and `src/bindings` already depends on the engine seam". `src/bindings/MODULE.deps`
  allows `util js dom html` — not `engine`, not `layout`, not `gfx`. The ADR spends two
  paragraphs on exactly why widening that line is refused and then assumes a dependency that
  would need it. The interface is declared in `src/bindings` and implemented by `src/engine`
  instead, which is the same decision with the arrow the other way, and `BoxGeometry` is four
  floats rather than a `gfx::FloatRect` for the same reason. Worth knowing before reading the
  ADR as a specification.
- **`element.style` writes bypass `DomBindings::SetElementAttribute`.** `StyleBindings.cpp`'s
  `set` trap calls `dom::Element::SetAttribute` directly, so a style written from script fires no
  custom-element `attributeChangedCallback` and records no `MutationObserver` entry for `style`.
  This session did not make that worse and does not fix it — the geometry seam sees the write
  because the mark is in `dom`, which is the argument for putting it there — but a page observing
  `attributeFilter: ['style']` is currently told nothing.
- **`getPropertyValue`'s receiver is the `Proxy`, not the element.** A method handed out from a
  `Proxy`'s `get` trap is called with the proxy as `self`, and `NodeOf` reads an *own* property,
  which a proxy does not have. The element travels on the function object beside the bindings
  pointer instead. The same trap is waiting for anything else that hands a method out of
  `MakeStyle` or `MakeComputedStyle`.
- **The synthetic test font has glyphs for `A`, `B`, `C`, `D` and the space, and nothing else.**
  A test that measures the word "hello" measures zero and passes anything that asserts a width is
  small. Two of this session's tests were written wrong that way and one of them looked like a
  layout bug for ten minutes.
- **The check for this session cannot be run through `console.log`.** `microbrowser_snapshot`
  prints scripts that threw and nothing a page logged, and it does not accept `file://`. Writing
  the measured number back into a second element's width, and reading it off the display list
  with `-v`, is the way to make a script's result observable from the command line — worth
  remembering for every later session whose check is about a number rather than a picture.

## Session 8 — the scroll model · 2026-08-05

**Status:** done
**Check:** the ledger's check needs `MICROBROWSER_TRACE_REDRAW=1` and a wheel event, and the
browser's copy of that trace lives in `Application::PaintAndPresent` — which needs a display this
machine does not have. So the trace was added to `microbrowser_snapshot`, which already takes `-y`
and already sends a `ScrollMessage`. On a page with a `position: sticky; top: 0` header over three
400px blocks, at 1280x900:

```
$ MICROBROWSER_TRACE_REDRAW=1 microbrowser_snapshot <page> -o out.ppm -y 53
[redraw] partial rects=1 coverage=100.0% surface=1280x900 scroll=0,0 commands=1
[redraw] partial rects=1 coverage=100.0% surface=1280x900 scroll=0,0 commands=9
[redraw] partial rects=2 coverage= 10.3% surface=1280x900 scroll=0,-53 commands=9
```

Two frames of load at 100%, then the scroll at **10.3%** — a 1280x53 band (5.9%) plus the sticky
header's own strip (4.4%). Before this session every scroll printed 100%. The image after the
scroll has the red header at y=0 with the second block under it, so the header sticks; the check's
second half was verified by looking at the picture, not by counting commands.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. Hacker News and old.reddit.com
were rendered and looked at before and after.

**Landed:**

- *A box may be scrolled, and sticky stops being relative with the truth left out*
- *A page may ask where it has scrolled to, and move it, and be told once per frame*
- *A scroll is a wheel over a box, and a frame that is the last one moved*
- *Fourteen ways for a scroll to become a layout again, each with a test*

**Left:**

- **No scrollbars are painted.** ADR 0018's consequence list calls them the first thing this
  browser draws that is neither page content nor browser chrome, and says they belong to
  `src/layout` plus `src/gfx` rather than to `src/ui`. Nothing here draws one, and `clientWidth`
  therefore does not subtract one — which is currently correct and will silently stop being so on
  the day one appears.
- **No smooth scrolling.** `scroll-behavior: smooth` and the smooth flag on `scrollTo` are ignored;
  every scroll is a jump. That is a missing behaviour rather than a stub, and ADR 0018 §3 has the
  design: an animation registered the way `bindings::AnimationFrames` registers one, running while
  it runs and then stopping.
- **The viewport scrolls vertically only.** Layout never exceeds the viewport width, so there is no
  horizontal document overflow to reach; a *box* scrolls both axes and is tested doing it.
- **`IntersectionObserver` and `ResizeObserver` are session 12 and are now tractable** — both are
  "is this box in that scrollport", sampled at the frame this session gave them.
- **`BoxFor` is still a tree walk per query** (session 7's note), and `scrollTop` now goes through
  it too. A page reading `scrollTop` in a scroll handler walks the box tree once per frame.

**Found:**

- **Full CSS 2.1 Appendix E paint order was implemented, measured against a real page, and
  reverted.** Hoisting every positioned box after every in-flow sibling is what the specification
  says, and it deleted the top row of old.reddit.com: its `#sr-header-area` background is a
  positioned box in one subtree and the subreddit list is in another, so the background painted over
  the list. Ordering *between* subtrees is what a stacking context decides and what per-parent tree
  order cannot express. Only `sticky` and `fixed` are hoisted now, with the reason written where the
  code is. **That is session 21, and this is the measurement it should start from.**
- **A sticky box painted in tree order is invisible, not misplaced.** The first attempt had the
  display list right — `FillPath 0.0,0.0 1280.0x40.0 #FFCC0000` at the top, exactly where it should
  stick — and the picture had no header in it, because three later siblings drew over it. Reading
  the display list said the feature worked; looking at the image said it did not. This is the third
  session in a row where that pair disagreed.
- **`position: fixed` was scrolling with the page**, and had been since it was implemented. Nothing
  reported it because nothing scrolled far enough to notice, and the fix is one line in the paint
  recursion. Its *containing block* is still the nearest positioned ancestor rather than the
  viewport, which is wrong and is layout's half of the same bug; it only shows on a fixed box
  underneath a positioned one.
- **`DispatchEventTo` reports `preventDefault`, not "a listener ran".** For a non-cancelable event
  those are not the same question, and using the return value as the second would have relaid out
  the whole document on every wheel notch — the exact cost this ADR exists to avoid. The `scroll`
  path asks whether anything is listening first, walking the ancestor chain for the `#on:scroll`
  slot the way `DispatchAtWindow` already did for the window. **Every other caller of
  `DispatchEventTo` that treats its return value as "something happened" has the same bug waiting.**
- **Damage that over-reports is safe and still worth bounding.** A sticky box's damage is the strip
  between where the flow put it and where it sticks, and most of that strip is off screen: reporting
  it unclipped turned a 40-pixel header on a 900-pixel window into 38% of the surface — true,
  useless, and a 3.7x pessimisation of the number the check reads.
- **The paint is partial and the texture upload is still whole**, and that is not a shortcut. A
  streaming texture cannot be told its contents slid, so the pixels that moved have to be re-sent.
  Rasterizing paths, glyphs and images is the expensive half; a full-surface upload is a memcpy.
  Two flags on `Application` and not one, because conflating them either repaints the window or
  leaves the texture one scroll out of step with the canvas.
- **The `check` in the ledger named a tool that could not run it.** `MICROBROWSER_TRACE_REDRAW` was
  a browser-only flag, and the browser needs a display and a wheel. Sessions 9–12 all have checks
  phrased as interactions ("typing in reddit's search box works", "Escape closes a menu"); each of
  them needs an input path into `microbrowser_snapshot` that does not exist yet, and writing one is
  probably session 9's first deliverable rather than a surprise at its end.

---

## Session 9 — input, events and focus (1 of 2) · 2026-08-05

**Status:** done

**Check:** amended, and the amendment is a finding of its own — see **Found**. The original check is
one sentence shared verbatim with session 10, and its last clause is session 10's own scope. What
was run:

```
$ microbrowser_snapshot https://old.reddit.com/ -click 1120,103 -type cats -key Enter
https://old.reddit.com/search?q=cats: 902 commands, 640 runs, 22 fonts, 21 images,
  title "reddit.com: search results - cats" -> /tmp/r3.ppm
```

Clicked old.reddit's search field, typed four characters, pressed Enter, and the browser navigated
to the search results and rendered them with `cats` in the box. Before this session there was no
message that could have carried the Enter: a key crossed the seam as *the text it produced*, and
Enter produces none, so it arrived as an `InputCommandMessage` enum with three values.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each. The IPC fuzzer: 65,651 runs, no
crash.

**Landed:**

- *Three input messages become two, and a key finally has an identity*
- *Eight rules of dispatch, each one silently wrong until something asserted it*
- *Three seeds, because a corpus of version-2 frames never reaches a version-3 decoder*

**Left:**

- **Session 10 is the focus model** — `activeElement`, `focus()`, `blur()`, Tab order,
  `:focus-visible` — **and `src/app`'s chrome-or-page decision as a tested security boundary.** Key
  routing today is `Page::DispatchKeyToFocus`, which goes to the one element a *click* focused, or
  to the document. Nothing lets script move focus, so a page that calls `input.focus()` and then
  expects to be typed into is still broken. The ordering in `Application::HandleInputEvent` is
  already chrome-first; what session 10 owes is the test that says so.
- **`Escape closes a menu` needs a menu, not a key.** Escape reaches a page now, pressed and
  released, with `key === 'Escape'` and `keyCode === 27`, and `tests/EngineTests.cpp` asserts it.
  old.reddit's own two Escape handlers are both `keyup` with `keyCode == 27` — a report modal and a
  saved-category bubble — and both need a logged-in session to open the thing they close. Pick a
  different page for that half of session 10's check rather than discovering this at its end.
- **Deliberately absent, each with its ADR paragraph:** composition/IME (§1), touch events (§6 —
  `ontouchstart` stays undefined), `keypress` (Alternatives). `PointerInputMessage::Kind` has only
  `Move`/`Down`/`Up`: `Enter`/`Leave` arrive with `:hover` in session 11, and a wheel is still
  `ScrollMessage` because session 8 built it and two paths for a wheel is exactly what ADR 0017 §2
  forbids.
- **A `keyup` for a printable character now exists and nothing consumes it.** It was suppressed
  along with the keydown that would otherwise have typed the character twice, which is why it had
  never been noticed as missing.

**Found:**

- **The check was one sentence for two sessions, and could not gate either.** Sessions 9 and 10
  share the string "Typing in reddit's search box works; Escape closes a menu; a page cannot type
  into the omnibox." The third clause is session 10's scope by the roadmap's own words, so session 9
  could never pass its own check as written and session 10 would inherit a clause already passed.
  The ledger now splits it. This is the second session in a row to find its check unrunnable as
  phrased — session 8's named a browser-only environment variable — and both times the cause was the
  same: **a check written as a user interaction, in a repo whose only headless tool took a URL and
  nothing else.** `microbrowser_snapshot` now has `-type` and `-key`; sessions 10, 11 and 12 all
  need them.
- **The typed text was painted and invisible, for the third session running.** The display list has
  `Text 979.0,106.7 "cats"` exactly inside the input's `975,92 300x21.6` box, and the picture shows
  an empty field: old.reddit's "Welcome to Reddit" interstitial is a positioned box in another
  subtree that paints over it. That is session 21's problem and session 8 already measured it —
  full CSS 2.1 Appendix E paint order was implemented, measured against this same site, and
  reverted. Recording it again because the pattern is the lesson: **reading the display list said
  the feature worked and looking at the image said it did not, and this time the display list was
  right.**
- **`Object::Set` clobbers an accessor rather than refusing it.** `Heap.cpp:92` returns early for a
  non-writable *data* property and then falls through for an accessor, overwriting `getter`/`setter`
  with a value — while the comment beside `Freeze` says assigning to a getter-only property is a
  silent no-op. It is a no-op from *script*, because `Interpreter::SetProperty` handles the accessor
  case before it ever gets there; it is not a no-op from C++. `isTrusted` is safe because nothing in
  the engine writes it, but the next getter-only property installed by a binding will be silently
  destroyed by any `Set` on the same key.
- **Bumping a protocol version silently invalidates a fuzz corpus.** With the version at 3 every one
  of the ipc corpus's ~1,400 inputs was refused before its tag byte was read: 3,347 runs in 90
  seconds and *zero* executions of the two new decoders. Three hand-written seeds took the same 90
  seconds to 65,651 runs. A fuzzer that finds nothing because it decodes nothing is indistinguishable
  from a fuzzer that found nothing.
- **Three translation units passed their cap at once, and each split was real.** `EventBindings.cpp`
  became registration plus `EventDispatch.cpp`, which is the one dispatch algorithm; `Engine.cpp`
  became routing plus `EngineInput.cpp`, which is dispatch-then-default-action for a click and a
  key; `Page.cpp` became boxes plus `PageEditing.cpp`, which is the one caret's worth of editing
  there is — and writing that file down is what makes it obvious that "delete forwards" is missing
  because there is no caret, not because the key was forgotten.
- **SDL splits one keypress across two events and neither has both halves.** `KEY_DOWN` knows which
  physical key was struck; `TEXT_INPUT` knows what it typed. ADR 0017 needs `code`, `key` and `text`
  in one message, so `SdlWindow` carries the scancode from the first to the second — its fourth
  member. This is the only part of the session that cannot be tested here, because it needs a
  display.

## Session 10 — input, events and focus (2 of 2) · 2026-08-05

**Status:** done

**Check:** amended, and the amendment is a finding — see **Found** (1). The original is "Escape
closes a menu on a real page, and a page cannot type into the omnibox." The second clause stands.
The first is not reachable, and not because of anything in this session. What was run:

```
$ microbrowser_snapshot https://news.ycombinator.com/ -h 1400 \
    -click 650,1250 -key Escape -type rust -key Enter
https://hn.algolia.com/?q=rust: 2 commands, 1 runs, 1 fonts, 0 images,
  title "Hacker News Search powered by Algolia" -> /dev/null
  focus: none

$ microbrowser_snapshot https://news.ycombinator.com/ -key Tab
  focus: a[href=https://news.ycombinator.com] keyboard
$ ... -key Tab -key Tab
  focus: a[href=news] keyboard
$ ... -key Tab -key Tab -key Tab
  focus: a[href=newest] keyboard
```

The first is the whole chain on a real page: a click moved focus to Hacker News's search field,
Escape was delivered to the page without disturbing it or being taken by the chrome, typing reached
the focused control, and Enter submitted the form it owns. The second walks the page's own tab
order in document order, keyboard-visible. `Focus/EscapeReachesThePageAndClosesItsMenu` is the menu
half, against the real dispatch algorithm. `Chrome/NothingTypedIntoTheOmniboxReachesThePage` and
`Chrome/TheWayOutOfAPageIsNotThePageToTake` are the security half.

`tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each.

**Landed:**

- Focus as **one element on one document** — `dom::Document::FocusState`, with the
  `:focus-visible` bit beside it because they are two facts about the same thing. `activeElement`,
  `focus()`, `blur()`, Tab and Shift+Tab in the specification's order, and the four focus events.
- `app::KeyRouting` — chrome or page, decided before the key becomes an `ipc::KeyInputMessage` and
  therefore before it could cross into a sandboxed renderer.
- The arrow and page keys moved out of the chrome and into `Engine::ScrollByKey`, as a keydown's
  default action.
- `Engine::FocusDescription`, printed by `microbrowser_snapshot` as a `focus:` line.

**Found:**

- **A second copy of focus had been quietly wrong the whole time.** `Page::focused_text_control_`
  could only ever hold a text field, so `input.focus()` from a script and the element a click
  focused were two different facts and the key went to whichever the last *click* had set. A page
  that focused a field and expected to be typed into was broken by construction, and no test could
  have seen it because there was no `focus()` to call. The member is gone and Page has one fewer;
  this is the same shape as session 8's scroll offset and session 7's layout-clean flag — **the bug
  is not in the algorithm, it is in there being two places to keep the answer.**
- **Session 9's check command no longer works, and that is the correct outcome.**
  `-click 1120,103` on old.reddit focused the search box only because the old hit test looked for a
  form control *under* the point and ignored whatever was painted over it. A click now focuses the
  nearest focusable ancestor of the **topmost** element, which is what a click means — and on
  old.reddit the topmost thing over the search field is the `.side` sidebar box at `975,66
  300x191.6`, which covers the field at `975,73 300x21.6` entirely. Clicking a story title still
  navigates, so links are unaffected; it is the search box alone. **The root cause is layout, not
  focus:** `#header-bottom-right` is `position: absolute; bottom: 0` inside a 66px `#header` and
  lands at y=73 — seven pixels *below* its containing block's bottom edge. It was seven pixels
  below before this session too (header 85, field 92), so it is pre-existing and independent.
- **A replaced element was inline-level whatever its `display` said**, so `img { display: block }`
  did nothing — and an absolutely positioned one stayed in the flow and was laid out *twice*, once
  by the flow it had never left and once as an absolute. Found sideways, from two `display: block`
  buttons in a focus test that were laid out side by side so a click meant for the second hit
  nothing. Fixed in its own commit; on old.reddit it takes 19px of false height out of the header
  (`#sr-header-area` is absolutely positioned) and puts the interstitial's close button at the
  right edge where `right: 0` puts it instead of mid-box at x=744. **Two comments asserted the rule
  that the code did not implement** — `Box::IsInlineLevel` and `css::ComputedStyle::IsInlineLevel`,
  the second of which names the exact clause of CSS 2.1 §9.7 it then fails to apply.
- **The interaction checks had no way to see an interaction.** Three sessions in a row now have
  checks phrased as clicks and keys, and until this one the only observable was the display list —
  which says what was *painted*, not where the next keystroke will go. A click that focused the
  wrong element renders identically to one that worked. `microbrowser_snapshot` now prints
  `focus: <tag>[#id|name|href] keyboard|pointer` whenever something was driven at the page, and
  every reddit probe above became a one-line read instead of a debugger session. Sessions 11 and 12
  are interaction checks too.
- **A closed menu is a container with `hidden` on it and its items still inside.** Asking only the
  element let Tab walk into a menu the page had closed — and focus is the input router, so that is
  a keystroke delivered to something the user cannot see. Focusability now asks the ancestors.
  `display: none` is the same bug through a property `src/html` may not name; that one needs ADR
  0016's element state bits, which is session 11.

**Not done, deliberately:**

- `:focus-visible` is the **state** and not the selector. Matching it needs the invalidation index
  of ADR 0016 — session 11 — and a state with no selector is honest where a selector that never
  matches would not be.
- No sequential-focus-navigation starting point: Tab after a click on nothing starts from the top
  of the document rather than from where the click was.
- Tab **wraps** inside the document rather than handing focus to the browser chrome. The engine has
  no chrome to hand it to, and ADR 0017 §4 keeps that decision in `src/app`.
- Hiding the focused element's container does not blur it. *Removing* it does —
  `Node::ReleaseFocusWithin`, beside `NoteMutation` and there for the same reason: missing a call
  is the failure mode, and the document's focus is a raw `Element*`.
- The known imprecision in `RouteKey`, written down at the bottom of `KeyRouting.h`: the
  destination is decided per event, so a key pressed while the omnibox had focus and released after
  it lost it delivers its release to the page. Enter is the case. Closing it means state in the
  routing rule, for a keyup with no keydown on a document being navigated away from.

## Session 11 — dynamic pseudo-classes and the invalidation index · 2026-08-05

**Status:** done

**Check:** *"A page with no `:hover` rules does not restyle, does not relayout and does not repaint
when the pointer crosses it."* Run as
`StyleInvalidation/MouseCrossingAPageWithNoHoverRulesCostsNothing`, in `tests/StyleInvalidationTests.cpp`
— twenty-two pointer moves across a loaded page, with `css.styles_resolved`, `layout.runs` and
`engine.paints_produced` read before and after. All three deltas are **0**, and so is
`style.hover_hit_tests`: the index is asked before the box tree is walked, so a mouse move on such
a page does not even hit-test. `tools/run-checks.sh tests`, `asan`, `ubsan`: 24/24 shards each.

The check is a unit test because there is no other observable — a browser that restyles the
document on every mouse move renders identically to one that does not. It lives next to
`IdleWaitStrategyTests` in spirit and says so in its header comment.

Verified on real pages with the new `microbrowser_snapshot -hover x,y`:
`nav:hover .menu { display: block }` reveals the menu, `li:hover + li` paints the *next* sibling
yellow, a link recolours on hover, `:checked + label`, `:disabled`, `:focus`, `:focus-within` and
`:target` all apply.

**Landed:**

- *A state a selector matches on is a bit on the element, except focus, which is one element on one
  document* — `dom::ElementState`, the matcher, `Selector::DynamicStates`.
- *A pointer crossing a page no rule cares about costs a bitmask test* — `css::StyleInvalidation`,
  `css::PropertyAffectsLayout`, `Page::RestyleWithoutLayout`, the engine's pointer path, five
  counters.
- *A pointer may be moved at a page from outside it, and a fragment stops being document content* —
  `-hover`, and the data-URL fragment bug it found.
- *The fieldset walk asks only the elements that could be inside one.*

**Found:**

- **Focus must not be a bit, and the enum says so in code rather than in a comment.** ADR 0016 §2
  says every dynamic state is "a bit on the element". Applied literally that would have put
  `:focus` back into the shape session 10 removed — two copies of focus, disagreeing about where the
  next keystroke goes. `dom::ElementState` names all ten states so the index can file a rule under
  any of them, `kStoredElementStates` names the seven that are stored, and `Element::SetState`
  **refuses** to write the other three. `DynamicState/FocusComesFromTheDocumentAndNotFromABit` sets
  the Focus bit and asserts that `:focus` still does not match.
- **`:disabled`/`:enabled` and `:required`/`:optional` are not complements**, and treating them as
  such styles the whole page: a `<div>` matches neither, and so does a `<div disabled>`. The tag
  list is in the matcher, beside `:link`'s `a[href]` test and there for the same reason.
- **The index is per-*state*, not per-element, and that is the measured cost.** On Hacker News one
  rule — `pre:hover { overflow: auto }` — makes **every** hover anywhere on the page a full
  relayout, because `overflow` is layout-affecting and `Hover` is therefore in the layout set.
  Three dynamic rules out of 182 on HN; 387–977 out of 4901–7640 on old.reddit (the page varies).
  ADR 0016 §3's table asks for "E, plus the subtree reachable by the combinators", which is element
  granularity; what landed is document granularity. Closing that gap needs the *last compound* of
  each rule indexed by tag/class/id as well as by state, and it only helps the style half —
  `layout::LayoutEngine` has no partial layout, so a layout-affecting hover relays out the document
  either way.
- **A data URL's fragment was being decoded as body.** `data:text/html,<h1 id=x>t</h1>#x` rendered
  `:target` correctly *and* drew the text "#x" after the heading. Found by rendering, not by a test.
  One existing test changed with the fix and the commit message says why: `DataUrl` in
  `EngineTests.cpp` embedded `querySelector('#one')` unescaped and had been relying on the fragment
  not being honoured.
- **`text-decoration: underline` is parsed and never painted.** `a:hover { text-decoration:
  underline }` changes the colour and nothing else. Not this session's to fix, and not on any
  roadmap row.

**Left:**

- **The other three rows of ADR 0016 §3's table** — a class change, an attribute change, a DOM
  insertion. They are not in the index, deliberately: they need a change signal finer than
  `dom::Document::MutationVersion`, which today says only that *something* moved, and keys nothing
  can query would be an index that is always right and never consulted.
- **A focus move does not consult the index.** `Page::MoveFocus`'s callers still invalidate the
  layout and repaint unconditionally, which is correct and more than necessary. The states are
  there; the wiring is four lines and was left out of this session rather than done untested.
- **The hover chain is recomputed rather than remembered**, one document walk per state change.
  That is the safe shape — a stored `Element*` dangles the moment a script removes what the pointer
  is over, and unlike focus there is no `ReleaseFocusWithin` choke point for it — and it is only
  reached when some rule depends on the state. The measurement that would justify changing it is
  `style.state_changes` against `style.hover_hit_tests`.
- **`:has()` is still out**, as ADR 0016 §1 said it might be.

## Session 12 — `IntersectionObserver`, `ResizeObserver`, lazy images · 2026-08-05

**Status:** done

**Check:** *"reddit's front page fetches the images on screen, not all 33."* Run against
`https://www.reddit.com/` with `MICROBROWSER_PERF_COUNTERS=1`, comparing this binary with one
built from the previous commit (`58cd8dd`) in a worktree, twice each because reddit's page varies
between loads:

| | 58cd8dd | this commit |
|---|---|---|
| `net.requests_started` | 29, 29 | 15, 13 |
| `engine.images_loaded` | 26, 26 | 12, 10 |
| `engine.images_revealed` | — | 7, 7 |
| `engine.images_deferred` | — | 114 |

The base binary fetched 26 images on every run; this one fetches 8–12, and the rendered page still
shows every image above the fold (checked by looking at the PPM, not by counting). Corroborated
deterministically on `en.wikipedia.org/wiki/CSS`, which does not vary: 12 deferrals at load,
18 images drawn → 16, and with `-y 30000` the two below-the-fold images are revealed, fetched,
decoded and drawn (`engine.images_revealed` 2, `engine.images_loaded` 19 → 21, 4685 → 4687
commands). Suite green; asan and ubsan green; `root_margin_fuzzer` 200,000 runs clean.

**Landed:**

- *Two observers that sample at the frame, and an image below the fold that is not fetched* —
  `src/bindings/ViewObservers.{h,cpp}`, `GeometrySource::QueryViewport`,
  `window.innerWidth`/`innerHeight`, `Page::DeliverObservations`, `Page::RevealLazyImages`,
  `Page::TakeUnrequestedImages`, `Engine::StartImageRequests`/`OnLateImage`,
  `src/engine/PageResources.cpp`, `fuzz/RootMarginFuzzer.cpp`, `tests/ViewObserverTests.cpp`,
  six counters.

**Found:**

- **An image first named by an external stylesheet was collected and never fetched**, and had
  been since stylesheets became asynchronous. `Engine::StartSubresources` runs **once**, at
  document arrival; `Page::CollectImages` re-runs whenever a sheet lands and adds the background
  images that sheet named. Those went into `pending_images` and stayed there. `CLAUDE.md` and the
  comment on `RebuildAuthorStyleSheets` both assert the re-collection exists so that "a page whose
  icons come from an external sheet would never fetch one" — the re-collection was there and
  nothing acted on it. The frame now asks for anything unrequested. On old.reddit.com that is four
  more requests and the *same* 23 images decoded, because the four are formats with no decoder yet.
- **`rootMargin: '1e300px'` made an observer go silent.** The value is a finite double and `inf`
  once narrowed to a float; an infinite root bound makes every ratio `inf/inf`, and a NaN compares
  false against every threshold — so the observer stops firing rather than firing wrongly. That is
  precisely the failure ADR 0018 §5 forbids, reachable from one string a page writes. Clamped as a
  double before the cast. The fuzz target exists for this class and asserts finiteness rather than
  absence of a crash; there was never a memory bug to find here.
- **`microbrowser_snapshot -y` did not turn the crank after scrolling.** The click and key paths
  called `RunLoadToCompletion` and the scroll path did not, which was invisible until a scroll
  could *start a fetch*. A `-y` snapshot wrote the frame from before the revealed image arrived,
  which looks exactly like a lazy loader that does not work. Fixed in the same commit.
- **ADR 0018 §5's premise about which reddit is off by one site, again.** It says `loading="lazy"`
  has "27 uses on reddit's front page alone". `old.reddit.com` has **zero** — `engine.images_deferred`
  does not move there. It is `www.reddit.com`, behind the Phase A challenge, that defers 114. This
  is the third session to find the survey's counts belong to the site the roadmap was not naming
  (sessions 3 and 4 found the same for `:not()` and `calc()`), and the pattern is now worth
  believing: **a count from the survey is about `www.reddit.com` unless it says otherwise.**
- **`engine.images_deferred` counts deferrals, not distinct images**, and the counter's comment
  says so. Collection re-runs on every stylesheet and every script turn, so an image below the fold
  is counted once per collection. `engine.images_revealed` is per image, exactly once. Making the
  first one distinct needs the deferred set to survive `CollectImages`, and it deliberately does
  not: its keys are `const dom::Element*`, and an element a script removed must not be asked for
  its box.

**Left:**

- **The intersection is not clipped by intermediate containers.** The specification intersects the
  target with every clipping ancestor between it and the root; this intersects with the root alone.
  A target positioned inside the viewport but hidden by an `overflow: hidden` ancestor smaller than
  it is reported as visible. Closing it needs the geometry seam to answer *which of my ancestors
  clip*, which it cannot — `GeometrySource` returns boxes, and `overflow` is a property.
  Recorded in the file header rather than here alone.
- **`devicePixelContentBoxSize` is refused with a `TypeError`, not answered.** It is the device
  pixel ratio times a size, which is ADR 0029's decision rather than a geometry one, and a
  CSS-pixel answer under that name renders a canvas at the wrong resolution. A page that
  feature-detects it in a `try`/`catch` gets its own fallback.
- **`loading="lazy"` reaches within one viewport in each direction**, a number this browser chose
  and no specification states. If reddit's feed ever looks like it is loading images too late, that
  constant (`Page::RevealLazyImages`) is the dial, and `engine.images_revealed` against
  `net.requests_started` is how to tell whether turning it helped.
- **The lazy reveal is a geometry test, not an `IntersectionObserver`.** It runs on a page with no
  script, which is right, but it means there are now two implementations of "is this box near the
  scrollport". They agree today because both ask `QueryBox`; if the observer ever grows the
  ancestor clipping above, this will not follow it unless somebody makes it.
- **`ResizeObserver`'s loop bound is 8 and it fires no error event.** The specification dispatches
  `ResizeObserverLoopError` at the window when the loop is cut off; this counts
  `view.resize_loop_limit` and stops. Adding the event is a few lines and was left out rather than
  landed untested.
- **Nothing observes yet on the target sites.** `view.observation_frames` stays at zero on both
  reddits, because their own scripts still die early — `www.reddit.com` on the masked-error problem
  session 11's log describes. The observers are proven by `tests/ViewObserverTests.cpp` and by the
  lazy-image path that shares their frame step, not by a real page having used one.

## Session 13 — `fetch` and CORS · 2026-08-05

**Status:** done

**Check:** the ledger's check for this session — "reddit's feed fills in past the three
server-rendered posts. A menu opens." — is the roadmap's check for sessions **13 and 14
together**, and its second half cannot pass here: a `fetch` response becomes nodes through
`innerHTML`, which is session 14's fragment parsing. The ledger's entry has been narrowed to what
session 13's own scope can prove, and this is what it printed.

`MICROBROWSER_PERF_COUNTERS=1 microbrowser_snapshot https://www.reddit.com/` on the live site:

```
https://www.reddit.com/?solution=…&js_challenge=1&token=…: 328 commands, 103 runs, 4 fonts,
  9 images, title "Reddit - The heart of the internet"
[counters]  1  fetch.requests
[counters]  1  fetch.delivered
[counters] 15  net.requests_started
```

**That is the first request this browser has ever made because a page asked for one**, and it
completed: reddit's own error-reporting code `POST`s to `/svc/shreddit/client-errors`, and the
promise settled. `old.reddit.com` (1081 commands, 20 images) and `news.ycombinator.com` (705
commands) render unchanged — neither makes a `fetch`, so neither could regress, which is worth
saying because it is the reason those two are not evidence for anything here.

The suites are: 1414 tests, 0 failed; ASan clean; UBSan clean; `cors_fuzzer` 2,381,490 runs with
no crash and no assertion.

**Landed:**

- *CORS, enforced where the attacker is not, with the response discarded* — `src/net/Cors.h/.cpp`,
  the check inside `FetchRequest::Complete`, the preflight in `RequestQueue`, `RequestQueue::Cancel`.
- *fetch, over the machinery that was already there* — `bindings::NetworkSource`,
  `FetchBindings.cpp` / `FetchTypes.cpp` / `FetchSupport.h`, `Engine`'s implementation in
  `EngineFetch.cpp`, `Loader::Cancel`.
- *A fuzz target for the CORS decision, and what it asserts is the decision.*
- *Request, as the value a page passes around rather than a second request path.*

**Found:**

- **A successful preflight re-queued its request, and the re-queued request preflighted again.**
  `CorsParams::preflighted` was set and never read where the decision is made, so any server that
  grants permission *without* an `Access-Control-Max-Age` — which is most of them, since a grant
  with no max-age is deliberately not cached — got one `OPTIONS` per turn of the loop, forever. It
  is the kind of bug that passes a single-request test and takes a server down.
- **The preflight's own response was being header-filtered on the way out.** `Complete` strips
  every header a cross-origin `cors` response did not expose, and a preflight *is* a cross-origin
  cors request — so the queue read `Access-Control-Allow-Methods` off a response whose only
  surviving headers were `Content-Type` and `Content-Length`, and refused every method. Filtering
  is for a response that leaves the module; a preflight's does not.
- **`Interpreter::SettleAsyncResult` was private while `NewPromiseValue` was public**, which meant
  the host could hand a page a promise and had no way to settle it. Nobody had noticed because
  nothing had ever handed one out. The pair is what makes a host-owned promise usable at all.
- **`src/js` declares its builtins in the global *scope*, not as properties of the global object.**
  `interpreter.Global()->Get("JSON")` is null and `GlobalScope()->Lookup("JSON")` is not, which is
  why `response.json()` first answered "JSON.parse is unavailable". Anything in `src/bindings`
  reaching for a language global has to use the scope; the properties of `window` are only the
  things this module installed.
- **A page could force a preflight with a header that never went on the wire.** `Fetch` drops
  `Origin`, `Cookie` and the rest on the way out, but the preflight decision reads the caller's
  header list — so `fetch(url, {headers: {Origin: '…'}})` made an `OPTIONS` asking permission for a
  header nobody would send. The owned-header list moved to `net::IsHeaderOwnedByFetch` and the
  queue drops them at its front door, so the set a preflight asks about is the set the request
  sends.
- **libFuzzer appears to hang in this environment and does not.** It stalls in `llvm-symbolizer`
  while printing `NEW_FUNC` lines — 9 runs in 90 seconds, no output. `-print_funcs=0` gives
  1,045,526 runs in 21 seconds. Worth knowing before diagnosing a fuzz target that is fine.

**Left:**

- **`.formData()` is absent rather than stubbed.** ADR 0020 §1 lists it with `.text()`, `.json()`
  and `.arrayBuffer()`, and it cannot be written honestly yet: there is no `FormData` class in
  `src/bindings`, and a `.formData()` that answered with something else would be exactly the stub
  ADR 0012 forbids. `FormData` is 81 occurrences in the survey and wants its own decision.
- **`response.body` is not declared, on purpose**, so `if (response.body)` tells a page the truth
  about streaming. Whoever adds `ReadableStream` adds it there.
- **A `fetch` keeps `Engine::IsLoading()` true.** That is what makes `microbrowser_snapshot` wait
  for one, and it is right for a tool that has to see the finished page — but a page with a
  long-poll open would hold the snapshot until the 30-second stall deadline. If that shows up, the
  fix is a separate "still loading" question for the tool rather than making a fetch invisible to
  the loop.
- **CORS is not applied to the browser's *own* subresources.** Every load this browser makes for
  itself is `RequestMode::Browser`, which is no check at all — correct today, and the thing that
  changes when SRI lands: `<script crossorigin="anonymous">` means a cors-mode fetch for a script,
  and session 15 is where that mode has to start being set from markup.
- **The redirect rule after a preflight is stricter than the specification's.** A preflighted
  request that is redirected is a network error here; the specification allows a re-preflight of
  the new URL. No target site needs it, and spending a granted permission on a URL the server did
  not name is the thing worth being strict about.
- **`PerformanceObserver` is what www.reddit.com's script now dies on**, past the challenge and
  past the `fetch`. It is the next name on that page's list, not a CORS problem.

## Session 14 — fragment parsing · 2026-08-05

**Status:** in_progress — the work landed and is verified; the session's `check` cannot be met by
this session, and it is measuring the wrong thing. See **Found**.

**Check:** `./build/microbrowser/microbrowser_snapshot https://www.reddit.com/ -o /tmp/out.ppm`
printed `143 commands, 3 runs, 1 fonts, 0 images, title "Reddit - The heart of the internet"`.
At `896d353` — the commit before this session — the same command printed
`331 commands, 98 runs, 4 fonts, 10 images`. **The drop is correct**, and the reason is the whole
of this session's finding.

Everything else run and green: `tools/run-checks.sh tests`, `asan` and `ubsan` all
`100% tests passed, 0 tests failed out of 24`; `html_fragment_fuzzer` 1,629,116 runs in 61s with
no finding; `microbrowser_snapshot` on `news.ycombinator.com` (`705 commands, 485 runs, 2 images`)
and `old.reddit.com` (`1079 commands, 641 runs, 19 images`) byte-identical to `896d353`.

**Landed:**

- *The parser, entered with a context element, and a template whose contents are not its children*
  — `html::ParseFragment` (§13.2.6), the "in template" insertion mode (§13.2.6.4.4),
  `dom::Element::Content`, `TreeBuilderTable.cpp`, `fuzz/HtmlFragmentFuzzer.cpp`.
- *innerHTML, and the three things that were silently not being announced* —
  `src/bindings/HtmlParsing.cpp`, `template.content`, and the three bugs under **Found**.

**Found:**

- **`<template>` was rendering its own contents, and reddit's front page depended on it.** The
  element was on the tree builder's unsupported list, so its start tag was dropped and its markup
  became ordinary document content — styled, laid out, its images fetched. On www.reddit.com that
  is 729 and 1668 nodes in two `<template for="s_8a5ed_N">` elements, which the page's own
  `<suspense-replace>` custom element is supposed to hoist. So the sidebar that used to render did
  so **because of a parser bug**, and the drop from 331 display-list commands to 143 is the bug
  being fixed. The feed never rendered either way.
- **The session's check was measuring the wrong thing.** "reddit's feed fills in past the three
  server-rendered posts" requires reddit's own bundle to run, and it still dies where the previous
  session left it: `ReferenceError: PerformanceObserver is not defined`, in the same `data:` module,
  before any `fetch`. Past that it needs the module loader — `Interpreter::SetModuleResolver` is
  synchronous, which session 15's notes already call out as undecided. Fragment parsing is a
  precondition for that check, not a cause of it. A check that measures *this* session is a
  fragment-parsing assertion, and the ones in `tests/TreeBuilderTests.cpp` and
  `tests/DomBindingsTests.cpp` are that.
- **Three bugs in the mutation layer, none from this session.** Appending a `DocumentFragment`
  fired no `connectedCallback` and produced no `childList` record — `InsertNodeBefore` moved the
  children and returned before reaching either, so a framework that assembles its subtree in a
  fragment was invisible to every `MutationObserver`. `ClearChildren` announced nothing either, so
  `el.textContent = ''` disconnected a subtree of custom elements without telling any of them.
  And `CopyNode` had to learn about template contents in the same commit, or
  `template.content.cloneNode(true)` — the idiom its own comment names — would have returned an
  empty template.
- **Upgrade order is load-bearing and not obvious.** `UpgradeElement` fires `connectedCallback`
  itself for an element that is *already* in the document, which is what `customElements.define`
  needs when it walks the page. So a parsed subtree has to be upgraded **before** it is moved into
  the tree, or the reaction fires twice. The walk is by index rather than by range-for, because an
  upgrade runs a page's constructor and a constructor that moves a node invalidates an iterator
  into the list being walked.
- **A fragment parse's open-element stack needs a floor.** Both inputs are chosen by a page, and
  the pair a page will find is the one whose end tags unbalance the stack — `</div></div></body>`
  into a `div` context. Past the bottom, every later node lands in the throwaway document the parse
  builds into and the caller silently gets fewer nodes than the markup described. `stack_floor_` is
  that, every pop goes through `PopCurrent`, and the fuzz target asserts the property directly.

**Left:**

- **`DOMParser` is deliberately absent**, though the session's `scope` names it. It returns a
  *Document*, and `document.getElementById`, `document.body`, `document.head` and `document.title`
  are bound to the binding layer's one document rather than to their receiver — so a second
  Document would answer queries about the *main page*. That is worse than absent (ADR 0012), and
  fixing it means making the whole `document` surface receiver-relative, which is its own change
  and touches everything in `DocumentBindings.cpp`.
- **`PerformanceObserver` is still the wall on www.reddit.com**, unchanged from session 13's note.
  It is on nobody's roadmap and it is what a real reddit check depends on first.
- **Foreign content is still absent**, and `<template>` leaving the unsupported list makes
  `<frameset>` the only member. `TreeBuilder/ReportsWhenItNeededAnUnimplementedInsertionMode`
  changed its subject accordingly, and says so.
- **The list of active formatting elements does not exist.** The spec's `</template>` clause says
  "clear the list of active formatting elements up to the last marker", and there is nothing to
  clear. That is a pre-existing gap — the adoption agency algorithm has never been here — and it
  is what makes `<b><template>` recovery differ from a real browser's.

## Session 15 — CSP, SRI, XHR · 2026-08-05

**Status:** done

**Check:** run against the live sites at `3166596`.

```
MICROBROWSER_PERF_COUNTERS=1 microbrowser_snapshot https://www.reddit.com/ -o out.ppm
  [counters] 2  csp.policies        (no csp.violations line, so: none)
  [counters] 3  engine.scripts_loaded
  https://www.reddit.com/?solution=…&js_challenge=1&…:
    143 commands, 3 runs, 1 fonts, 0 images, title "Reddit - The heart of the internet"

MICROBROWSER_PERF_COUNTERS=1 microbrowser_snapshot https://app.plex.tv/desktop/ -o out.ppm
  [counters] 4  sri.checks          (no sri.mismatches line, so: none)
  [counters] 2  engine.scripts_loaded
  [counters] 2  engine.stylesheets_loaded
  https://app.plex.tv/desktop/: 3 commands, 0 runs, 0 fonts, 0 images, title "Plex"
```

reddit really does serve `default-src 'none'; script-src 'nonce-…'; style-src 'unsafe-inline';
img-src https://www.redditstatic.com; form-action 'self';` — ADR 0020 §3 predicted the shape and it
is exact. Two policies were parsed (the challenge interstitial's and the page's), nothing was
refused, and the page renders identically to before the session. Plex really does serve four
`integrity="sha384-…"` resources, all four with `crossorigin="anonymous"`, and all four verify.

Hacker News and old.reddit.com are unchanged — 705/2 and 1060/23 commands/images, no violations —
which is the check that mattered most: this session changes which resources every page in the world
is allowed to load.

**Landed:**

- *SHA-2 and one base64 decoder, in util, where a policy module can reach them*
- *The page's own policy, parsed and answered, in a module that cannot act on it* — `src/csp`
- *CSP at the four places a resource is named, and `<base href>`, which base-uri needed*
- *Subresource Integrity, and the crossorigin rule that keeps it from being an oracle*
- *XMLHttpRequest, as a shim over the one request path rather than beside it*

**Left:**

- **Gate B is not reached and this session was never going to reach it.** www.reddit.com's own
  bundle still stops at `ReferenceError: PerformanceObserver is not defined`, in the same `data:`
  module sessions 13 and 14 both named, *before* the module loader question. Two things are needed
  and neither is on the roadmap: `PerformanceObserver` (plus `performance.mark`/`measure`/
  `getEntriesByName`, which the same file calls), and a decision about
  `Interpreter::SetModuleResolver` being synchronous. Session 16 is history and the SPA URL; the
  gate needs a session nobody has written down.
- **`base-uri` and `<base href>` are tested and unexercised in the wild.** No page in the
  compatibility set has a `<base>`, so the only coverage is `tests/CspEnforcementTests.cpp`.
- **`report-uri`, `report-to`, `NEL` and Report-Only are refusals, not gaps.** A violation report is
  an outbound request the user did not cause. There is deliberately no entry point that takes a
  Report-Only header, so a page cannot get a *partial* implementation of one.
- **`frame-src` is absent** for the same reason `<base>`'s directive nearly was: there are no nested
  browsing contexts, and a directive that parses and decides nothing reads as enforcement in a
  review of the file. It arrives with ADR 0027.

**Found:**

- **The snapshot tool's `N runs` is text runs on the display list, not script runs.** Session 14's
  notes read it as "3 script runs" and built an argument on it. It is
  `display_list.Texts().size()` (`tools/snapshot/main.cpp:392`). Script counts come from
  `engine.scripts_loaded` under `MICROBROWSER_PERF_COUNTERS=1`, which is also how this session's
  check was phrased. Plex's "0 runs" is a page that drew no text, not a page whose scripts did not
  run.
- **Session 14's check was Gate B's check, and leaving it `in_progress` was a process bug.**
  `tools/agent-loop.sh` picks the lowest unfinished session, so a session whose check can never pass
  makes every future run pick it forever. Its check now names what that session built — the
  fragment-parsing assertions and the fuzz target — and it is `done`. The roadmap's own list of
  failure modes includes "the measurement was measuring the wrong thing"; this is one, and the fix
  belongs in the ledger rather than in a note about the ledger.
- **ADR 0020 §4 says the digest is a `MODULE.deps` question rather than an ADR 0001 one. The answer
  is `util`, not `net`.** The TLS stack's SHA-2 is unreachable from `src/csp`, whose `allow:` line is
  `util url` precisely so that a policy engine cannot open a socket — and CSP's hash-sources need a
  digest. A digest in `net` is one that two of its three callers cannot reach, and reaching it would
  mean widening the line that makes the module worth having. `util::Sha2` is hand-written against
  the FIPS 180-4 vectors plus every padding boundary.
- **`base-uri` had no enforcement point at all, because `<base>` did not exist.** The tree builder
  parsed the tag and nothing read it. Implementing the directive without the element would have been
  a directive that decides nothing, so `<base href>` landed with it — and it is the first thing in
  this browser that changes what a relative URL in a document means, which is why the base is a
  member of `engine::DocumentPolicy` and `Engine` asks the page for it rather than parsing
  `page_.Url()` a second time.
- **Four call sites resolving a URL for a policy decision is four chances to disagree.** That is the
  whole reason `DocumentPolicy` exists rather than three members on `Page`: a stylesheet, a script,
  an image and a `fetch` all ask "may I load this", and each of them had its own idea of what the
  base was.
- **`integrity` on a cross-origin resource with no `crossorigin` had to be a *refusal*, not an
  unchecked fetch.** Without CORS the response would be opaque in a browser with the process split;
  here, where it is not, an integrity check over bytes the page may not read is an oracle for them,
  one guess per reload. So the resource is not fetched at all. Plex sets both attributes on all four
  of its resources, which is what makes this rule cost nothing on the site it was measured against.
- **`'unsafe-inline'` next to a nonce or a hash means nothing**, and getting that backwards would
  make every modern policy in the world permissive. It is stated as its own test rather than
  inferred from the parser.
- **`*` must not match `data:`.** `img-src *` allowing `data:image/svg+xml,<svg onload=…>` is the
  one case in the source-matching table where a wrong answer is an XSS rather than a broken page.
- **`JSON` is a binding in the global *scope*, not a property of the global object.**
  `interpreter.Global()->Get("JSON")` returns null; `GlobalScope()->Lookup("JSON")` is the way in.
  `globalThis.JSON` answering correctly is what makes it look like a property, and it cost twenty
  minutes on `xhr.responseType = 'json'`.
- **The class-budget lint counts `= {}` as a data member.** A default argument of `{}` puts the
  brace at depth 1, which resets the statement start, so the `) const;` that follows is counted as a
  field. `= std::string_view()` is the spelling that does not. Worth knowing before the next budget
  argument, because the symptom is a class that appears to have one more member than it has.

## Session 16 — history and the SPA URL · 2026-08-05

**Status:** done

**Check:** the origin half, which is the load-bearing half, run at `bd3b7ce`:

```
./build/microbrowser/microbrowser_tests History
  18 test(s) run, 0 failed
```

Five refusals in one assertion — a different host, a different port, a different scheme, a `data:`
URL and a `javascript:` URL — each of which looks like it should work and must not, and after all
five `location.href` is still `https://page.example/start`. Plus: a same-document traversal fires
`popstate` and makes no request (one request in the log, the document); a cross-document one loads
and leaves the forward entry where it was; `history.state === history.state`; a state carrying a
function is a `DataCloneError` and the URL does not move.

Hacker News, old.reddit.com, en.wikipedia.org and www.reddit.com all render exactly as before.
old.reddit's `1051 commands, 21 images` against session 15's `1060/23` is that site's own content
changing between requests — checked by running the pre-change binary against it, which gives 1051/21
too.

**The check's first clause was rewritten**, for the same reason session 14's was: "reddit's route
changes update the URL bar" needs reddit's own bundle to run, and it stops at
`PerformanceObserver is not defined`. That is now **session 50** in the ledger.

**Landed:**

- *The structured clone algorithm, as bytes, because a history entry outlives its document*
- *Session history moves to where the documents are, and pushState cannot move the URL bar
  off-origin*

**Left:** ADR 0026's own separate pieces — `window.navigation` (§5), which is a layer over
`pushState` and therefore a session of its own; `beforeunload` and the unload ordering (§3 steps 2
and 4); `scrollRestoration`, absent rather than a settable string that changes nothing.

**Found:**

- **ADR 0026 §1's "structured-clone bytes, not a live object" is a precondition, not a detail.**
  Nothing in this repository could serialize a value, so the history entry could not be written
  before this existed. It also turns out to be most of session 38 and all of ADR 0021's storage
  format, which is why it landed as its own commit rather than inside a history change.
- **A Set's `#entries` holds one-element arrays, not bare values.** `Collections.cpp` builds a
  Map's pair and a Set's member with the same `NewArrayValue`, so a serializer that wrote the
  entry directly produced a clone whose `size` was right and whose `has` was false — a wrong
  answer that no test of the size or the contents would have caught.
- **Anything a deserializer allocates has to be rooted while it is being built.** Rebuilding a Map
  calls the page's own `set`, the collector runs at every call, and a `js::Value` in a C++ field is
  invisible to it. A JavaScript array hung off the global holds the half-built graph, which is the
  same fix `src/bindings` uses for the same reason. This was a segfault, not a wrong answer.
- **`Object::Get` hands back a pointer into a property table.** Holding one across a call into the
  page is holding it across a possible collection and rehash. Two call sites here copied by value.
- **A test's failure message is evaluated eagerly.** `Expect(ok, "…" + ToString(value))` on a
  self-referential array recurses until the stack ends, so a test file about cycles cannot
  stringify its own fixtures unconditionally. The crash looked like a bug in the code under test.
- **`pushState` left `location.pathname` stale, and that was the first bug the tests found.** The
  `location` object is materialised once at install from a string the binding layer copied, so a
  same-document navigation has to rewrite it — *in place*, not by replacing it, because a page holds
  a reference to it and `document.location === window.location` is something pages check.
- **`base-uri` had no enforcement point, and `<base href>` has to outrank `pushState`.** An element
  in the document beats the address, so a `pushState` on a page with a `<base>` must not silently
  retarget every relative URL on it. `DocumentPolicy` now records which of the two set the base.
- **An in-page anchor was a full reload of the page you were already on.** Clicking a table of
  contents refetched the document. It is now a history entry, a fragment, and a `hashchange` — and
  that is the same code path a `pushState` traversal takes, which is why it cost nothing.
- **Three sessions in a row have been blocked by the same missing binding**, and the roadmap
  sequenced five features in front of it. Sessions 13, 14, 15 and 16 each had a check clause
  requiring reddit's bundle to run, and each stopped at `ReferenceError: PerformanceObserver is not
  defined` in the same `data:` module. Session 50 is now that work, written down with what reddit
  actually calls: `performance.mark`, `measure`, `getEntriesByName`, `getEntriesByType`,
  `PerformanceObserver.supportedEntryTypes`, and the entry types `navigation`, `resource` and
  `longtask`. **A roadmap that never names the thing four of its sessions are blocked on is the
  failure mode `docs/roadmap-to-any-page.md` calls "the measurement was measuring the wrong
  thing".**

## Session 50 — PerformanceObserver and the module loader · 2026-08-05

**Status:** done

**Check:** the session's first clause, run at `5c8370e`:

```
MICROBROWSER_PERF_COUNTERS=1 microbrowser_snapshot https://www.reddit.com/ -o out.ppm
  [counters] 3  engine.scripts_loaded
  [counters] 7  performance.entries
  [counters] 3  performance.observer_callbacks
  https://www.reddit.com/?…: 143 commands, 3 runs, 1 fonts, 0 images,
    title "Reddit - The heart of the internet"
```

**No `script error:` line at all**, where four previous sessions each got one.
`js.compile_bailouts` is zero. Hacker News, old.reddit.com and wikipedia render unchanged.

**Landed:**

- *performance and PerformanceObserver, and the compiler bug they made visible*

**Then the module loader landed too**, and www.reddit.com went from `143 commands, 3 runs, 1 fonts,
0 images` to **`210 commands, 48 runs, 3 fonts, 1 images`** — the search box, the user menu and the
sidebar card ("Join the most real place on the internet", with its User Agreement and Privacy Policy
links) all render where the page was blank. Looked at as an image, not inferred from counters.

**Left: Gate B, and it is not the module loader.** reddit's feed still does not fill in, and
`js.dynamic_imports` is **zero** — its own `import()` calls never execute, because the paths that
reach them are behind `window.Sentry?.…` and `requestIdleCallback`. The feed lives in two
`<template for="s_…">` elements that the page's own `<suspense-replace>` custom element is supposed
to hoist; session 14 established that those hold 729 and 1668 nodes and that rendering them directly
was a parser bug. That custom element is where a next session should look.

**Found:**

- **A compile bailout was invisible, and that invisibility was the bug's hiding place.** `Compile`
  returning null is not a fault — the tree-walker takes the program — but the tree-walker *refuses
  an async function at the call*, so a program that bailed out fails later, somewhere else, with an
  error naming neither the bound it hit nor the file it was in. Four sessions read that TypeError as
  "the tree-walker ran this" and stopped. Six `js.compile_bailout_*` counters now name the reason,
  and **two of the five reasons are defects rather than bounds** — which is the distinction that
  turned a day's guessing into two minutes.
- **The bug: `ReservePattern` and `BindTarget` disagreed about what a destructuring default looks
  like.** A pattern is read with the *expression* grammar, so the default in `{a: c = 1}` and
  `[c = 1]` arrives as a plain `Assignment`; only the shorthand `{c = 1}` and an arrow's parameter
  list get rewritten to `AssignmentPattern`. `BindTarget` knew, and had the comment saying so.
  `ReservePattern` did not, so the name behind the default was never reserved, `DeclareSlot` found no
  slot, and the compile of the **entire program** was abandoned. Minified code writes
  `({renderBlockingStatus: c = ""}) => …` constantly, so this fired on any real bundle.
- **`supportedEntryTypes` is a page's only honest way to ask what a browser measures**, and reddit
  reads it: it observes `longtask` only when the list contains it. That is why `longtask` is absent
  here rather than present-and-silent — the list is the mechanism ADR 0012's rule works through.
- **Entries produced before the first script runs would all have been lost.** Every subresource of a
  document completes *before* its first script — that is what render-blocking means — so a
  `resource` entry has no heap to live in when it happens, and the entries a page observes with
  `buffered: true` are exactly those. They are held as plain C++ data and flushed at install.
- **The `navigation` entry arrives after the last paint of the load**, so the frame that would have
  delivered it has already happened. `AdvanceLoad` delivers once more after the load ends; without
  that, a page observing `navigation` on a settled page hears nothing ever.
- **`jsshell` could not tell you which engine ran your file.** It dumps the counters now, which is
  how the bug above was minimised: `MICROBROWSER_PERF_COUNTERS=1 jsshell file.js` and a
  delta-debugging script over the top-level statements took it from 19KB to
  `function f({a: c = ""}) { return c }`.

## Session 49 — @media · 2026-08-05

**Status:** done

**Check:** three sites, at two widths, at `2e3e736`:

```
width  site                                        commands / images
1280   https://old.reddit.com/                     1045 / 20
 500   https://old.reddit.com/                     1079 / 20
1280   https://en.wikipedia.org/wiki/Web_browser   2827 / 9
 500   https://en.wikipedia.org/wiki/Web_browser   2921 / 8
1280   https://news.ycombinator.com/               705 / 2
 500   https://news.ycombinator.com/               714 / 2
```

Every one of them **differs between the two widths**, which none of them did before, and none
regresses at 1280. Plus 13 assertions in `tests/MediaQueryTests.cpp`.

**Landed:**

- *@media, wired to the evaluator that has existed since session 6*

**Left:** the design rather than the feature. The prelude is evaluated when the sheet is *parsed*, so
`Page::SetViewport` re-parses the author sheets when the viewport actually changes. Keeping the
condition on the rule and asking it during the cascade is the end state; it also makes
`window.matchMedia` and a `@media` that changes under a resize without a re-parse possible, neither
of which exists yet.

**Found:**

- **The whole bug was a missing call.** `css::MediaQueryListMatches` has answered this exact grammar
  — `and`/`or`/`not`, the comma list, width/height/orientation/resolution — since `srcset` landed in
  session 6. `MediaListItemMatches` next door accepted a single Ident. Two functions for one
  question, one of them a placeholder, and nothing pointed from one to the other. ADR 0014 called
  `@media` supported at 791 occurrences on the strength of the placeholder.
- **The default context had to be the *old* answer, not a sensible one.** A zero-sized viewport
  matches `max-width` and not `min-width`, which is precisely what every parenthesised prelude got
  before — so the user-agent sheet and every test that parses a sheet without a viewport keep the
  behaviour they had. A "sensible" default of 1280 would have quietly changed the meaning of every
  such sheet.
- **`@media { … }` with an empty prelude applies.** An empty media query list evaluates to true,
  which is the same rule that makes `sizes="100vw"` a valid entry with no condition in front of it.
  A test asserted the opposite first, and the implementation was right.
- **The prelude reaches the evaluator through `Reconstruct`**, the serializer a declaration's value
  already uses. A second token-to-text function is a second set of answers about what
  `(min-width:600px)` says.

### Session 50, second half — the module loader · 2026-08-05

**Landed:** *The module loader, split the way the two kinds of import actually differ*

**Found:**

- **The split *is* the design, and neither half works as the other.** A static graph can be closed
  before evaluation, which is what lets `SetModuleResolver` stay synchronous — parse each module for
  what it names, fetch that, repeat, and only then evaluate. A dynamic `import()` cannot: a page
  reaches one whenever it reaches one, so the promise goes back pending and is settled on a later
  turn. ADR 0011 asked which of the two to build; the answer is both, because they are not the same
  question.
- **A bare specifier must resolve to *nothing*.** `import "react"` resolved to
  `https://page.example/react` at first, because a relative-URL resolve against the document accepts
  anything. There is no import map here, so that was a request to a URL the page never named — and
  the rule is that a specifier is a full URL or starts with `/`, `./` or `../`.
- **The graph is populated before the interpreter exists.** A module script's source arrives through
  `AddFetched`, which happens while the load is still running; the resolver is installed later, at
  `EnsureInterpreter`. So the install must *not* clear the graph — the first version did, and threw
  away exactly the sources it was about to be asked for — and the document URL has to be set at
  parse time, or asking a module what it imports has no base to resolve against.
- **A `data:` referrer is not a base.** `new URL("./x", "data:…")` means nothing, so a relative
  import inside a `data:` module resolves against the document, which is what a browser does with
  one. reddit's entry point is a `data:` module, so this is not a corner.
- **A pending import's promise has to be rooted where the collector can see it.** The host holds a
  raw `Object*` while the fetch is in flight, and a raw pointer is *worse* than invisible to a
  collector: it survives the sweep that freed its target. It lives in a JavaScript array on the
  global for the same reason the fetch table does.

## Session 17 — Shadow DOM, the tree · 2026-08-05

**Status:** done

**Check:** 13 assertions in `tests/ShadowDomTests.cpp`; 1539 tests, 0 failed; asan clean. Hacker
News, old.reddit.com, www.reddit.com and wikipedia render unchanged (705/2, 1050/20, 210/1,
2827/9 commands/images).

The session's original check — "youtube.com's home page shows video thumbnails and a masthead" — is
**sessions 17 and 18 together** and was rewritten, for the reason session 14's and 16's were: it
needs the scoped cascade, which is session 18. www.youtube.com currently renders 16 display-list
commands and 0 text runs.

**Landed:**

- *The flattened tree, as a traversal, and the shadow roots layout walks through it*
- *Event retargeting, and composedPath as the thing it hides*

**Left:** session 18 — the scoped cascade. The concrete blocker is small and worth naming: a
`<style>` **inside a shadow root is not collected**, because `CollectStyleSheets` walks the document
and a shadow root is deliberately not reachable from it. So a component that styles itself renders
unstyled today. `:host`, `::slotted()`, `adoptedStyleSheets` and `::part` follow. Declarative shadow
DOM (`<template shadowrootmode>`) is also still absent, and ADR 0019 §1 groups it with this session.

**Found:**

- **`textContent` on a Text node was the empty string.** `Node::TextContent()` walked *descendants*,
  and a Text node has none. The DOM says it is the node's data, and a caller that asked a node it had
  not type-checked got a silent "". Four of these tests found it, which is the argument for asserting
  on values a page can read rather than on internal structure.
- **The three cases a materialised flat tree gets wrong each needed their own assertion**, because
  none of them is visible in the common one: a host with no `<slot>` renders none of its own
  children; a slot's fallback is *conditional*, so a tree built once is stale the moment a matching
  child appears; and an assigned node appears exactly once — at the slot, not also where it is
  written.
- **Retargeting is not tidiness.** Without it a listener on the page receives a `target` it could
  never have obtained a reference to: the component's internal shape leaks, and the page holds a node
  it cannot compare against anything of its own. And it must apply *only* where a boundary was
  crossed — an ordinary light-DOM event keeps its own target, which is the assertion that would have
  caught an unconditional implementation breaking every existing page.
- **A shadow root must not have a parent.** Giving it one would make it reachable by every ordinary
  tree walk — the cascade, the script collector, `querySelectorAll`, the image loader — and being
  unreachable that way is the entire point. The one link back is `DocumentFragment::Host()`, which is
  what dispatch crosses and what tells a slot which children it may be filled from.

## Session 18 — Shadow DOM, the scoped cascade · 2026-08-05

**Status:** done

**Check:** 21 assertions in `tests/ShadowDomTests.cpp`; 1547 tests, 0 failed; asan and ubsan clean.
Hacker News, old.reddit.com, www.reddit.com and wikipedia unchanged (705/2, 1050/20, 210/1, 2827/9).

The shared 17/18 check — youtube's home page — is **still not met**: www.youtube.com renders 16
display-list commands and 0 text runs. It is no longer the cascade. Its shell is two custom elements
that build the page from script, which is the same shape as reddit's `<suspense-replace>`.

**Landed:**

- *The scoped cascade, and the three ways a shadow tree was invisible*

**Left from ADR 0019:** `adoptedStyleSheets` and constructable stylesheets (§4), `::part` (§6), and
declarative shadow DOM (`<template shadowrootmode>` — §1 groups it with session 17).

**Found — and this is the whole entry, because the feature was four lines and the bugs were not:**

- **A `<style>` inside a shadow root was never collected.** `CollectStyleSheets` walks the document,
  and a shadow root is deliberately unreachable from it — which is the point of it. So a component
  that styled itself rendered unstyled, and no test anywhere would have caught it, because nothing
  could put a `<style>` in a shadow root before this session. Collected at layout now, which is the
  one point that runs after a batch of mutations and before the cascade reads anything, and compared
  by *text* so an unchanged component costs one walk rather than a re-parse.
- **A mutation inside a shadow tree never reached the document's mutation version.**
  `Node::OwnerDocument` walks parents; a shadow root has none. So `root.innerHTML = …` bumped
  nothing, `EnsureLayoutClean` saw a clean layout, and **a component could rewrite itself with no
  effect on the screen**. It crosses through `DocumentFragment::Host()` now — the same one link event
  retargeting uses, which is the argument for having exactly one.
- **`getComputedStyle` skipped `EnsureLayoutClean` for a computed value.** That was correct while only
  a node move could change the cascade. It stopped being correct the moment a `<style>` could live in
  a shadow root: the cascade can now change without a node moving.
- **`root.innerHTML = …` did nothing**, because `innerHTML` was installed on Element and a shadow
  root is a `DocumentFragment`. It is how every component fills one. Its fragment-parsing *context
  element* is the **host**, which is ADR 0020 §6's rule reaching a place ADR 0019 created.
- **Inheritance and matching had to disagree, deliberately.** `StyleWithoutBox` walked parents, so a
  node in a shadow tree inherited from nothing and `getComputedStyle` inside a component answered
  with the initial values. It walks the *flat* chain now — which is ADR 0019 §3's sentence made true:
  the cascade is scoped and inheritance is not.
- **`:host` and `::slotted()` are the only two selectors that leave their tree, and they go opposite
  ways.** Neither belongs in `Selector::Matches`, which is a pure function of (element, selector);
  "which root did this rule come from" is neither. `StyleResolver::ScopeAdmits` answers both, and the
  bare `:host` needed its own selector *kind* rather than a pseudo-class name the matcher
  special-cases — otherwise the purity rule would have been broken to make it work.

## Session 19 — web fonts, @font-face · 2026-08-05

**Status:** done

**Check:** 6 assertions in `tests/WebFontTests.cpp` plus 5 in `tests/CssTests.cpp`; 1558 tests, 0
failed; asan clean. Hacker News, old.reddit.com and wikipedia render unchanged.

**Landed:**

- *@font-face, parsed as the descriptor block it is*
- *A declared face is fetched and registered, and a WOFF2 is not fetched at all*

**Left:** `font-display` is parsed and not honoured — *when a browser paints* is a paint decision and
belongs where the first paint is gated, not in the font loader. `unicode-range` is recorded as a
bool rather than a range, for the reason below.

**Found:**

- **The measurement that says where ADR 0024's value actually is:** `gfx.web_fonts_registered` is
  **zero** on Hacker News, old.reddit.com and wikipedia. None of them has a *decodable* `@font-face`,
  because the web ships WOFF2. So this session built the machinery and session 20 — brotli and the
  WOFF2 container — is what makes it visible. The machinery is what will make that landing observable
  the moment it happens, which is the right order: a decoder with nothing wired to it proves nothing.
- **`Engine::IsLoading()` had gained `module_fetches_` and not `font_fetches_`.** So the loop stopped
  turning while a font was in flight and the request never went out — zero requests with the face
  parsed perfectly, which is exactly the shape that sends you looking at the parser for an hour. The
  probe that found it printed the parsed face and the started request id, and the absence of a
  *third* probe line was the answer. Worth remembering: when a fetch is started and nothing arrives,
  the question is not "was it refused" but "did anything ask the loop to keep going".
- **`unicode-range` cannot be kept as text at this layer.** A declaration value arrives reconstructed
  from tokens and `U+0000-00FF` comes back as `U00FF`. Keeping the mangled string would be keeping a
  lie the next reader trusts, so what is kept is the one thing that is true: the face covers part of
  the alphabet and is therefore not a complete substitute for the family.
- **`local(...)` is skipped rather than answered**, and that is a privacy decision: answering it from
  the system font database would let a page ask which fonts are installed, which is the
  fingerprinting surface ADR 0029 prices separately. The URL sources beside it still work.

## Session 20 — brotli, and the WOFF2 container · 2026-08-05 (brotli done)

**Status:** in_progress — brotli landed; the WOFF2 container has not.

**Check:** the measurement, not a test:

```
MICROBROWSER_PERF_COUNTERS=1 microbrowser_snapshot \
  'data:text/html,<script src="https://cdnjs.cloudflare.com/ajax/libs/jquery/3.7.1/jquery.min.js">…'
  [counters]  28462  net.bytes_received
  [counters]  87533  util.brotli_bytes_produced
  [counters]      1  util.brotli_streams
```

3.1x, on a resource that arrived **uncompressed** before this — because we did not advertise the
coding the server already had on disk. Plus 10 assertions in `tests/NetTests.cpp` and 400k fuzz runs
with no finding; 1559 tests, 0 failed; asan clean.

**Landed:**

- *Brotli, which Content-Encoding needed as much as WOFF2 does*

**Left:** the WOFF2 container, which is the harder half. Its shape is known and worth writing down: a
WOFF2 file is a table directory plus **one brotli stream holding every table concatenated**, and
`glyf`/`loca` are stored *transformed* — the outlines are re-encoded and `loca` is dropped entirely,
so both have to be **reconstructed** rather than copied. That reconstruction is the bulk of the work,
it is a hostile-input parser, and it lands with its own fuzz target on the same commit.
`gfx::FontCatalog::RegisterWebFont` and `Page::CanDecodeFontFormat` are the two places that then stop
refusing `format("woff2")` — session 19 built both so this landing is one line each and immediately
visible in `gfx.web_fonts_registered`.

**Found:**

- **Brotli's bound cannot be the gzip bound, and that is the whole reason it is a separate
  function.** gzip carries ISIZE, so a bomb is refused from its own claim before a byte is produced.
  A brotli stream declares nothing, so the ceiling has to be enforced *during* the decode — checked
  before each chunk is kept, not after. A brotli bomb therefore reads as `Malformed` where a gzip bomb
  reads as `TooLarge`, and that is a diagnostic difference rather than a decision.
- **A refusal must empty its output.** "Fails rather than truncates" is only true at the call site if
  the buffer is empty, so the fuzz target asserts `!ok ⇒ out.empty()` alongside the ceiling. The
  target is deliberately not doubting the third-party decoder — it is checking the bound, which is
  ours.
- **Wikipedia serves gzip even when `br` is offered first.** So does redditstatic. The saving is on
  CDN-fronted assets (cdnjs served `br` immediately), which is where the bytes are — and it means the
  three rendering sites' `net.bytes_received` is unchanged by this, which would look like the feature
  doing nothing if the measurement had not been taken against a host that actually serves it.

### WOFF2, the transformed `glyf`, and `unicode-range` · 2026-08-06 (session 20 finished)

Three commits after the brotli one: `a1963e4` the container plus the transformed
`glyf`, `b7c46a9` `unicode-range`, `270aca7` the sfnt directory check.

**The first cut of the container refused a transformed `glyf`, and the refusal was
worthless.** The reasoning behind it was sound in isolation and is written in the
diff it replaced: reconstructing `glyf` means rebuilding every outline from seven
parallel substreams, and a half-reconstruction is mangled glyphs rather than a
failure — a bug nobody can attribute to the font. What made it worthless is a
measurement rather than an argument. `fonts.gstatic.com` transforms `glyf` on every
face it serves, and so does every file the reference compressor produces, so the
container accepted nothing anyone actually ships. **A refusal that is honest about
one file and wrong about the whole web is still wrong.**

What made the reconstruction safe to write was an **oracle rather than care**.
fontTools implements both halves of WOFF2 independently, so its reconstruction of a
real font is a reference answer: 807 glyphs of Inter's variable font and 518 of its
latin subset (312 of them composite), compared outline by outline — coordinates,
on-curve flags, contour ends, bounding boxes and hinting programs — with no
mismatch. That is the only reason the 128 coordinate encodings could be written as
the arithmetic that generates them (which is how the reference decoder writes them)
rather than as 896 transcribed numbers where a typo is indistinguishable from a
specification detail.

fontTools needs `python-brotli`, which pip refuses to install here (PEP 668). A
12-line `ctypes` shim over `libbrotlidec.so.1` and `libbrotlienc.so.1` — just
`decompress`, `compress` and the three mode constants — was enough for its reader
*and* its writer, which is also how the test fixture was generated. **An unavailable
reference implementation is often one shim away from being available.**

**Web fonts had never registered outside the tests, and session 19's check could not
have seen it.** `FontProvider::RegisterWebFont` returns false by default so that a
provider with no way to load bytes refuses honestly. `platform::SystemFontProvider`
— the provider every real binary constructs — inherited that default and never
overrode it. `tests/WebFontTests.cpp` builds a `gfx::FontCatalog` directly, which
*does* override it, so every assertion passed while `@font-face` did nothing in the
browser. It surfaced by rendering a page with a real gstatic face and watching a
face that parsed perfectly, fetched successfully, and then simply was not there.
**A test that constructs its own collaborator cannot see a missing override in the
one the binary uses** — and the fix belongs where the real object is, not in the
test.

Diagnosing that took a wrong turn worth recording: `gfx.web_fonts_refused` was
incrementing with none of the `gfx.woff2_*` counters moving, which reads as "the
decoder refused it". Both refusal sites had to be found by grep before the absence
of a `[probe]` line — added at the first of them — proved the code never reached it.
A counter incremented in two places answers a different question than the one being
asked.

**Two `unicode-range` bugs came from a real stylesheet and neither was reachable
from a unit test.** Google's
`css2?family=Inter:wght@400;700&family=Roboto` is 23 `@font-face` blocks differing
only in this descriptor. First run: 4 requests where an independent count said 3
should match. The extra was Roboto's *symbols* subset, whose range list begins
`U+0001-000C` — and the page's code points included the newlines inside the
`<style>` element's own CSS text. Every page with a `@font-face` block was fetching
an emoji font because of the whitespace in its stylesheet. Second: `wght@400;700`
names the *same* file twice, because a variable font serves both weights, and the
request key included the weight. 23 faces are now 2 requests and 3 registrations.

The parse side is a lesson in where a fix belongs. Session 19 recorded only
*whether* a face had a range, with a correct explanation: `U+0100-02BA` cannot be
read back out of a reconstructed declaration string, because by then it has been
through generic tokens and is an ident, a number and a dimension with the leading
zeros and the hex reading gone. The conclusion drawn — keep a bool — was the wrong
half of the fix. **The information was destroyed one layer lower**, so
`Token::Kind::UnicodeRange` scans `<urange>` where the original text still exists,
and every consumer above it gets both ends resolved, wildcards expanded.

**The fuzzer's new invariant found two bugs, both table confusion rather than memory
safety.** The invariant is narrow and had to be: *a reconstructed* `loca` describes
the `glyf` that was built. It cannot apply to an untransformed font, whose `loca` is
copied out of the file and is free to be nonsense — that is the font's bug, not the
decoder's. Which case happened is a question `ReadPerformanceCounter` already
answers, so the fuzzer asks the counter rather than growing a second directory
parser. What it caught: a **duplicate tag** in the WOFF2 directory (two `glyf`
entries make "which one" a question, and first-wins versus last-wins is the shape of
half of this format's CVE history), and a transformed font with **no readable
`head`** — where short-versus-long `loca` is declared, which this decoder decides
itself because it re-encodes outlines slightly larger than the original and can be
forced from short to long.

Numbers. A 23,664-byte woff2 becomes 67,136 bytes of sfnt and draws
`Hamburgefonstiv 123 & WOFF2 reconstructed` in Inter. youtube.com went from 0 web
fonts registered to **11** — and still renders 16 display-list commands with no
text, which is the useful part of that number: the font pipeline is not what blocks
it. 6.3M woff2 fuzz runs and 2M sfnt runs, ASan and UBSan clean, 1576 tests.

Still refused: the `hmtx` transform, which no measured font uses, and which would
mean reading every glyph's bounding box back out of the `glyf` just rebuilt.

### `transform` and stacking contexts · 2026-08-06 (session 21)

Four commits: `a644836` the property, `6dbb25a` the display-list command,
`04e4f8d` stacking contexts and `z-index`, `8e9affc` hit testing.

**The value is stored as the operations the author wrote and not as a matrix**, and
the reason generalises: a matrix cannot hold `translate(50%)`, whose percentage
resolves against the box's own border box — something the cascade does not know and
layout has not decided. The second reason is session 35's: interpolating
`rotate(0deg)` to `rotate(90deg)` means interpolating the *rotation*, because
interpolating those two matrices component-wise gives something that is not a
rotation at all. `Length` moved into its own header to break the include cycle that
storing a transform on `ComputedStyle` created — a length was the one type in that
file with no dependency on the rest of it.

Two error rules, both the specification's and both in the safe direction. One
unparsable function drops the whole declaration, because half a transform puts the
box somewhere the author never wrote while a dropped one leaves it where layout put
it. And **3D is refused rather than flattened**: `rotateY(90deg)` flattened to 2D is
a box at full width where the page meant an edge-on sliver, which is a wrong page
rather than a missing effect. Flattening is the tempting thing for a 2D engine to do
and it is the wrong thing.

**How much of this already worked.** Paths and text go through the rasterizer, which
has taken a transform since M1 — so rotated and skewed *text* worked the moment the
command existed, glyph outlines and all. What did not: images honoured only the
translation, and now sample through the inverse matrix (backwards, destination to
source, because walking the source forwards leaves holes between the pixels it
writes). A clip is still the bounding box of the rotated rectangle, because a canvas
clip is a rectangle and the alternative is a per-pixel mask; it errs wide, which
keeps a page readable rather than blank.

The matrix is named by index into a side table, like a path, because six floats plus
the variant tag is over `DisplayCommand`'s 24-byte budget and that budget is paid on
every command on the page. A first version dropped an *identity* push to save two
commands and a test caught it within a minute: dropping the push made it conditional
while the pop stayed unconditional, and a pop that outlives its push restores a
transform the list never saved — which is how the page ends up painted at the wrong
origin under the browser chrome. Whether an identity transform is worth a pair of
commands is the builder's question, and the builder does not emit one.

**The stacking-context model took two wrong versions, and both were found by
rendering rather than by a test.** The distinctions are subtle to state and not at
all subtle to see:

1. **A unit is not a context.** A positioned or transformed box is *collected* into
   its ancestor context's z-ordered buckets; it becomes a context itself only with a
   transform or an explicit `z-index`. The first version let every ancestor collect,
   which paints each descendant once per ancestor — a page drawn three times over,
   visible immediately in `-v` as the same three commands repeated.
2. **The collect walk descends through a non-context unit.** `z-index: auto` means
   "order me, but let my own positioned descendants be ordered against my siblings"
   (CSS 2.1 E.2). Stopping at the unit left those descendants collected by nobody,
   and old.reddit.com went from 1055 display-list commands to **40** — because nearly
   everything on that page is inside a positioned box with no z-index. The command
   count was what caught it; the page still had a plausible-looking header.

Collecting has to be a separate walk from painting, and that is the shape of
Appendix E rather than a preference: a negative layer paints *before* the in-flow
content of the box it is in, and the units that belong in it can be arbitrarily deep
in subtrees whose content paints after. One traversal cannot produce that order.

The two-pass sticky/fixed hoist this replaces had a comment saying that hoisting
`relative` and `absolute` too had been tried and broke old.reddit.com. That comment
named this session, and it was right about the cause: ordering between subtrees is
what a stacking context decides. Both of its cases now fall out of the general rule.

**An anonymous box carries a copy of its parent's style.** A transformed `<div>` with
text in it pushed the same matrix three times — once for the div and again for each
generated wrapper — and applied it three times over. Emission is restricted to boxes
that came from an element. This is the second time this session that "a generated box
looks like its parent" cost an hour; the first was session 19's text boxes carrying a
background.

Hit testing un-maps the pointer through the inverse matrix, in `UntransformedPoint`
beside `PointInside`, because that function is already the single answer to "how does
a point change as it enters this box" — scroll offset and clipping were already
there. All three walks map at entry rather than at their own rectangle test, since a
transformed box's *own* border box has to be tested against the mapped point too. A
matrix with no inverse hits nothing, matching what the painter does with it: `scale(0)`
paints nothing, and a hit test that answered would be an invisible element eating
clicks.

**youtube.com is still 16 display-list commands, and its blockers are now named** —
`performance.timing.responseStart`, `canvas.getContext`, and a `prototype` read in
its webcomponents polyfill. None of them is `transform`. That is what the check for
this session was measuring against and it belongs to Gate C, not here.

### Storage, partitioned · 2026-08-06 (session 22)

One commit, `f4dd125`. A new `src/storage` module, `bindings::StorageSource` declared
in `src/bindings`, and the engine implementing it.

**The seam is the whole design and it is worth stating as a rule.** `src/bindings` may
see `util`, `js`, `dom` and `html` — not `url`, not `storage`. So a binding chooses
`Session` or `Local` and *nothing else*: the partition key comes from the document's
own URL on the engine side, in one function. ADR 0021 §1 requires the key on every
store, and the way to require that of a caller is not to check it — it is to give the
caller no way to spell one. `PartitionedStorage` joined the architecture lint's
partitioned types on this commit, which is what makes the rule survive the next store.

**The bug worth remembering is the one that changed the design.** An opaque origin — a
`data:` URL, `about:blank` — has no site and therefore no partition, and the first
version answered that with a per-operation `SecurityError` from the `Proxy` traps, which
is what Chrome and Firefox do. It did not work, and the reason is an engine bug this
found by accident: **`Interpreter::GetProperty` returns `Value` and has nowhere to put
an abrupt completion**, so three lines read `got.IsAbrupt() ? Value::Undefined() :
got.value` and an exception thrown by a `get` accessor or a `Proxy` trap is silently
swallowed. The page saw `TypeError: undefined (setItem) is not a function` instead of
the `SecurityError` that had been thrown one frame earlier.

That affects plain accessors as well as proxies, which makes it the largest remaining
JavaScript conformance gap here, and it is not a local fix: `GetProperty` has 73 call
sites, and the choices are to return a `Result` from all of them or to latch an
in-flight exception on the interpreter — which needs a GC root, because a `js::Value` in
a C++ field is invisible to the collector. It is item 8 in
`docs/js-conformance-roadmap.md` now, with that cost written down.

The storage answer went the other way and is better for it: **neither name is declared
for an opaque origin**, decided once at install time. That is ADR 0012's rule, and it is
the answer that survives feature detection — `if (window.localStorage)` takes the
fallback path, where an empty store that accepts writes and forgets them is a page that
believes it saved.

Three things live in `StorageArea` rather than at a caller, each for a reason a caller
would get wrong. Insertion order, because `key(n)` and `length` are API and a hash map
cannot answer `key(0)` at all. The quota, because storage is memory and unbounded
storage from a page is a denial of service against the process — a security bound with
an opt-out is not one. And atomicity: a write that would exceed the quota changes
nothing, because a page that catches `QuotaExceededError` and retries must not find a
half-written value.

The binding is a `Proxy`, for the reason `element.style` is one. `localStorage.theme` is
more common in the wild than `getItem('theme')` and a page mixes the two freely, so both
have to be one store; `Object.keys` returns the stored keys and not `length` or the
methods; a missing key is `undefined` as a property and `null` from `getItem`, which is
the specification's asymmetry and not an oversight.

Two small things worth keeping. `Value::Bool`, not `Value::Boolean` — and a native
function must **return** `call.Throw(...)`: calling it and then returning a value has
thrown nothing, which is how the quota test failed the first time. And
`microbrowser_snapshot` prints the page's own `console.log` lines now, because half the
questions asked of that tool are answered by a line the page already prints — including
this session's.

**Plex's first inline script finds `sessionStorage`.** `storage.lookups 2`,
`storage.writes 1`, `storage.partitions_created 2`, where before it found nothing. Its
main bundle now fails later, on `TypeError:  is not a function` with an empty callee
name — the next thing to chase on that site.

### WebSocket, the first two parts · 2026-08-06 (session 23, unfinished)

`fd743cf` the wire format, `5fd73bd` the connection. The engine table, the `WebSocket`
binding and `EventSource` are not built; the ledger entry says so and says in what
order.

**SHA-1 exists in this tree now, and the header is mostly an argument against using
it.** RFC 6455's `Sec-WebSocket-Accept` is SHA-1 of the client key plus a fixed GUID,
which is a *protocol handshake check* rather than a security claim: it proves the peer
speaks WebSocket, and `wss://` is what proves anything about trust. So the rule written
in `util/Sha1.h` is that a second caller is a bug, because what a second caller almost
certainly wants is collision resistance and this does not have it.

Three rules in the codec are refusals, and none of them is refused merely because the
RFC says so:

- **A masked server frame.** Masking is not confidentiality — it defeats proxy cache
  poisoning — but the *direction* is load-bearing: accepting a masked server frame means
  accepting a frame a proxy could have rewritten.
- **A redundant length form.** A server that writes 5 in the two-byte form is legal by
  the letter of §5.2 and is refused here for the reason WOFF2's base-128 refuses leading
  zeros: a second spelling of a length is a second way for two implementations to
  disagree about what a frame is. The encoder writes the shortest form and a test asserts
  the two agree — otherwise this browser produces frames it would itself refuse.
- **A control frame that is fragmented or over 125 bytes**, and a `close` with one
  payload byte, which would hand a caller half a status code.

The decode result's `Failed` versus `Incomplete` is the bound doing its work rather than
an enum with three cases: they mean opposite things to a connection — close versus wait
— so a declared nine exabytes has to be `Failed`. Answering "incomplete" would be an
instruction to buffer it.

**The connection's first bug: it returned as soon as the peer closed.** That lost the
handshake and every frame already sitting in the buffer, and a server is allowed to send
the response and hang up in the same packet — many do. The close is remembered and acted
on after framing now. This is the same shape as the WOFF2 session's lesson about doing
the work before believing the failure.

**The tests needed a transport that stays open, and that is a finding.** The shared
`ScriptedTransport` hangs up the moment its canned response has been read, which is a
real server behaviour and exactly the wrong one here: with it, the socket is Closed by
the end of the first `Advance` and nothing about `send`, `close`, a second message or the
idle wait can be observed. `OpenTransport` answers `Blocked` when it has nothing to give,
which is the state an idle WebSocket spends its life in. What its tests then assert is
the shape that matters for ADR 0020 §5: an open socket with nothing outstanding produces
no work and queues nothing, a frame split across two turns is reassembled rather than
dropped or re-read, and a close we start is a closing handshake rather than a hang-up.

One deliberate non-random choice, written where the code is: the masking key is a
counter. RFC 6455 wants unpredictability so that an attacker controlling the payload
cannot make the wire bytes look like an HTTP request to an intermediary; `wss://` means
there is no intermediary that can see them, and a weak PRNG is no better than a counter
against that attack while being harder to reason about. A plaintext `ws://` path must not
ship without a real mask.

### WebSocket and EventSource · 2026-08-06 (session 23 finished)

Five commits. The two long-lived connections, and everything about them that is not the
bytes on the wire is about the loop.

**The zero-idle-CPU argument is a test now, not a paragraph.** An open connection is one
descriptor in the idle wait and nothing else — no timer, no poll, no keepalive — so a page
holding one reports *no runnable work*. The test asserts both halves, and it has to:
work-and-waiting spins, and neither-work-nor-waiting means messages arrive only when
something else happens to wake the loop. Ping is answered and never originated for the same
reason, and a waiting `EventSource` is the only long-lived connection that contributes a
deadline at all.

**The reconnect is the only request in this browser the user did not cause**, which makes
it the one place a bug becomes a browser hammering a server on a page nobody is looking at.
So the bound *is* the feature: the delay doubles from `retry:` or three seconds, caps at
thirty, and after six consecutive failures it stops and stays stopped. A non-200 or a wrong
content type is permanent with no retry at all — a URL that answers 404 will answer 404
again, and retrying it six times with backoff is six requests nobody asked for. A stream
that delivered something resets the counter, because that counter is about a server that
cannot hold a connection rather than one that eventually drops a healthy one.

The engine supplies the transport for a reconnect and the *connection* decides when. That
split is the interesting one: a connection that could make its own transport would be one
that could reconnect after the page that opened it was gone.

**Three refusals in the frame codec are refusals for reasons of their own**, not because
the RFC says so. A masked server frame: accepting one means accepting a frame a proxy could
have rewritten, which is what the masking rule exists to prevent. A redundant length form:
legal by the letter of §5.2, refused for the reason WOFF2's base-128 refuses leading zeros
— a second spelling of a length is a second way for two implementations to disagree about
what a frame is, and the encoder writes the shortest form so this browser never produces a
frame it would itself refuse. And a control frame that is fragmented or over 125 bytes,
because a fragmented `close` is a state machine two implementations disagree about.

`Failed` versus `Incomplete` is where the bound lives rather than in a size check: the two
mean *close* versus *wait* to a connection, so a declared nine exabytes has to be `Failed`
— answering "incomplete" would be an instruction to buffer it.

**Two bugs, both from doing the work before believing the failure.** The connection
returned as soon as the transport said the peer had closed, which lost the handshake and
every frame already in the buffer — a server may answer and hang up in the same packet, and
many do. And `EventSourceConnection` never set `opened`, because the line that should have
was a leftover from an earlier shape; the fix also made `open` fire again after a
reconnect, which is what tells a page the stream is back.

**Testing long-lived connections needed a transport that stays open**, and that is a
finding worth reusing. `ScriptedTransport` hangs up the moment its canned response has been
read — a real server behaviour and exactly the wrong one here, since with it the socket is
Closed by the end of the first `Advance` and nothing about `send`, `close`, a second
message or the idle wait can be observed. `OpenTransport` answers `Blocked` when it has
nothing to give, which is the state an idle connection spends its life in. Two smaller
lessons came with it: **a fake server must compute its handshake from the key the
connection actually sent** — a hard-coded `Sec-WebSocket-Accept` tests its own paste, and
fails — and **ASan caught the test holding a pointer to a transport the connection had
destroyed**, because a closed socket here is usually a destroyed one. The observation is
shared state that outlives the transport now.

SHA-1 exists in this tree for exactly one caller, and `util/Sha1.h` is mostly an argument
against a second: the handshake accept is a *protocol* check, not a security claim, and what
makes the peer trustworthy is `wss://`. The masking key and the handshake key are both
counters for the same reason, which is also why `ws://` is refused — accepting a plaintext
socket would quietly invalidate two decisions made elsewhere.

In the `EventSource` binding, one detail a page depends on: `readyState` goes back to
CONNECTING on a retryable drop and to CLOSED only when the browser has given up. That is how
a page tells "reconnecting" from "failed", and getting it backwards makes a page tear down a
stream the browser is about to re-open.

### The audio ring and the playback clock · 2026-08-06 (session 24, unfinished)

`7ee58fb` and `ad6a4c2`. Two objects, both small, both with the property that their bugs
are *audible* rather than visible — which is why the reasoning is in the headers.

**The ownership statement came before the code**, as `AGENTS.md` requires of any thread, and
writing it first is what decided the shape: the audio thread owns the device handle, the read
cursor and the clock; the engine thread owns the write cursor and the storage; the ring
borrows nothing, because samples arrive copied rather than pointed at — so the audio thread
cannot reach a document, a decoder or the heap even by accident. Single producer, single
consumer is not a simplification but the thing that makes it correct with two atomics and no
lock, and a lock in an audio callback is a click in the output.

Three decisions that are audible when wrong, and all three are the sort a single-threaded
test would not force:

- **A full buffer leaves one frame unused.** With `write == read` meaning empty, without the
  spare frame full and empty are the same state and a consumer drains a full buffer as
  silence.
- **Cursors are in samples, not frames**, so a wrap cannot land mid-frame. If it could, one
  channel would lead the other by a sample for the rest of the stream — a phase shift rather
  than a failure.
- **An underrun is silence plus a count**, never a short buffer: a device callback must fill
  its whole block, and a short answer is a click. A count rather than a flag because "three
  times" and "once" call for different responses.

**The memory ordering is the part that cannot be tested into existence, so it is tested with
two real threads under TSan.** The producer releases the write cursor after the samples; the
consumer acquires it before reading them. A relaxed store would pass every single-threaded
assertion in that file and tear on a machine with a weaker memory model — so the test hands
20,000 frames of a *counting sequence* through a 64-frame ring, which makes a torn handoff a
detectable discontinuity rather than merely wrong samples. TSan clean.

The clock is driven by frames the device consumed rather than by wall time, and that is the
whole design: a wall clock and an audio device drift apart, so a video synchronised to the
wall clock loses lip-sync at a rate nobody can predict. It reports the **presented** position
by subtracting the device's buffer occupancy — the arithmetic a naive clock omits, and
omitting it runs video ahead of the sound by exactly the buffer depth. A seek restarts the
frame count, which is the same bug in the other direction if forgotten.

What is left of this session is the device and "no audio thread when nothing is playing".
With SDL3's audio API the callback runs on *SDL's* thread, so "our thread" is really "the
device is open" — which makes the session's check a question about whether a device handle
exists, and is worth knowing before writing it. And an `<audio src="…mp3">` cannot actually
play until session 27's codec decision lands, so the honest intermediate is a synthesised
tone through the ring to a real device.

### The audio device, and the clock that did not need the thread · 2026-08-06 (session 24 finished)

`a59e3e0` completes it. The device, the sink interface, and a tone tool that verifies
everything except the decode.

**Writing the ownership statement first changed the design, which is the whole argument for
the rule.** ADR 0028 §4 has the audio thread owning the playback clock. It does not need to:
the ring already counts frames read, and making that an atomic means the *engine* builds a
position from a value it can read at any time. The audio thread then owns the device handle
and the read cursor and nothing else — strictly less to reason about than a clock two threads
touch.

Two properties fell out of that and are now asserted. **A total is idempotent where an
increment drifts**: a caller that polls twice or misses a turn gets the same position rather
than one that ran ahead by whatever it double-counted. And **an underrun's padded silence is
not counted**, because it was never in the stream and counting it would advance the clock past
what the media contains. Neither would have occurred to me while writing an additive
`FramesConsumed`; both are obvious once the counter belongs to the ring.

**The device is the thread.** SDL3 calls its callback on its own audio thread, so "no audio
thread when nothing is playing" is exactly "no open device" — which turns this session's check
from a lifecycle nobody can observe into a state the object can be asked about. `Stop` joins,
because SDL guarantees no further callback after the stream is destroyed, and that is what
makes the ring safe to destroy afterwards; the interface says so, since a caller that reverses
the two has a use-after-free the sink cannot prevent.

The callback's four rules are in the header rather than in a review comment because each has
been a real bug in some player: no allocation (the allocator may be held by the thread trying
to stop it), no lock (a lock in an audio callback is a click), no document or decoder (it
cannot see them, by module contract), and always fill the whole block.

**`heard 0.50s` against `generated 0.50s`** is what the tone tool prints, with zero underruns
while playing and seven in the tail after the tone ended. The two counts are reported
separately on purpose: the tail is a device asking for samples that no longer exist, and
lumping them together would hide the number that matters behind the one that does not. The
first version printed `heard 0.00s`, which is how the clock's missing driver was found — the
tool paid for itself before it made a sound.

An `<audio src="…mp3">` still cannot play, because there is no decoder until session 27's
codec decision. That is why the tone exists: it is the honest intermediate, and it exercises
the ring, the device, the callback and the clock together.

### The media state machines, and activation a page cannot forge · 2026-08-06 (session 25, unfinished)

`2ec98ae` and `37bd0c7`. The part of `HTMLMediaElement` that had to be right, and the flag the
autoplay refusal rests on.

**"The states are the API" is why the state machine is a pure object.** It holds no samples, no
element and no network, so its transitions can be *driven and asserted* — which is the only way
to know they are the specification's rather than an approximation. It produces an ordered list
of events to fire rather than firing them, because the order is observable: a page that gets
`canplay` before `loadedmetadata` reads a duration that is not there yet, and `TakeEvents`
means a document whose script has not run cannot lose them.

The transitions worth writing down, because each is a bug that presents as something else:

- **The ladder fires every rung it climbs past.** A whole file can arrive at once and a page
  waiting on `canplay` still has to hear it.
- **A seek drops readiness to Metadata.** ADR 0028 §1 names this one outright: what was decoded
  was for somewhere else, and a `readyState` that stays at EnoughData across a seek is a player
  that stalls with no error and no way for the page to tell.
- **Playing without enough data is `waiting`, not `playing`** — a page shows its spinner on one
  and hides it on the other.
- **`NO_SOURCE` is not a slow load**, and `play()` on it is `NotSupportedError` rather than
  `NotAllowedError`, because a page shows an error message for one and a play button for the
  other. Confusing them makes a video that could have played look broken.
- **The end is not a pause.** `ended` is a stream running out; `pause` is something a page or a
  user did. And `play()` after the end rewinds, or a replay button does nothing.

**For user activation, what matters is where it is set, not what it stores.** A trusted click
reaches `Page::DispatchClickAt` and a trusted keystroke `Page::DispatchKeyToFocus`; a click a
page dispatches itself goes through the binding layer and reaches neither. So the test is a
page that clicks its own button and dispatches its own event, asserting the flag stays clear
before a real click sets it — the flag being unforgeable is the whole feature, and it is the
sort of thing that is easy to implement in the wrong place and impossible to notice afterwards.

It is sticky rather than transient, and the header says why and what that costs: the
specification expires a transient activation so a click cannot license a popup a minute later,
nothing here opens a window, and the only consumer is autoplay — where sticky is what a user
expects. The first transient consumer turns this into a timestamp.

The binding landed too, in `7298d18`. **The promise is the part that had to be right**: every
player on the web calls `play()`, catches `NotAllowedError` and shows a play button, so a
`play()` that returned undefined would make those players silently do nothing. `NotSupportedError`
when there is no source is the same argument one level down — a page shows an error for one and a
button for the other.

Two things are refused rather than faked, both ADR 0012's rule: `canPlayType` answers the empty
string for everything, because "maybe" is a lie a page acts on and there is no decoder; and
`load()` throws rather than returning, because a page that calls it expects a reset and a no-op
leaves stale state.

**Three lessons from that commit are about shape rather than logic, and all three cost time:**

- **The architecture lint refused the first version.** Two state maps on `Page` made it hold
  members from five modules, and the message is "split the coordination rather than widening the
  class". It was right: what `Page` does with media is coordinate — read the document's
  activation, fire the events at script — and `engine::MediaElements` owns the map.
- **A `const` read has to create the state.** `video.networkState` on an untouched element must
  answer LOADING when it has a `src`, because that is what the attribute means. The first version
  only *found* state on the const path and answered EMPTY, reporting "no source" for an element
  that had one.
- **`Page::Load` builds a tree but does not run scripts** — the engine's load pipeline does. The
  first version of the three element tests asserted against a script that never ran and reported
  an empty console, which is the failure mode a harness should not be able to have quietly. They
  use the engine harness now.

What is left of the session is the engine half — nothing fetches a media `src` yet, so nothing
drives `MetadataArrived`/`BufferedAhead`; that is where sessions 26 and 27 arrive — and default
controls as user-agent boxes, which is the one part of this session that is layout work rather
than plumbing.

### Replaced media elements and controls that are boxes · 2026-08-06 (session 25 finished)

`abc455e`. `<video>` and `<audio>` are replaced elements now, and `controls` paints a bar.

**Replaced is what stops the fallback from rendering**, and that is the whole reason it matters
before any decoder exists: a `<video>`'s children are content the element replaces, so without
it a page's `<source>` list and its "your browser does not support video" paragraph lay out as
page content. One line in a predicate, visible on any page with a fallback.

The default sizes are load-bearing before anything loads. 300x150 for a video is what every
browser uses, and a page laying out around one before it loads is laying out around that number
— a wrong one moves the page when the video appears. An `<audio>` is 300x54 *with* controls and
**nothing at all** without them, which is what keeps one used as a sound effect from pushing a
page around.

**The controls are boxes the user agent creates inside the page**, which is ADR 0028 §1 putting
them in ADR 0018's category rather than making them `src/ui` widgets — and the consequences are
the argument: they live in the page's coordinate space, so a transformed or clipped video
transforms and clips its controls, and `display: none` removes them because it removes the box.
Neither would be true of a widget layer above the page.

They are **not interactive**, and the honest version is better than the plausible one: hit
testing does not know about them, `src/layout` may not see `media` so the builder cannot read
the state machine, and therefore the play glyph points right and the scrubber is empty. A pause
bar would be a claim about state this code cannot check, and a bar that looked clickable and did
nothing would be worse than one that plainly is not.

`LayoutEngine.cpp` went over its module cap, and the cap was pointing at something real rather
than at a line count: `src/layout/ReplacedBoxes.cpp` is the question that was hiding in it —
"an element whose content comes from outside CSS: how big is it, and what text does the user
agent put in it?" — asked by the box builder and nothing else, with `<img>`, the form controls
and now the media elements each answering differently.

One correction worth recording as method rather than as fact. The first version of that commit
message explained old.reddit's display-list count moving from 1061 to 1076 as "the whitespace
collapse moving". That was a guess, and checking it showed it was probably wrong: the page has no
`<video>` or `<audio>` at all, it is a live feed whose content changed between the two
measurements, and neither reading was a controlled comparison. Hacker News being unchanged at 705
is the useful control. The message now records the reddit number as **unexplained** rather than
explained away — a plausible cause in a commit log is worse than an admitted gap, because the next
person reads it as evidence.

### The last two demuxers · 2026-08-06 (session 26)

`dc206c8` the HLS playlist parser, `ea56136` WebM/Matroska. Fragmented MP4 had landed out of order
two days earlier, so this closes ADR 0028 §2.

**Both were verified against real inputs before their fixtures were written**, and that ordering is
the method rather than a courtesy. `test-streams.mux.dev`'s master playlist parses as 5 variants
with their bitrates, resolutions and comma-bearing codec lists; its 720p media playlist as 64
segments, complete, with an 11-second target duration. An ffmpeg-produced VP9 + Opus WebM of 18,685
bytes comes out as 2 tracks and 61 samples with **51 of them sync** — which is exactly 50 Opus
frames, all of which are keyframes, plus one VP9 keyframe. That number is why I trust the shape;
the unit tests then assert the corners a real file never exercises.

**For HLS the refusals are the substance**, because every string the parser produces becomes a URL
this browser fetches and every number becomes a schedule it advances by:

- A file without `#EXTM3U` is not a playlist. A CDN serving an HTML error page with a 200 is the
  common case, and playing it as one is how a player ends up requesting a segment named `<html>`.
- A duration a player cannot schedule is *dropped*, not clamped: zero is an infinite loop in a
  scheduler that advances by duration, and a clamp invents a schedule the playlist never described.
- A playlist with variants *and* segments is unplayable, because a player cannot know whether to
  fetch a URL or recurse into it. The first kind seen wins and the rest is refused — a playable
  subset rather than a guess — and the kind comes from evidence rather than from a tag.
- Unknown tags are ignored, and that is required rather than lax: HLS is extended by adding them,
  so a parser that refused one would refuse every playlist written after it.

**EBML's hazards are not the box format's**, which is why WebM is a separate parser rather than a
mode of the MP4 one. An element's id *and* its size are variable-length integers, so the length of
the length comes from the file — one function reads both with a flag, because an id keeps its marker
bit and a size strips it, and two functions would let that arithmetic diverge. An **unknown size is
legal** and is what a live stream sends, so treating it as an error would refuse every live WebM.
And a block's timecode is **signed**: a frame may precede its cluster's base, and reading it
unsigned puts that frame 65 seconds into the future, where a player schedules it and waits.

The Matroska tests *build* their fixtures rather than pasting hex, because EBML is structured and a
blob hides which field a test is about — `Element(kTracks, …)` says what it means, and a test that
needs a malformed length writes exactly that. Two of them are exhaustive rather than illustrative:
every prefix of a file, and every single byte flipped, must come back as a usable file or a refusal
with no sample pointing past the data. A partial download is the common case for a large video.

One practical note for later sessions: **ffmpeg is on this machine**, which is how the WebM fixture
was made. Session 28's MSE work needs an initialization segment and media segments, and those are
one ffmpeg invocation away rather than a hand-built file.

### The codec decision · 2026-08-06 (session 27, the ADR half)

`13d1008`. ADR 0031 is written, and the part of it that is code today is the allowlist.

**ADR 0013 deferred this with a reason worth having honoured**: "choosing a dependency before that is
how a project acquires ffmpeg by accident." Deciding it now was informed by things that did not exist
then — the audio path, the element's state machines, and all three demuxers — and by two measurements
taken today rather than recalled: the public HLS master playlist serves `avc1.64001f` with
`mp4a.40.2`, and a locally produced WebM carries `V_VP9` with `A_OPUS`. So H.264 and AAC are
unavoidable, and a browser shipping only the open formats would fail the sites ADR 0007 picked.

**The decision is mixed rather than uniform**, and that is the interesting part: dav1d for AV1,
libvpx for VP9, libopus for Opus, and `libavcodec`'s decoders for H.264 and AAC alone. Three of the
five have a library that is small, readable, replaceable *and* the implementation their encoders were
validated against; two do not, and their alternatives (openh264, fdk-aac) are licensing arrangements
rather than libraries one audits and patches. Taking ffmpeg for AV1 when dav1d exists would be
choosing the larger dependency for uniformity, which is precisely the trade ADR 0001 says not to make.

**The load-bearing decision turned out to be where the allowlist lives**, and writing the ADR is what
surfaced it. ADR 0013 says the container is ours because it decides what the codec is asked to decode
— and a `--enable-decoder=h264,aac` build flag does *not* keep that true. It is correct the day it is
written and drifts the first time somebody debugs a build, and a drifted flag re-enables a hundred
parsers this project owns the replacements for. So `media::CodecId` is a five-entry table that fails a
test instead, and it reconciles the two spellings the demuxers produce (`V_VP9` versus `vp09`) in one
place rather than in four callers each writing `codec.find("vp9")`.

The entry that needed care is AAC. A bare `mp4a` is an MPEG-4 audio object type the table cannot
resolve, and `mp4a.40.34` is MP3-in-MP4 rather than AAC — so the table lists the profiles rather than
the family, and both refusals are asserted. Fourteen other codecs a container can legitimately name
are asserted refused too, because "refused before a library is configured" is what ADR 0013's sentence
means in code.

Two things the ADR states that are worth repeating outside it. **Hardware decode is refused for now
and flagged as the first line to re-examine** — unlike EME's refusal, nothing about it is
incompatible with this project's values; it is deferred because a GPU driver bug is a kernel
compromise rather than a process one, and what that costs (no smooth 4K, more power for 1080p) is
written without softening. And **a decoder crash is an ordinary path**: the element fires `error`,
which the state machine from session 25 already has, and the process is *not* restarted for the same
sample, because a restart loop on a hostile file is a denial of service the page chose.

What is left of this session is the process itself, and the first step needs the user: **none of the
four libraries is installed here.** `pkg-config` finds no dav1d, libvpx, libopus or libavcodec,
though apt has all four packaged. ADR 0001 makes adding a dependency a reviewable act, so installing
them is not something to do silently — the ffmpeg *binary* that produced the WebM fixture is present,
but that is a tool rather than a link-time dependency.

### The sandbox, before the thing it confines · 2026-08-06 (session 27, second half)

`cc214d4`. ADR 0004's seccomp mechanism with ADR 0031 §4's policy — landed *before* the decoder,
because the policy is the security property and the codec library is a leaf, and because it needs
none of the four dependencies that turned out not to be installable here.

**The tests fork, and that is the only way to test a sandbox**: a successful test of a denial is a
dead process, so every assertion is about a child's exit status. Four denials are watched directly —
`openat`, `socket`, `fork`, and a second `seccomp` call, which is what makes the filter one-way —
and so is the other direction, which is the half that is easy to get wrong: a confined process must
still allocate a megabyte and write to a descriptor it already holds, because every library ADR 0031
chose does both per frame. A sandbox nobody has watched refuse something is a sandbox nobody knows is
applied.

Three entries in the policy carry their own reasoning. **A violation kills rather than returning an
error**, and that is chosen rather than inherited — a library that gracefully handles being denied
`open` keeps trying, and a compromised one would probe the boundary. **The architecture is checked
before the syscall number**, because a filter that skips it is one a 32-bit syscall walks straight
through: a number that is `read` on x86-64 is something else on i386, which is the classic seccomp
bypass. And **`mprotect` is allowed**, which is the uncomfortable one since it is how W^X is defeated
— it is on the list because the allocator needs it, and narrowing it by argument is possible and not
attempted, which is written down rather than glossed. `openat` gets its own note because it is the
entry a naive list forgets: `open` is the name in the manual and `openat` is what glibc calls.

**The finding: a sanitizer runtime cannot live inside this policy.** The first ASan run killed the
confined child before the test body executed — ASan intercepts allocation and signals and reads
`/proc/self/maps` to symbolise a report, so it calls `openat`, `rt_sigaction` and `sigaltstack`, none
of which a media decoder has any business making. The forked tests skip under sanitizers rather than
the policy being widened, and the distinction matters: widening it means putting `openat` back on a
decoder's allowlist, which is the single entry this whole mechanism exists to remove. Every browser
with a sandbox carries the same note — a sandbox and a sanitizer are alternative ways to inspect one
process, not simultaneous ones. The ordinary build runs all six tests; ASan, UBSan and TSan are clean.

The policy is a *parameter* rather than the implementation, because the renderer split needs the same
mechanism with a different list, and a second seccomp filter written later would be a second chance to
leave a hole in.

**Where this session stops, and it is not a context limit.** The four libraries are not installed:
`pkg-config` finds none of them, apt has all four, and `sudo` requires a password here. ADR 0001 makes
adding a dependency a reviewable act, so this is a decision for the user rather than something to do
quietly — and it is the largest dependency addition in the project's history. Everything that could be
built without them has been.

### Character encodings · 2026-08-06 (session 30, taken out of order)

`f469b94` the decoders and the sniffing algorithm, `02b60c7` the wiring. Taken up out of order
because session 27's remainder needs four libraries this machine cannot install, and this session
needs none.

**The rule that carries the weight is one I got wrong first.** A UTF-8 sequence's valid *second byte*
depends on its lead — `E0` requires A0–BF, `ED` requires 80–9F, `F0` requires 90–BF, `F4` requires
80–8F — and that is not a refinement of "is the resulting code point in range". It decides how many
U+FFFDs an ill-formed run produces and therefore **where the run ends**. `ED A0 80` is three
replacements, not one, because the maximal subpart ends after `ED`; a decoder that checks only the
resulting code point consumes all three bytes and emits one, which is a different document. Overlong
forms fall out of the same table rather than needing their own check.

The companion rule: a byte that *ends* an ill-formed sequence is never consumed. That is what makes a
`<` after a bad sequence reach the tokenizer as a `<`, where the tokenizer's own rules apply to it —
and swallowing it is how a decoder hides the character a sanitiser was looking for.

**Three bugs, and the fuzzer found the first one on its first run.** `find('>') + 1` is 0 when there
is no `>`, so a truncated `<meta ch` computed a negative length and read gigabytes past the buffer.
It is reachable from any document whose tag straddles the 1024-byte prescan boundary, which happens by
construction rather than by accident. The second was the prescan's value scan not stopping at `>`, so
`<meta charset=utf-8>` produced the label `utf-8>`, resolved to nothing, and fell back to
windows-1252 on a page that had declared UTF-8 — the confusion this whole file exists to prevent, in
miniature, in my own code.

The third was in the **fuzzer**, and it is worth recording as a category: my idempotence invariant
("decoding the output again as UTF-8 changes nothing") trips on well-formed output, because a decoded
U+FEFF is byte-for-byte a leading BOM and gets stripped the second time. That is a property of the
format, not a defect, and the check guards with an ASCII byte now. An invariant that is *nearly* true
is worse than a weaker one that is exactly true.

**Three of my own test expectations were wrong and the code was right** — the replacement counts for
`E0 80 41` and `E0 80 AF`, and which byte of ISO-8859-5 is А. They are corrected in place with a note
saying so, because a test that was wrong is worth more as a record than as a silent edit: the next
person to touch the substitution rules will want to know that the counts are surprising.

Three specification decisions that look arbitrary and each have a reason worth keeping: `iso-8859-1`,
`latin1` and even `ascii` all mean **windows-1252**, because a page labelled ISO-8859-1 with a 0x93 in
it means a curly quote and rendering a C1 control there renders something no reader saw; the fallback
is **windows-1252 rather than UTF-8**, because an undeclared page is overwhelmingly old; and a bare
`utf-16` label means **little endian**, because that is what the installed base emits.

Measured end to end: `naïve café "quoted" … 90°` renders from raw windows-1252 bytes with no
declaration, and the three rendering sites are unchanged because they all declare UTF-8 and the
algorithm agrees with them.

### Unicode tables, UAX #14, and CJK inside its box · 2026-08-06 (session 31)

`7fa51f8` the generator and the algorithm, `b4fc5aa` the wiring. The visible bug is gone: a 260px box
of Japanese wraps to three lines inside its border where it used to be one line running off the page.

**The tables are generated and checked in, and both halves of that matter.** Generated, because
LineBreak.txt is 3,608 lines and a transcription error in it is a class of text that wraps wrongly with
no way to notice; checked in, because a build that downloads from unicode.org fails when the network
does and produces different output depending on when it ran. Ranges rather than per-code-point
entries: CJK ideographs are one run of 20,992, so the whole of Unicode is 2,812 rows and a lookup is
twelve comparisons instead of a megabyte.

The pair table is written as UAX #14's numbered rules **in order**, each block citing the rule it
implements, rather than as a 33x33 grid of letters. A grid is smaller and unreadable; this way a wrong
answer is traceable to a rule by someone with the specification open, which is the only way this stays
maintainable.

Two of my expectations were wrong and the data was right, again: `)` is class **CP**, not CL — since
Unicode 6.1 the parenthesis and square bracket have their own class because LB30 treats them
differently from `}` — and small kana is `CJ`, resolved to `NS`. Both corrected with the reason at the
assertion, because a reader will not believe either without it.

**Two findings came from rendering the fixed page, not from the tests.**

The first: the CJK box wrapped correctly and displayed `æ—¥`. That was ADR 0025's windows-1252
fallback doing exactly what it says — and it is right for a document from a server and wrong for one
carried in its own URL. A `data:` URL's payload arrives percent-encoded, and the bytes `%E6%97%A5`
decodes to are UTF-8 because that is what the encoder emitted. So a `data:` URL that names no charset
is UTF-8 now, which is a deliberate deviation from RFC 2397's `US-ASCII` that every browser also
makes. One existing test asserted the old content type; it was changed with the reason written at the
assertion, and a second case added for the URL that names its own charset and must not be overridden.

The second is recorded rather than fixed: **CJK still renders as boxes, and that is our font fallback
rather than the system's.** `fc-list :lang=ja` finds 31 faces on this machine — Noto Serif CJK is
installed — and this browser asks for `sans-serif`, gets DejaVu Sans, and stops. There is no
per-character fallback to a font that covers the code point. So the layout is now right and the glyphs
are not, which is a shaper question and the next thing this area needs.

A trap worth writing down for that session: the synthetic test font reports **zero width** for glyphs
it lacks, so a CJK wrapping assertion written against a real font passes without exercising anything.
The test uses `FixedTextMeasurer` (width per byte) for exactly that reason, and finding this cost a
failing test that looked like a broken feature.

### Per-character font fallback · 2026-08-06 (session 31's second finding, fixed)

`d1034c8`. The bug the previous commit found by looking at the page it had just fixed: line breaking
put the Japanese inside its box, and every ideograph rendered as a box, on a machine with **31
Japanese faces installed**.

One font per *element* is what the author asked for; one font per *character* is what the machine can
do. Every browser resolves it the second way, and the three pieces here are the minimum that does:

The **pair of questions** is the part worth keeping. `FontForCodePoint` answers with a font — and for
an uncovered character it answers with the *preferred* face rather than null, because that draws
`.notdef` and a visible box is the honest glyph for something this machine genuinely cannot draw.
Null would drop the character silently, where a reader cannot tell text is missing. But a caller
deciding whether to keep looking needs the coverage answer rather than the font, so
`CoversCodePoint` exists beside it. Collapsing the two would make "nothing covers this" and "this is
what to draw" the same answer, and they are opposites.

The **block cache** is what makes it affordable. A page of Japanese asks once per character and the
answer is the same face every time; without a cache that is a face load per character, and this
machine has 533 faces to load. Keyed by 256-code-point block, because scripts occupy contiguous
ranges — and "nothing covers this block" is cached too, or an undrawable run re-probes every
installed face for every one of its characters.

**Painting and measuring go through the same split**, and that is not tidiness: a measurement taken
with one font and a paint done with several is a line that overflows by however much the fallback
face differs from the requested one. One function, two callers, no way for them to disagree about
where a fallback began.

Measured: 14 fallbacks and 12 runs split for the Japanese paragraph, 139 glyphs drawn, and `UAX 14`
still in the Latin face *inside the same paragraph* — which is the mixed-script case the split exists
for. Hacker News is unchanged at 705 display-list commands with **zero** fallbacks and zero splits,
which is the assertion that the common path costs nothing.

## Session 32 — the legacy multi-byte decoders

`886e95d`. Five encodings, four generated indexes, and three bugs — every one of them found by a
check written before the code passed it, and none of them by a test that failed on its own.

**A wrong range in a legacy decoder produces plausible wrong characters, not a failure.** That is
what makes this kind of code different from a parser: a Japanese page decoded with an off-by-one
lead range renders as a *different Japanese sentence*, and nothing downstream — not the tokenizer,
not layout, not a reader who does not read Japanese — can tell. Unit tests written by the same
person who wrote the arithmetic test the same misunderstanding twice. So the verification was a
sweep of the entire two-byte space of all five encodings, 27,972 sequences each, against two
independent references: a from-the-spec reimplementation of the algorithm, and the platform's own
`cp932`/`cp949`/`big5hkscs`/`gb18030` codecs. **EUC-KR agreed with cp949 on all 27,972.** Where the
others disagree, the disagreement is the standard's index deliberately differing from the vendor
table — GB18030 pointer 6555 is an ideographic space in the index and a private-use character in
cp936, and it is the index a page was authored against in a browser.

The three bugs are worth separating by *what found them*, because they are three different kinds of
check:

**The differential found that Shift_JIS byte 0x80 was passed through raw.** It is U+0080 — two
bytes in UTF-8 — so pushing the byte emitted a lone continuation byte. A decoder whose entire job
is to produce UTF-8 was producing output no UTF-8 decoder accepts, on all 66 sequences that reach
it. It looked exactly like the ASCII pass-through one line above it, which is why it was written.

**Designing the fuzz invariant found two deletion bugs before the fuzzer ran.** Writing down "every
input byte below 0x40 survives" forced the question *can any sequence consume one?* — and the answer
was yes, in two places I had written: EUC-JP's `0x8F` and GB18030's four-byte form are both
*refusals* here (no JIS X 0212 index; no four-byte support), and both consumed the whole sequence
without checking its shape. So `8F 3C` and `81 30 3C 3C` each deleted a `<`. **A decoder that
deletes a `<` hides the character a sanitiser was looking for**, which is the encoding-confusion XSS
family in one line. Both now check the byte ranges first and otherwise consume one byte — which is
what the standard's pushback amounts to.

**Then the fuzzer found the invariant itself was wrong**, on `cf 34 d6 32` as GB18030: the four-byte
form's second and fourth bytes are ASCII digits *by construction*, so a well-formed one legitimately
eats two of them. The fix was to the assertion, not the code, and the exemption is narrow on
purpose — a digit cannot begin a tag, an attribute or an entity, so losing one cannot change how a
document parses. 9,040,399 runs clean afterwards. The single-byte family keeps the stronger
exact-equality form, because none of its trail bytes exist.

**Flat arrays rather than sorted ranges**, and the measurement is why: the four indexes are 70–100%
dense (0.70 / 0.72 / 0.94 / 1.00), so a range structure over them is a binary search to save
nothing. 648KB of generated `.inc`, one pointer-indexed lookup, and the element width chosen *from
the data* — which mattered: the generator asserted "no index maps above the BMP" and **the assertion
fired**, on Big5 pointer 947 (U+27267). A uint16 table would have truncated that to U+7267 silently,
and a wrong character is worse than a missing one.

**Four of my own new expectations were wrong and the code right** — an EUC-KR syllable, a Big5
sentence, and *both* of Big5's macron/caron pointers, where 1164 is the macron and 1166 the caron.
Corrected in place with a note, because getting one of those four backwards is invisible by eye.

**Two assertions in an existing test were changed, deliberately.**
`AnUnknownLabelFallsThroughRatherThanToUtf8` used `shift_jis` and `gb18030` as its examples of labels
this browser lacks. It now uses `iso-2022-jp` and `hz-gb-2312`: an example that has become supported
proves nothing about the fall-through it exists to test.

Measured on kakaku.com, which declares `shift_jis` in a `<meta>` and sends no charset in its
`Content-Type`, so it exercises the prescan and the decoder together: title
`価格.com - 「買ってよかった」をすべてのひとに。` exactly, 1136 commands, 704 runs, 34 images, and
**`encoding.replacements` absent — not one U+FFFD on a whole real Shift_JIS page.** Before this the
label was unrecognised and fell through to windows-1252.

Found on the same pass and left for later, because it is not this session's: **aozora.gr.jp's first
inline script fails with `SyntaxError: unexpected token '<'` on `<script type="text/javascript"><!--`.**
That is Annex B HTML-like comments, a JavaScript lexer gap, and the pattern is on a great many older
pages — where it costs the *whole* script rather than one line.

## Sessions 33–34 — UAX #9, and what a conformance suite cannot tell you

`45e675f`, `4e850f5`. Bidi in two halves: the algorithm and its reordering, then mirroring,
`dir="auto"` and `unicode-bidi`. Five bugs between them. **The conformance data found none of
them.**

That is the finding worth keeping. Unicode ships 861,948 test cases for this algorithm — 91,707 in
BidiCharacterTest.txt and 770,241 in BidiTest.txt — and `src/text/Bidi.cpp` passed all of them on
the first run: paragraph levels, resolved levels, and visual order. It is genuinely the only thing
that can validate twenty-odd interacting rules, because a test I write exercises the cases I thought
of and those are the ones I got right. Three of the ten hand-written tests I *did* write had wrong
expectations against correct code.

And every one of the five real bugs was outside the algorithm's boundary. The suite checks levels
and an order of *indices*; it has nothing to say about which font shapes a run, which end of a run
is painted first, or whether the style a text box carries is the style its parent had.

**One direction is not one script.** A line of Hebrew and Arabic is one bidi run — both are level 1,
correctly — and handed to HarfBuzz as one buffer it is shaped entirely as Hebrew, because HarfBuzz
takes a buffer's script from its contents. Arabic shaped as Hebrew gets no joining, so `مرحبا` came
out as five disconnected letters: unreadable rather than ugly. `SplitByCoverage` now splits by
script as well as by font, through `hb_unicode_script` rather than a new table — script itemization
is what that library is sanctioned for, and Common and Inherited continue whatever run they are in
so a space or a combining mark cannot split a word.

**The bug that only a probe could find.** With the script split in place, the pieces of a run were
still laid down left to right — so within a right-to-left run the logically-first piece landed
leftmost instead of rightmost. I looked at the rendering three times and could not tell. I described
it to myself once as correct and once as wrong. A four-line `fprintf` settled it in one run:
`pen=630.7 'ערבית: '` followed by `pen=683.6 'مرحبا بالعالم'`, exactly reversed. **Reading a
rendering of a script you cannot read is not verification**, and the general form of that is worth
remembering: when the observation channel is the thing under test, add a second channel rather than
looking harder.

**HarfBuzz will not be told by the text.** `<bdo dir=rtl>abcdef</bdo>` drew `abcdef`, because Latin
script means left-to-right whatever the resolved level says — and an override is precisely the case
where the level and the script disagree. `TextShaper::Shape` now takes the direction as a parameter.
The resolved direction rides on the display list's *text run* rather than on the command, because a
command is 24 bytes and full — and because it belongs beside the advance: both are facts decided
before paint that paint cannot recompute.

**A synthetic control announces itself to nothing.** `unicode-bidi` is implemented as the pairs of
explicit control characters UAX #9 already defines, which is why the property cost almost no code —
but those controls are inserted by layout, so `NeedsBidi`'s byte scan over the document's text
correctly reported that an all-ASCII `<bdo>` line needed nothing. The fast path now also asks whether
any box on the line *wants* a control.

**Two lists of inherited properties, and the older one had drifted.** `src/layout` built the style
for the anonymous box around a text node with its own hand-written list of seven inherited
properties. `direction` and `unicode-bidi` went into the cascade's list and not into that one, so a
right-to-left `<span>` was right-to-left and the text inside it was not. This was invisible in every
rendering — the paragraph's direction comes from the *block*, so `direction` on a text box is never
read, and only `unicode-bidi` silently did nothing. There is one list now, `css::InheritInto`, with a
test asserting what it copies; the text-box caller passes `with_custom_properties=false`, because a
text box has no declarations and copying that table per text node is a vector copy per text node.
**Two lists that must agree is not a duplication smell, it is a scheduled bug**, and the schedule is
"whenever someone adds an inherited property".

Where the algorithm runs is the only design decision in it: after line breaking, because L1 and L2
reorder per *line*; before shaping, because a shaped run must be uniform in direction. And the
reorder is across the **line**, not per box — `<span>שלום</span> world` is one bidi paragraph, and
reordering each span separately is a different wrong answer from no bidi at all.

Measured: `he.wikipedia.org` renders right-to-left with numbers reading forward inside it (11,696
bidi lines, 12,148 runs). `<bdi>` demonstrably does its job — `user: شخص 3 posts` puts the `3`
*before* the name without isolation and after it with. And Hacker News is **byte-identical at 705
display-list commands with zero bidi counters**, which is the assertion that a feature capable of
costing every page in the world something costs the English ones nothing: `NeedsBidi` rejects a line
with no byte at or above 0xD6 before decoding anything.

**One piece of session 34 is not built, and it is not bidi's fault.** The two-position caret has
nothing to attach to: a page's caret is end-of-value only, which `src/engine/PageEditing.cpp` has
said at the top since the editing code was written. There is no caret *position*, so there is no
direction boundary to put two of them at. Recorded in the ledger against the caret model, which is
not on this roadmap at all.

## Session 28 — MSE, and five bugs in the one place a wrong answer is invisible

`92ad317`, `1fcd802`, `37aa536`. `MediaSource`, `SourceBuffer`, `TimeRanges`, the object URL
registry, and the coded frame processing algorithm underneath them.

**The through-line: MSE is the API where a wrong answer looks like nothing at all.** A player reads
`buffered` every few hundred milliseconds and decides from it what to fetch next. A range set that
claims what it does not have makes the player skip a fetch and stall forever; one that omits what it
does have makes it re-fetch the same bytes forever. Neither is a crash, neither throws, and both
present as "the video does not play". Five bugs landed in that shape and each was found by a different
instrument.

**A floating-point test found that seven frames of 40 ticks do not add up to 280 ticks.** Dividing
`decode_time` and `duration` separately and adding is the obvious way to write it, and it lands a few
ulps off — so the eighth frame did not abut the seventh and `buffered` reported a **gap of 2e-17
seconds**, which a player would spend a request trying to fill. The division now happens once, on
`(decode_time + duration)`, which is exact in integers; and a one-microsecond join tolerance covers
what a `timestampOffset` added to both sides can still shift. Both halves are needed and the header
says which does what. The tolerance is not a fudge factor — it is the unit the times are actually
known to.

**The fuzzer found unbounded memory reached through the *eviction* API.** `remove` of a
sub-microsecond span split a range, so `for (i) sb.remove(i*1e-9, i*1e-9+1e-12)` fragments one range
into one entry per call, with the page choosing both the count and the widths. It surfaced as an
invariant violation — two ranges 1e-301 apart — in the first minute of 47 million runs. A coded frame
is milliseconds long, so a span shorter than a microsecond contains no frame and removing it now
removes nothing.

**A page found that `<video>`'s `src` was not a reflected attribute.** `video.src = url` set a plain
JavaScript property on the wrapper and the element never saw it, so nothing reached the attach and
`sourceopen` never fired. The hook was correct; nothing was arriving at it.

**And the one that cost the most: an exception in an event listener vanished completely.**
`EventDispatch` discarded the result of the call, so a `ReferenceError` inside a `sourceopen` handler
produced no console line, no script error, no anything — the only symptom was a page whose output
stopped mid-way. I spent a long time looking at MSE for a bug that was in the event loop. The
specification says such an exception is *reported* and dispatch continues with the next listener;
continuing was right and staying quiet was not. **The lesson generalises past this session**: a
diagnostic channel that silently drops one class of failure is worse than no channel, because its
silence reads as "nothing went wrong there".

**`URL` became a real constructor, and that is not scope creep.** `createObjectURL` has to hang off
something, and a `URL` that answered `typeof 'function'` while throwing from `new URL(href)` is
exactly the stub ADR 0012 forbids: a page that finds it has already taken the branch that assumes it
works. The parse goes through one new virtual on `NetworkSource` to the single parser in `src/url`;
only the *splitting* of the canonical result happens in the binding layer, reusing what `location`
already does. A second URL parser there would be the "two parsers disagreeing about where the host
ends" that `url/Url.h` names as the vulnerability.

Decisions worth knowing before extending this. The quota is checked *before* the bytes are copied,
because checking after means the allocation has already happened; it is per **source** rather than per
buffer, or a page with audio and video holds twice the limit. `QuotaExceededError` is not an error to
avoid — it is the signal a player is waiting for and how it is told to evict — so it is thrown with
that name and the `error` event fires. A `remove` frees *bytes* and not only time, or the quota is
unrecoverable and a player told to evict evicts, retries, and is refused forever. Frames outside the
append window are dropped rather than clamped, because a player can fetch a missing frame and cannot
detect a moved one. And an id resolves to **nothing** once the thing it named is gone:
`MediaElements::Buffer` looks the source up first and checks the buffer is still on it, which is what
turns "the page kept a SourceBuffer too long" into an `InvalidStateError` rather than a
use-after-free.

**The session's check had to be restated and the restatement is the honest part.** The ledger said
"Plex direct-plays a video", which needs a decoder — session 27, blocked on four libraries that need
installing. That is nothing to do with MSE, which is a *buffer* API: the bytes are held, described,
and never looked inside. So the check is now what is actually verifiable end to end, and the gap is
named. `tests/Mp4Fixtures.h` was extracted the moment a second test file needed the fMP4 builder,
because a fixture copied twice is two fixtures that drift — and tests over a drifted fixture agree
with each other and with nothing else.

## Session 29 — MPEG-TS, and the format difference the other two containers hide

`800e261`. The third container, and the one HLS carries. The playlist parser landed in session 26 and
was pointing at segments nothing could read.

**One finding, and it is about what a container abstraction hides.** `IsoBmff` and `Matroska` both
produce `MediaSample` — a `track_id`, an `offset`, a `size` — and after two demuxers that shape looked
like the module's vocabulary rather than a property of two particular formats. It is not. In a
transport stream an access unit is carried across however many 188-byte packets it takes, and **every
one of those packets puts a four-byte header in front of its payload**, so the payload pieces are
never adjacent in the file. A single-range sample cannot describe one.

The first draft did the obvious thing: it tracked `next_expected` and set a `contiguous` flag. The
flag was always false for anything spanning two packets, and the parser dropped those samples — which
is every video frame in every real stream. A test that appended three packets and expected two access
units got one, and that was the whole diagnosis. `MpegTsSample` now carries a list of ranges and a
total, so the ADR 0013 line still holds: this module reports *places in the input* and copies nothing.
The lesson is about the third implementation of anything: two agreeing on a shape is not evidence the
shape is right, and the cost of finding out at the third is a struct rather than a rewrite.

The rest of the format's character is refusals, and they cluster in a way worth noticing: **almost
every decision here is "refuse and count" rather than "fail" or "guess"**, because a transport stream
is designed to be read through damage. It resynchronises when it loses sync, twice — at the start,
because a live tune-in or a byte-range request begins mid-packet, and again after any corruption. It
discards the access unit being assembled when the continuity counter skips, rather than joining across
the hole: half a frame stitched to half of a later one is a frame no decoder can reject, which is
worse than a missing frame the player can see is missing. It ignores a duplicate continuity value,
which is legal and deliberate in a broadcast. And it reports counts of all three, because a caller
needs to know how much it had to recover from.

Two refusals are not about damage. A **scrambled** packet is refused rather than passed on, which is
ADR 0028 §5's EME refusal reaching all the way down to the container — handing scrambled bytes to a
decoder as though they were media is feeding it input nothing checked. And a **stream type outside ADR
0031's five** is refused at the PMT, which is ADR 0013's whole argument for owning this layer: the
stream type is the only thing that decides what a decoder will be asked to decode. MPEG-2 video and
AC-3 are common in real transport streams and neither is in the five. The *kind* is still reported, so
a caller knows it found a video stream it cannot play rather than no video stream at all.

Two absences are stated in the header rather than left to be discovered. There is no duration in a
transport stream, so `MpegTsSample` has **no duration field at all** rather than a zero a caller might
read as "instantaneous". And the 33-bit timestamps are reported with their wrap intact, because
unwrapping needs to know where the previous segment ended — the caller's state, not this parser's.

The fuzz target checks what a *caller* depends on rather than only memory safety: every reported range
lies inside the input, a sample's pieces are ordered and non-overlapping, and `total_size` is their
sum. That last one matters because a caller allocates from the number and copies by iterating the
pieces, so a mismatch is a heap overflow at the far end rather than here. 11,926,908 runs clean.

**The check had to be restated for the second session running, and the reason is the same one.** "A
Plex transcoded stream plays" needs a decoder — session 27, blocked on four libraries that need
installing. Both sessions 28 and 29 wrote checks against playback and both are verifiable only up to
the point where bytes would be handed to a codec. That is worth flagging as a property of the ledger
rather than of these sessions: several remaining media checks are written against an end state that
one blocked session gates, and each will need the same restatement until it lands.

## Session 35 — animation, and the third instance of one bug

`5c6797c`. `transition`, `@keyframes`, the easing functions, and the frame deadline that decides
whether the loop sleeps.

**ADR 0014 §5 named the risk and it was right to.** "An animation system that keeps a 60Hz loop alive
on a static page is the most likely way this project loses its central property." So the interface is
shaped so the loop cannot stay awake by accident rather than shaped for convenience:
`NextDelayMs` returns *nothing* when nothing is running; a finished transition is **removed** rather
than parked at progress 1, because one left in the map would answer "yes, I need a frame" forever;
and `animation-play-state: paused` contributes no deadline at all. The test drives a transition to
completion frame by frame and asserts the map is empty and the deadline is gone. Hacker News is
byte-identical at 705 commands with zero animation counters.

**The finding is that this session's two bugs were the same bug as session 34's**, in a third place.
Session 34: `src/layout` kept its own list of which CSS properties inherit, and it had drifted.
Session 35, first: `resolver_` is rebuilt in *two* places — a navigation, and a re-parse of the sheets
after a resize — and when re-registering the animation pass was two lines at one of them, the other
silently dropped it. The diagnosis was that `AdjustStyle` was never called at all, for any element,
ever. Session 35, second: the animation clock was set before *painting*, but `InvalidateLayout` also
re-resolves the cascade (to collect background images), so it is a restyle — and a restyle is where a
transition starts. Transitions began at instant zero and were over before anything looked at them; the
symptom was an animation that showed only its final value.

All three are the same shape: **a thing established in one place and quietly invalidated by an
assignment in another.** No error, no crash, no failing test — the feature simply does not happen.
What the three have in common is that the invariant was expressed as *a step a caller must remember*
rather than as something the type enforces. The fixes were the same too: one function that both
callers go through (`css::InheritInto`, `Page::ResetResolver`), and, where that was not possible,
writing the ordering requirement into the header where the next person will read it before they need
it. That is worth generalising: when a feature can be turned off by a line somewhere else, the
question is not "did I remember" but "can it be remembered from the wrong place".

The easing functions are the part worth having separately, because they are a pure function of one
number and therefore the only part checkable against the specification's arithmetic rather than against
a rendering. `ease` and the other keywords are stored *as* their cubic Béziers, so a page spelling one
out by hand and a page using the keyword animate identically — they would not if one were a curve and
the other a special case. The Bézier is inverted by Newton falling back to bisection, because the
derivative is zero at t=0 for `cubic-bezier(1, 0, 1, 1)` and a Newton step there leaves the curve. The
four `steps()` positions are one piece of arithmetic over the same two numbers.

Three decisions in the interpolators that a first draft gets wrong:

**Colours interpolate in premultiplied alpha**, which is the whole reason it is not three lerps: red
fading to transparent blue interpolated per channel passes through a half-transparent purple.

**A length whose units do not match snaps at the halfway point** rather than producing a number.
`10px` to `50%` needs a containing block the cascade does not have, and `auto` is not a number until
layout runs. A wrong number here would be invisible where a snap is not — the same reasoning as
session 29's refusals.

**Progress is not clamped to [0,1].** `cubic-bezier(0, 1.5, 1, 1)` overshoots, which is what a page
asking for a bounce is asking for. The clamp lives where a range is actually *known* — a colour
channel, not a margin — because a margin genuinely can go negative.

Two things are deliberately absent and said so where the code is. **`opacity` is not on the animatable
list**, because the property does not exist in this browser: paint has no alpha compositing pass, so
an animated opacity would interpolate a number nothing reads. That is ADR 0014 §5's own argument for
ordering animation after transform, applied to itself. And **transform interpolation is componentwise
on the matrix**, which takes the chord rather than the arc for a rotation; doing it properly needs the
angle kept beside the matrix, which is a change to `TransformOperation` rather than to the
interpolator.

Verified on a page with three animating boxes, and the render is the assertion: `#a` a few
milliseconds into `slide` has moved 1.4px with its background interpolated `FF0000` → `FD0002` — two
properties from one `@keyframes` — while `#c`'s `steps(4)` correctly holds the first step on *both*,
left still 0 and colour still pure red. A hover on `#b` starts exactly one transition, on the one
property that changed.

## Session 36 — Canvas 2D, and a comment that predicted its own bug

`326aadd`. `getContext('2d')`, the state stack, paths, transforms, text, and `ImageData`.

**ADR 0029 §2's claim was that this would be nearly free because `src/gfx` already is a 2D canvas, and
that held better than expected.** Layout needed *one line*: a `<canvas>` is a replaced element whose
pixels come from somewhere other than the network, and `ImageProvider::ImageForElement` was already the
hook for exactly that — it exists because `<img>`'s URL is chosen from `srcset` and the viewport
together, and "the element answers with its own pixels" turned out to be the same shape. The rasterizer,
the stroker, the path type, the affine transforms and the shaper were all already there.

**The seam is one command type rather than forty virtuals.** `src/bindings` may not see `gfx`, so a
drawing call becomes a `bindings::CanvasOp` and `src/engine` executes it against a real `gfx::Painter`.
A virtual per method would have made the seam as wide as the feature is; a command is *data*, so it can
be counted, bounded and eventually sent to another process — the reasoning `gfx::DisplayList` is built
on. And the graphics state lives with the painter rather than in the binding layer, because two copies
of a graphics state is how a `restore()` ends up restoring something the painter never had.

**Two bugs, and the interesting thing about both is that the feature looked like it worked.** Nothing
threw, nothing crashed, and a page that drew a picture got a picture.

The `width` and `height` **attributes** never sized the backing store — only the JavaScript property
did. So `<canvas width=320 height=200>`, which is how nearly every page in the world sizes one, got the
specification's default 300×150 store. And because the CSS box *is* the attribute size, the symptom was
a canvas with a blank right edge: a layout quirk, not a bug in canvas. Drawing was silently clipped.

`clearRect` cleared nothing. It was written as a fill of transparent black, and every fill path in `gfx`
blends — `Canvas::FillRect` goes further and returns immediately for a fully transparent colour, so the
call did precisely nothing. **The comment directly above that line had predicted this exact failure**
("this one *writes* the colour where the painter blends it… which is the bug this comment exists to
prevent") and the code underneath it did it anyway. That is the finding worth carrying: writing down
the hazard is not the same as checking that the function you called avoids it. The comment was
load-bearing prose attached to a call that did the opposite of what the prose said.

Both were found by probing a real page, and one probe line named both at once:
`read 265,125 -> 16,16,16,255 (canvas 300x150)` — the cleared region was not cleared, and the canvas was
the wrong size. That is the third session running where a temporary print settled in one run what
reading the rendering could not, and the pattern is now clear enough to state as a rule: **when the
output is a picture, the diagnostic channel has to be something other than the picture.**

The refusals are where the security content is, and they cluster the same way session 29's did.
**Tainting is set at the *draw*, never at the read** — a flag computed at read time would have to
re-derive what had been drawn, and getting that wrong means a page reading pixels of an image it was
never allowed to see. A resize does **not** clear it, which would be a one-line bypass of the whole
check. An unparseable `fillStyle` is *ignored* so the previous colour survives, and `lineWidth: 0`
likewise — a page that computed a zero width meant nothing visible, and a hairline instead is a
rendering nobody asked for. A canvas over 16 megapixels, or a document holding more than 64, is refused
and keeps the size it had, because `canvas.width = 1e9` is one line and a loop making canvases is
another.

Two absences, both ADR 0012's rule. **`toDataURL` is not defined**: it needs a PNG *encoder* and this
browser has a decoder, so a page saving an image would read an empty string as success. And
`getContext('webgl')` returns **null**, which is how a page learns there is no WebGL and takes its 2D
path — returning something would send it down a path that fails later and less clearly.

One stated approximation: a non-rectangular `clip()` is intersected with its *bounding box*, because
`gfx::Canvas`'s clip is a rectangle. That clips **less** than asked and never more, so nothing is hidden
that should be visible — which is the safe direction, and the alternative is a coverage mask per clip.

## Session 37 — the answer table, and a bare identifier that could not see an accessor

`ef7a0fa`. ADR 0029 §§1, 5 and 6: what a page is told when it asks about the machine.

**The ADR had already done the hard part, which was deciding.** §6 is a table of answers with reasons
attached, and this session's job was to put those answers in one place — `bindings/Fingerprint.h` — for
exactly the reason `util::kUserAgent` is one constant rather than two: a page may sniff several of these,
and two constants that were meant to agree eventually do not. The governing rule is worth restating
because it is unusual: **constant, not randomised.** A jittered answer is still an answer, it is
distinguishable *as* jittered, repeated sampling averages it away, and meanwhile it breaks every honest
consumer.

**The absences turned out to be the substantial part of the work**, and the test that names them is the
real artifact. `deviceMemory`, `connection`, `getBattery`, `geolocation`, `mediaDevices`, `doNotTrack`,
`globalPrivacyControl`, `fonts`, `userAgentData` — nine things a page can look for. Under ADR 0012's
rule, a page that finds nothing takes whatever path it has for a browser without them; a page that finds
a plausible-looking zero takes the path that assumes it works. Absence is not the *lack* of a decision
here, it is the decision — so it needs a test, or it decays into "nobody has got round to it yet" and
somebody adds one back as an obviously-harmless line.

Two rows are worth the reasoning behind them. **`screen.*` reports the viewport, not the display**: a
display's resolution is a strong, stable identifier readable with no interaction at all, and it is not a
number any page needs — what a page actually wants from `screen.width` is "how much room do I have",
which is the viewport. And **the viewport rounds *down***, not to nearest: a page laying out to the
reported width has to fit inside the real one, and rounding up would make a page that filled it overflow
by up to a quantum.

**`crypto.getRandomValues` is the one entry on the table that is not reduced**, and the asymmetry is the
interesting part: randomness carries no information *about* the machine. A page handed 32 random bytes
learns nothing; a page handed predictable ones has its session tokens guessed. So weakening this would
trade a privacy property for a security hole, which is the wrong direction on this project's priority
order. `util::FillRandomBytes` is `getrandom(2)` then `/dev/urandom` and **never a pseudo-random
fallback** — it returns false and both callers throw, because quietly weak bytes are the failure mode
that ships and is never noticed.

`performance.now()`'s coarsening is on the same table for a different reason, and the ADR says so: it is
a *security* measure — high-resolution timers are what turn cache and speculative-execution side
channels from papers into practical attacks — but the mechanism is identical, so it belongs beside the
privacy answers rather than somewhere else. It floors rather than rounds, because rounding leaks which
side of a boundary the true value was on and that is exactly what repeated sampling recovers.

**And the session found a pre-existing bug in the JavaScript engine, in both engines.** `innerWidth`
threw a `ReferenceError` as a bare identifier while `window.innerWidth` answered 1280. The identifier
path checked the global object with `GetOwn` — which answers with a *stored value* — and an accessor has
none. So **every global the host installs as a getter was unreachable by its own name**, silently, and
had been since `innerWidth` landed. This session added three more such globals (`devicePixelRatio`,
`screen`, and the quantised extents), which is how it surfaced: writing a page that reads the whole
answer table meant writing bare identifiers, and one of them threw.

That is the fourth session running where the finding was not in the feature being built. Sessions 34 and
35 found a thing set in one place and clobbered in another; session 36 found a comment that predicted its
own bug; this one found a lookup path that could not see half of what it was looking in. The common
thread is narrower than "bugs exist": **each was a mechanism that worked for the shape of input it was
written against and failed silently for a shape added later.** `GetOwn` was right when every global was
a value. The inherited-property list was right when both copies were written on the same day. Neither
announced itself when the assumption stopped holding, which is the argument for the architecture lint
existing at all — and for tests that assert absences.

## Session 38 — the first thread that runs a page's code, and a test that had to learn to wait

`7d342a6`. Dedicated workers, structured clone across the seam, and `structuredClone()`.

**The ownership statement is the deliverable as much as the code is.** `AGENTS.md` requires one for any
thread, and ADR 0022 §1 had already written it — so the work was making the code enforce it rather than
promise it. The strongest line is that a worker's `js::Interpreter` is **constructed on the worker
thread and destroyed there**: that is not a discipline, it is a scope. There is no window in which two
threads could both hold it, so there is nothing to synchronise and no lock to get wrong. The borrow list
is empty for the same reason — no DOM, no document, no font, no loader — and what crosses is
`js::SerializedValue` bytes and two atomics.

`Workers::Clear` joins on navigation and the destructor joins whatever is left, so a worker is always
joined and nothing is ever detached. A detached thread holding an interpreter is a use-after-free waiting
for the process to exit. `terminate()` joins **before returning**, which is what stops a page that
terminates and then navigates from racing it — and the assertion for that is not a count, it is the line
*after* `terminate()` in the page's own handler running at all.

**Zero idle CPU survives, and the mechanism is the one the sockets already use.** A worker with nothing
to do blocks on its condition variable. The main loop is woken by a pipe the worker writes one byte to,
handed to the platform wait beside the sockets — no polling, no timer, and a page with no workers adds no
descriptors at all. One byte per *batch* rather than per message: if the loop has not drained the last
signal, another tells it nothing. The drain reads the pipe **before** the outbox, not after, because the
other order loses a message posted between the two — the byte gets eaten and the message is left, and the
loop goes back to sleep holding it.

`new Worker` is synchronous while its fetch is not, and the shape that makes that work is worth
recording: **the id and the inbox exist before the thread does.** A page constructs a worker and posts to
it immediately; the messages queue in an inbox that is already there, and the thread drains them on its
first iteration. A script that never loads produces an `error` event, which is what the specification
says and is why returning an object for a doomed worker is correct rather than a lie.

The one security check on the whole feature is that the script is **same-origin**. A worker runs a page's
own code with its own heap; a cross-origin script would be another origin's code running with this page's
messages.

**The finding was about the test, not the code.** The first version turned the crank in a tight loop 2000
times, because that is what every other engine test here does — a canned transport answers instantly and
there has never been anything to wait *for*. It passed when run alone and failed in the full suite, where
the other shards had the cores: 2000 spins take microseconds and the worker thread had not been scheduled
yet. The fix is to sleep a millisecond when nothing is runnable and give up on a wall-clock deadline,
which is what the real loop does by blocking on the pipe. **A busy-wait against another thread is a test
that passes on an idle machine** — and it is a new failure mode for this suite, because until now nothing
in it was concurrent. Every future test that involves a thread inherits the rule.

Worth noting what the structured clone bought without extra work: it already existed in `src/js`, written
for `history.pushState`'s state, and it was exactly the right thing for a worker message *and* for
`structuredClone()`. A `Map` survives both crossings with its contents — serialised in the page's heap,
rebuilt in the worker's, read there, and the answer serialised back — where `JSON.parse(JSON.stringify())`
loses it on the first. Cycles deserialise to one object rather than a tree, which is the property a page
that stores a graph and reads back a tree cannot see it has lost.

TSan clean, which is the check that matters for this one and the reason it was worth running the whole
suite under it rather than the new tests alone.

## Handoff — sessions 27 and 39 both part-landed, on purpose

`e3b07c2`, `17080b8`, `6ec0d72`. Two sessions are `in_progress` in the ledger and both have their
remaining work written out there in order. This entry is about *why* they are in that state, because a
half-finished session is normally a smell and these two are not.

**Session 27 was unblocked mid-session.** It had been stuck for four sessions on four libraries that
needed a `sudo` this agent does not have, and the user installed them while session 39 was in progress.
The ledger's rule is to take the lowest unfinished session, and 27 is lower than 39 — and it *gates* the
media checks that sessions 28 and 29 had to have restated. So the right move was to stop 39 where it stood
and pivot, which is what the two partial commits are.

**Both partial commits are value types and declarations, and that is what makes them safe to land.**
`e3b07c2` is grid's `GridTrack`/`GridPlacement`/`GridStyle` with no algorithm; `6ec0d72` is the decoder
protocol's message shapes with no bodies and deliberately no CMake entry, so nothing links against
functions that do not exist. In both cases the *reasoning* is the expensive part and the code that will
read it is mechanical: a track is a minmax with equal ends, a placement is line numbers and not cell
indices, a decoder reply is a trust boundary in the direction nothing else here is. Re-deriving those
costs a session; typing the algorithm afterwards does not.

**And one thing went wrong that is worth the entry on its own.** `e3b07c2` added two values to
`css::Display` and did not build: `engine::GeometryQueries`'s `DisplayText` switches over every value with
`-Werror=switch`, and I committed without a full build. That is this repo's one hard rule — never leave a
red tree — broken by me, and `17080b8` is the fix. The lint is right to be exhaustive rather than to
default: `getComputedStyle` answering `''` for a display value that exists would be a page concluding the
element is not displayed, and a compile error is the cheap version of that bug. The lesson is narrower than
"build before committing": **adding a value to an enum is a change to every exhaustive switch over it**,
and this codebase has those on purpose.

Where the next session should start: `docs/roadmap-sessions.json` session 27's `notes`, which lists the
five remaining pieces in order and two findings not to rediscover — that a sanitizer runtime cannot live
inside the decoder's seccomp policy, and that a frame currently crosses inline rather than in shared
memory, which departs from ADR 0031 §3 with the cost written into the header rather than hidden.

## The three errors between this browser and youtube.com — and the two behind them

`78e5e0c`, `05cc89f`, `28fbf81`, `a2be0ed`. Not a roadmap session: a named target, run against the
real page, three errors fixed, and the point of the entry is what they turned out to be.

The three, in the order `microbrowser_snapshot https://www.youtube.com/` printed them:

1. `TypeError: cannot read property 'responseStart' of undefined` — the page's *first* inline script,
   `ytcsi.setStart(w.performance ? w.performance.timing.responseStart : null)`. Guarded on
   `performance`, unguarded on `timing`, because no browser has ever had one without the other.
2. `TypeError: cannot read property 'prototype' of undefined` in `webcomponents-all-noPatch.js` —
   `window.SVGElement.prototype`, and past it `EventTarget`, `Window`, `ShadowRoot`,
   `CustomElementRegistry`, `HTMLUnknownElement`.
3. `ReferenceError: AbortSignal is not defined` in the kevlar bundle —
   `var xh = (typeof AbortController === "function") ? AbortSignal : <own polyfill>`.

**Not one of them was a missing feature.** Every one was a missing *name* in front of a feature this
browser already had. We had `AbortController` and every signal it makes; we had the whole DOM type
hierarchy; we had four kinds of performance entry. What a page needs beyond the behaviour is the
vocabulary to talk about it, and ADR 0012's rule reads differently from this side: *a partial
implementation is what sends a page down the native path*. A browser with `AbortController` and no
`AbortSignal` is a shape the web does not have, so nothing is written to survive it — the detection
passes and the next line throws.

**Two more appeared behind them, and both were worth the trip.**

`SyntaxError: regular expression is too large`, which only became reachable once AbortSignal stopped
throwing. The pattern is youtube's HTML unescaper: an alternation of all 2,100 named character
references, 18,390 bytes of source, compiling to a shade over 20,000 instructions — about one per
character it was written with. `kMaxProgramSize` was a flat 20,000 with a comment saying it was "far
past any pattern a page contains". It was not. The bound is now the same double bound `src/net` uses
for a decompression — a floor, an allowance per source byte, a ceiling — because **what is being
refused is blowup, and blowup is a ratio**. `(a{100}){100}` still earns only the floor, so nothing
that was refused before is accepted now.

`ReferenceError: HS is not defined` in the kevlar bundle, still open. `HS` appears 964 times in that
file and every visible occurrence is a *local* — a function parameter or a `var` inside a closure. So
this is either a scoping bug in the engine or a masked error of the kind `masked-errors-hide-the-real-one`
describes for reddit. Annex B block-function hoisting is the obvious suspect and is a known gap in
`docs/js-conformance-roadmap.md`. Whoever takes it: the bundle URL changes per request (the `am=`
parameter), so pin a copy before bisecting it.

**Where youtube stops now**: `webcomponents-all-noPatch.js` runs past everything above and stops on
`NodeFilter`, which needs `document.createTreeWalker` and `document.implementation.createHTMLDocument`
— real new API rather than a missing name, and the first thing on that page that actually is.

Method note, because it is the same one three sessions running: none of these five failed a test
first. All five came from running the real page and reading what it said, and the fourth was found by
adding four lines of `fprintf` to `RegExp::Compile` to print the pattern it was refusing — which took
two minutes and is the only way the entity table was ever going to be identified from
"regular expression is too large".
