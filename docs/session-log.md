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
