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
