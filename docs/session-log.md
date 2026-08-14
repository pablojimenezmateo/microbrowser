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

## Twelve steps into youtube.com, and the first one was a stack trace

`5666416` .. `c363385`. Not a roadmap session: the named target again, run against the real page,
twelve fixes. The kevlar bundle went from dying at source offset 1,904,386 to running past
5,614,363 -- past the halfway point of 10.7MB -- and the entry is about the *method*, because eight
of the twelve were not the feature being built.

**The first change paid for the other eleven.** A compiled function now carries the source offset
each instruction came from, sparse and sorted, and `CaptureStack` prints it: `at HS (@1814415)`
rather than `at <anonymous>`. Offsets rather than line numbers, matching what the parser already
reports its errors by, because the scripts this is read against are minified. Every error below was
located by pasting that number into a `python3 -c` that prints the source around it. Four sessions
of this repo's log say the same thing about `masked-errors-hide-the-real-one`; this is the tool that
makes the masking stop mattering.

**The first thing it located was the machine not running at all.** youtube's bundle was reporting
`ReferenceError: HS is not defined` from a file in which every visible `HS` is a local. The counter
said why: `js.compile_bailout_instructions`. 10.7MB of source over a flat `1<<20` instruction cap,
so the whole bundle ran on the **tree-walker**, which is now only the differential engine. The cap
was the wrong shape for exactly the reason `RegExp.cpp` learned last session: **what is being
refused is blowup, and blowup is a ratio.** A floor of `1<<20`, one instruction per two source
bytes, a ceiling of `8<<20`; kevlar measures 2,740,243 instructions for 10,736,041 bytes, one per
3.9. `js.compiled_source_bytes` and `js.compiled_instructions` are the counters that say whether the
bound is near a real page.

Once the machine had it, `@1814415` pointed at `var kY$ = function HS(X){ ... hD.gamma = HS;
return hD }(1)` -- d3's colour interpolator. **A named function expression could not see its own
name**, in *either* engine, which is why the tree-walker differential could not see it: the same
lesson `var` hoisting taught, two engines agreeing is evidence and not proof. Three things put a
name on a FunctionExpression and only one binds it, so it is a parser flag rather than
`!string.empty()` -- a method's function carries the method's name and `{ foo(){ return foo } }` is
a ReferenceError. The binding is immutable *and silent*, because the loud version would take a
TypeError out of `var f = function f(){ ...; f = null }` and lose the rest of the script. And
adding one flag exposed `NewFunction`'s `node.number != 0`, which read every *future* function flag
as "async or generator" -- so the tree-walker refused every named function expression the day the
flag landed.

**The rest were names in front of features, and one bug that had been waiting.**

`console.info` was absent, and an absent console method is a TypeError that takes the rest of a
script with it -- one line of Polymer's legacy shim. The whole family landed; six alias `log`
honestly (nothing is missing behind the name) and eight are *implemented*, because a `console.count`
that lied about counting is what ADR 0012 forbids. That pushed Builtins.cpp past the module cap,
correctly: `console` is the host's diagnostic channel and not part of the language.

`NodeFilter`, `createTreeWalker` and `createNodeIterator` are real API rather than a missing name --
the first genuinely new thing on that page. The hostile-input surface is not a parser: **the filter
is a page's function and it is called mid-walk**, so it can throw, move `currentNode`, or remove the
node the walk stands on. Every step re-reads the tree, a current node no longer under the root ends
the walk rather than resuming from detached memory, and a throw propagates instead of being
swallowed into "reject".

Writing a TreeWalker filter against `node.tagName === 'SPAN'` found the next one: **`tagName`
returned the parser's lower-case name while `nodeName` upper-cased it**, with a comment on the test
calling the difference deliberate. The DOM says they are the same string, every browser agrees, and
`if (el.tagName === 'SCRIPT')` was silently false here. Twenty-nine test expectations moved; each
one was a page-visible answer no other browser gives.

`document.implementation.createHTMLDocument` is the one this repo had a written reason not to
answer -- every `document.*` method was an own property of one wrapper and resolved against
`document_`. Now the surface is on `Document.prototype`, every query resolves against its
**receiver**, and `createHTMLDocument` builds a real second `dom::Document`. The test is the
isolation, both ways. It is also the inversion same-origin iframes will need, and the reason
`DOMParser` was absent.

`MessageChannel` needed a **task** queue, and the timer queue is the right home rather than a
convenient one: it is already the thing that hands the loop a deadline, so `TimerQueue::QueueTask`
inherits zero-idle-CPU instead of arguing for a second wakeup mechanism. A microtask would have made
a page's scheduler starve exactly the work it was written to let through, invisibly.

`matchMedia` goes through `bindings::GeometrySource`, and only half the reason is that the evaluator
is in `src/css` where this module may not look. The other half: `matchMedia('(max-width: 700px)')`
and `window.innerWidth` are one question, and two seams would eventually answer it differently. The
test asserts the `@media` rule that won the cascade, the script's answer and `innerWidth` all agree
at once.

`Range` is two boundary points and one ordering function; `collapsed`, `commonAncestorContainer`,
`compareBoundaryPoints` and `toString` all go through it, because a second implementation of tree
order would disagree about a case nobody tested. The content-mutation half is **absent** rather than
approximate: those algorithms split text nodes and reparent partially-contained subtrees, and a
version that got partial containment wrong would corrupt a page's DOM silently.

**And the bug that had been waiting: a script's thrown value was freed on its way out.** ASan named
it exactly. `RunCompiled` takes the completion out of the machine, calls `DrainMicrotasks()`, and
returns it -- and draining is where `MaybeCollect` runs. Between those two lines the value is a
`js::Value` in a C++ variable, which is the one thing this collector cannot see. Not a new bug: it
needed a script that throws *and* leaves microtasks pending *and* has allocated past the threshold,
which is why nothing had reached it before the bundle ran far enough. `ValueRoot` is the guard.
Reading the root set afterwards found a second hole with no crash behind it yet -- **the module
table was not a root**, and a module outlives its importer on purpose, so the second `import` of a
specifier read freed memory.

The last one is a shape rather than a bug: **`XMLHttpRequest` and `Worker` handed out instances from
a prototype nothing could reach.** They are exactly the two constructors that do not go through
`MakeInterface`, which has set `constructor.prototype` since it was written. So the test is a *list*
of ten constructor names rather than two assertions -- the next hand-rolled one will be wrong the
same way.

Method note, for the fifth session running: **none of these twelve failed a test first.** All twelve
came from running the real page and reading what it said. What changed this time is that the first
one made "what it said" include *where*, and the eleven after it took minutes rather than sessions.

**And then the second tool, for the same reason.** With the bundle running clean the page was still
blank, and there was no way to ask it anything -- the gap this log named for reddit three sessions
ago. `microbrowser_snapshot -eval '<js>'` runs a string in the loaded page's *own* interpreter and
prints what it evaluated to. Two runs, and the investigation turned around:

    document.querySelectorAll("*").length          -> 1234
    customElements.get("ytd-app")                  -> function
    typeof ytInitialData / key count               -> object, 6
    elements whose tag starts YTD-                 -> 2
    document.querySelector("ytd-app").shadowRoot   -> null
    getComputedStyle(ytd-app).display + its rect   -> block, 1280x0
    not display:none / with a non-empty box        -> 1136 / 180

The app registered its elements and has its data, and built **no component tree**: two `ytd-*`
elements where a rendered page has hundreds, and `ytd-app` with no shadow root -- a Polymer element
attaches one when it renders its template, so it never rendered. The zero height is the consequence.
So the next session's question is one question and not three: what stops Polymer's first render.

A third probe narrowed it once more, and this is the sharpest starting point:

    typeof document.body.attachShadow               -> function
    template.innerHTML = "<b>x</b>"; content count  -> 1
    div.attachShadow({mode:"open"})                 -> ShadowRoot
    own names on customElements.get("ytd-app")      -> is, properties, observers, __properties,
                                                       __observedAttributes, _finalizeClass, ...
    typeof Polymer                                  -> function
    e = createElement("ytd-masthead");
      e.constructor.name / e instanceof its class   -> "p" / **false**

**The primitives are all fine** -- `attachShadow` works, a `ShadowRoot` comes back, `template.content`
parses, and the registered class is a real Polymer class with its `properties` and `observers`
finalized. What is wrong is narrower and stranger: an element created for a *defined* name gets a
constructor called `p` -- so an upgrade did happen and did apply some class's prototype -- and is
still not an `instanceof` the constructor `customElements.get` hands back. Those two cannot both be
true unless the prototype applied is not `constructor.prototype` at the moment `instanceof` looks.
Polymer's `_finalizeClass` builds the class in stages, so the suspect is *when* `UpgradeElement`
reads `constructor.prototype` relative to that -- see CustomElements.cpp:112, which reads it once,
after construction. Verify with `-eval` before changing anything: compare
`customElements.get(n).prototype` against `Object.getPrototypeOf(document.createElement(n))`.

The general lesson is the one both tools share. **Every session that ended in "it renders wrong and
I do not know why" ended there because the browser could not be asked.** An error that says where,
and a page that can be questioned, are not conveniences on top of the work -- between them they
turned a session's worth of guessing into twelve fixes and a named next step.

## 2026-08-06 — Performance: the four things that were seconds, and the one that was the white screen

Started from a user report: youtube.com shows a white screen and the window manager repeatedly
offers to kill the process. Both halves turned out to be true and neither was where the previous
session was looking.

### The method, again, and it is the whole story

Every one of the five fixes below was found by **splitting a scope that covered two unrelated jobs**
and re-reading the summary. None was found by reading code. The first split — `engine::Page::Layout`
into `BuildBoxTree` and `LayoutBoxes` — took one run and immediately said 29,097ms against 22ms,
which is not "layout is slow", it is "the cascade is the entire program".

The corollary is worth keeping: **the profiler was unavailable and it did not matter.** `perf` is
blocked in this sandbox and `ptrace` with it, so there was no sampling and no attach. Scopes and
counters found everything, because they answer "how many times, and in which half" — which is the
question — where a sampler answers "where was the program counter".

### What it cost, per page

| page | before | after |
|---|---|---|
| news.ycombinator.com | 9.68s | **1.47s** |
| old.reddit.com | 34.41s | **5.06s** |
| en.wikipedia.org/wiki/CSS | 258.97s | **6.36s** |
| www.youtube.com | 82.4s | **13.65s** |

### The five

1. **The cascade asked every rule about every element.** `StyleResolver::StyleFor` walked all of
   `rules_` per element: 18,360 rules against 686 elements, seventeen times. Rules are now filed
   under the most selective part of their subject compound — id, else a class, else the tag — with a
   universal bucket for everything that names none of those. 29.1s → 1.9s.

2. **A punctuator was lexed by walking all 57 of them,** longest-first, so `.` `(` `)` `,` `=` `;`
   sat at positions 34–57. Minified script is 1.28M punctuators out of 2.05M tokens. Indexed by
   first byte, grouping stable so maximal munch is untouched. Parse 2.61s → 1.59s.

3. **A font stack was resolved once per text run.** This was 227 of wikipedia's 259 seconds and it
   is the one worth remembering, because `font.lookup_hits` read 985,000 and looked *healthy* — it
   was counting the sized-`Font` cache, which was working, while the three full passes above it
   (every font file on the machine, every loaded face, then the catalog's match) went unmeasured.
   Each comparison built a `std::string`, normalizing a family name that had been normalized at
   registration. **A counter on the cheap half of an operation is worse than no counter**: it reads
   as evidence that the operation is fine.

4. **Custom properties inherited by copying.** Every element copied its parent's whole set; a page
   that declares its palette on `:root` copies fifty string pairs into each of 19,000 elements, per
   cascade pass, twice per rebuild, three rebuilds. Copy-on-write. 30.4s → 6.4s. The old comment
   argued for a vector over a map *because* the copy on inherit is the most frequent operation —
   right observation, wrong conclusion.

5. **The white screen: a custom element's prototype went on after its constructor ran.** A derived
   class's `this` comes from its base, and in a real engine it already carries
   `new.target.prototype` when `super()` returns — so the next line may call the class's own
   methods. Ours applied it afterwards, so `super()` handed back a bare HTMLElement. Polymer's base
   constructor begins `this._initializeProperties()`. Twenty-nine of youtube's thirty-two upgrades
   threw on that line, no component ever rendered, and the page was blank.

### Why (5) survived a whole previous session

Because nothing reported it. The early return on a throwing constructor carried a comment saying
"the throw is reported the way any uncaught one is" and **nothing reported it** — `ConstructValue`
hands the error back and the function returned. `Interpreter::ReportUncaught` had existed for
exactly this since `EventDispatch.cpp` lost whole scripts the same way. One line, and the invisible
failure became `TypeError: undefined (_initializeProperties) is not a function`, ×29, which is the
entire diagnosis.

Four more call sites had the identical `(void)CallFunction(...)` — element reactions,
`attributeChangedCallback`, MutationObserver, the view observers — and all four now report. And
`ReportUncaught` carries the *stack*, which `PageScript::RunTiming` had and nothing reached from a
binding did: the errors hardest to place were being reported with the least.

The previous session's note guessed the cause correctly in shape (an element not an instance of its
own class) and wrongly in mechanism (`_finalizeClass` staging). Verifying with `-eval` before
changing anything, as that note instructed, is what caught the difference: the element's prototype
chain was `[p, Element, …]` and the class's was `[p, p, Element, …]`, i.e. the element had been
given the *superclass's* prototype — which no story about staged class construction explains, and
which "the prototype is applied too late" explains exactly.

### Where youtube is now

The bundle runs, upgrades succeed (throws 30 → 1), 59 elements upgrade where 32 did before. It still
does not render: the remaining throw is its dependency-injection container reporting no provider for
a key, at `EhE (@1323410)` —

    if(!V.providers.has(J)){if(P)return;throw Error("nd`"+J);}

which is a question about that page rather than an engine fault, and now a readable one. Two
`instanceof` checks in `Wpt.prototype.resolve` decide whether a missing provider throws or returns
undefined, so that is the first thing to check.

### Tech debt

`docs/tech-debt.md` is new and has seven entries, each with its measurement. TD-0007 is the honest
statement of what is *not* fixed: the loop still runs a page's script to completion in one turn, so
youtube is a single 9.7-second uninterruptible call. Six times faster, same shape.

## 2026-08-06 — Latency: where a page actually waits, and the instrument that had to exist first

**Status:** done
**Check:** `tools/run-checks.sh tests` (all 24 shards, 0 failed) and `asan` (0 failed);
`inflate_fuzzer` 108,781 executions with no crash; display-list counts unchanged on the three
pages that have them (Hacker News 705 commands / 485 runs / 4 fonts, wikipedia 4713 / 4173 / 30).
**Landed:** `566f7d9` the snapshot tool never ran a due timer · `fa423b2` resolve a name once per
page · `55f7b40` a load timeline, and the first paint stops waiting for every image · `34b7842`
TD-0008 and TD-0009 · `ddfcabf` inflate 2.5x · `343e6a4` apply a declaration by view.

**Found — five things, and the ordering of them is the point.**

**1. Every number in `CLAUDE.md` is from a Debug build, and the difference is 4-7x.** `build/` is
`CMAKE_BUILD_TYPE=Debug`; `build/microbrowser-perf/` is Release+LTO. wikipedia is 6.36s in one and
1.13s in the other. Nothing is wrong with the recorded numbers, but a reader comparing them to a
browser is comparing the wrong build, and the process this machine had open at the time --
`./build/microbrowser/microbrowser https://www.youtube.com/`, 172 minutes of CPU -- was the Debug
one. Say which build a number came from.

**2. The perf preset did not build at all.** Two bench files had rotted against
`TextShaper::Shape` gaining a direction argument and `js::Compile` gaining a source length. So the
one build that can produce an honest measurement was the one nobody could compile, which is
probably why (1) went unnoticed.

**3. A ranked scope summary cannot see latency, and these pages are latency.** Hacker News spends
1.21s of a 1.41s load blocked on a socket and every scope in the table put together accounts for
58ms. `util::LoadTimeline` (`MICROBROWSER_LOAD_TIMELINE=1`) is the answer: one navigation, one
clock, printed in the order things happened with a **gap** column, because the row after a long gap
is what the browser was waiting for. It found the next two items within minutes of existing, and it
is the thing to reach for before optimising anything on a real page.

**4. The snapshot tool was rendering a different page from the browser.** `RunLoadToCompletion`
called `Advance()` and not `RunDueWork()`, so no page timer ever ran inside it -- and a page that
armed one made `NextDeadlineMs()` answer zero, which span the loop **376,522 times** on youtube's
front page. Both halves matter, and the second is worse: this repository's whole method is to
render a real page and look at it, and what it was looking at was a document whose timers had never
fired. With the fix youtube lays out 73 times rather than 17 and reaches its media host.

**5. `wikipedia` renders between 4 and 17 of its images at random, and it is not our loop.**
`upload.wikimedia.org` answers **429** to a burst of six parallel HTTP/1.1 connections. Reproduced
with `curl --http1.1` outside this browser (`200 200 200 429 200 429`), and the obvious suspect was
tested and cleared -- it is not the `User-Agent`, which gets 200 on its own. This is the first
*rendering correctness* cost anybody has measured for the missing HTTP/2, as opposed to a latency
cost. TD-0008.

**What was fixed, and what it bought.**

Name resolution was not cached at all: a connection is opened per concurrent request and every one
of them called `getaddrinfo`, which is the one call in the stack that blocks the loop. Hacker News
resolved one host four times, youtube thirteen times across three hosts, old.reddit about thirty.
Now once each. **The cache is keyed by the ADR 0005 partition key and `Transport::StartConnect`
grew a partition parameter to make that structural** -- a warm name answers in microseconds and a
cold one in tens of milliseconds, so a host-keyed cache is a "has this browser been to that site?"
oracle for every site on the web. Same argument as the connection pool, same argument as TLS
tickets being off.

`PendingLoad::MayPaint` required `images_outstanding == 0` -- the first frame waited for every
image on the page. Hacker News painted at 1116ms and the last thing it waited for was `s.gif`, a
spacer; wikipedia painted at 1058ms with its stylesheets in hand since 403ms. Images now stay owned
by the load (so `load` still means the document *and* its subresources, and no response is dropped)
but do not hold the frame, and ones arriving afterwards are decoded in a **batch** -- eleven of
wikipedia's finish within 250 microseconds of each other, and one relayout each would be eleven.

And `StartImageRequests` ran at the document and at each paint but not when a *stylesheet* landed,
which is the moment the cascade first names a background image. Hacker News put `triangle.svg` on
the wire at 1104ms for a sheet that arrived at 726ms. Now 734ms.

**Two instruments earned their place immediately and should be extended rather than replaced.**
`bench/CodecBenchmarks.cpp` and `bench/CssBenchmarks.cpp` are the first benchmarks for anything
outside gfx and js, and both exist because the alternative was timing a page load on a shared
machine -- where the same binary read three times slower while something else was linking, which is
larger than any change either file was measuring. The codec one carries its own DEFLATE encoder,
since `src/util` deliberately has none, and **verifies its corpus by decoding it before timing
anything**: a benchmark measuring a decode that failed on the first symbol reports a wonderful
number.

**Left.** TD-0005 is still open and is now the largest non-JavaScript item on wikipedia: the
duplicate cascade in `CollectImages`. Two routes were considered and both have a catch worth
knowing before starting. Collecting backgrounds from the *box tree* instead removes the second
cascade but pushes the requests after the first layout, which is exactly the 375ms regression the
`StartImageRequests` fix above just removed. Caching the box tree -- which `Page::Layout`'s own
comment proposes, and the `boxes_` member and the eight `boxes_.reset()` call sites are already
most of the machinery -- is the better route, but `LayoutEngine` takes the `ImageProvider` as an
input, so an image *arriving* changes the tree; every path that changes an input has to be audited
for invalidation before it can be trusted, and getting it wrong renders a stale page, which is
priority 1 rather than priority 4.

---

## 2026-08-06 — HTTP/2, and the discovery that the protocol was not the fix

ADR 0010 §3 had been the last unfinished part of transport since the connection-reuse work landed.
It is done: HPACK, framing, multiplexing, flow control, the stream lifecycle, ALPN, and the pool
change underneath all of it (ADR 0032). Four things are worth writing down that a diff does not say.

**The fix for TD-0008 was not HTTP/2. It was the coalescing, and I only found that out by
measuring.** TD-0008 was `upload.wikimedia.org` answering 429 to a burst of six parallel HTTP/1.1
connections, which is why `en.wikipedia.org/wiki/CSS` rendered between 4 and 17 of its 19 images at
random. The obvious reading — "HTTP/2 multiplexes, so the burst goes away" — is wrong, and it is
wrong for a reason that is only visible once the pool is in front of you: **ALPN settles the
protocol during the handshake, which is after a socket is open.** Six concurrent images would have
opened six sockets, each independently discovered that the server speaks `h2`, and the page would
have finished with six sessions carrying one stream each. Same burst, same 429, new protocol. What
fixes it is that the pool serialises the first connect to an origin whose protocol it does not know.

Measured, Release build — five runs on HTTP/1.1 and **thirty** on HTTP/2:

| | images drawn (of 19) | `engine.images_loaded` | `engine.images_failed` | connections | TLS handshakes |
|---|---|---|---|---|---|
| HTTP/1.1 | 4, 4, 7, 4, 10 | 6 | 15 | 13 | 13 |
| HTTP/2 | 19 in 28 of 30; 18 once, 15 once | 21, every run | **0, every run** | 3, every run | 3, every run |

**Thirty, and not five, because the first five all drew 19 and I nearly wrote that down.** They
did — and then a sixth drew 15. What is actually deterministic is the network half: same fetches,
same connections, same images loaded, nothing failed, thirty times out of thirty. Two runs in
thirty still do not *draw* everything they loaded, which is a different bug in a different layer
and is now TD-0011. Reporting "19 every run" would have closed TD-0008 correctly and hidden a
one-in-fifteen rendering difference inside the same sentence.

The HTTP/1.1 row was taken **on the same machine the same afternoon**, by commenting out one line —
the `SSL_CTX_set_alpn_protos` call — and rebuilding. That is worth remembering as a technique: a
one-line switch that turns a whole feature off is a same-conditions baseline, and it is far better
than a number from a different day in a different build.

**A page can get slower by rendering correctly, and this one did.** Wikipedia's paint went from
1.62–1.84s to 2.13–2.79s. `wait::Network` is unchanged (252ms against 240ms). The difference is that
the browser now downloads 74KB more and decodes fifteen more images: the HTTP/1.1 run was quick
because fifteen of its requests were refused in a few milliseconds each. Reporting the paint number
without that sentence beside it would have been true and misleading.

**The bug that got all the way to real servers was a dangling `string_view`, and the interesting
part is why the suite did not see it.** `Http2Session::Request` held views; `FetchRequest` filled
one with `request.authority = AuthorityFor(url)`, binding to a temporary that dies at the semicolon.
Every request carried whatever the stack slot held next. news.ycombinator.com answered 400;
example.com, google.com and wikipedia all reset the stream. Meanwhile 29 tests were green and ASan
was clean, because the scripted test server recorded `:path` and nothing else — the one
pseudo-header that was garbage was the one nothing read.

Two lessons, and the second is the durable one. **A pseudo-header nobody asserts on is a
pseudo-header nobody is sending correctly**; the test server records `:authority` now and the test
checks it. And a struct whose fields are *built* by the caller should own its strings — a view field
invites exactly the line that was written, and four small allocations against a network round trip
buys the class of bug being impossible rather than merely absent.

**Two tests earned their keep by being checked against their own absence.** The coalescing test
(`Http2Fetch/SixConcurrentRequestsShareOneConnection`) passed at first *for the wrong reason*: the
scripted transport completed its handshake instantly, so each request finished before the next one
asked the pool for a connection, and six requests shared one session whether or not anything
coalesced. It needed a **held handshake** — `Advance()` returning `Blocked` the way a real socket
does for as many loop turns as the round trips take — before the question "how many sockets?" had an
interesting answer. With that, commenting out the coalescing makes it report six. Do this: a
concurrency test that passes either way is worse than no test, because it reads as coverage.

**What is left here, and it is not more protocol.** `kMaxConnectionsPerPartition` is six, and the
name is now wrong twice: it bounds *requests* rather than connections, and six was the number the
web assumed because six was how many sockets a polite HTTP/1.1 client opened. old.reddit.com defers
91 times for 53 fetches, over six sessions that would each have taken all of them. That is TD-0010.
Raising it is not a one-line change — a hundred concurrent streams is a hundred response bodies
accumulating, bounded individually at 64MB and not at all in aggregate — which is why it is written
down with its measurement instead of done.

---

## 2026-08-06 — ADRs for script slicing and the module loader; TD for Plex/YouTube/Reddit

**Status:** done

**Landed:** documentation only — no code changes.

**Written:**

- **ADR 0036** — script time-slicing: defer `js::Execute` slicing until TD-0003 and ADR 0030 are
  measured; if still needed, yield at bytecode safepoints with microtask-atomic checkpoints.
- **ADR 0037** — ES module loader host design (session 50's split: static graph pre-fetch,
  synchronous resolver; dynamic `import()` pending table). Records that Gate B still needs
  TD-0016, not more loader work.
- **TD-0014** — Plex main bundle `TypeError` @370 (~4.9s wall, 3 display-list commands).
- **TD-0015** — YouTube gstatic font failures at ~39–58s on an ~87s load (56 commands, 0 images).
- **TD-0016** — reddit feed blocked on `<suspense-replace>` hoisting + `requestIdleCallback`
  (214 commands after spread-`super()` fix; `js.dynamic_imports` 0).

**TD-0005** remains open — no commit closed the duplicate cascade in `CollectImages` during this
pass. Wikipedia perf summary still shows `engine::CollectImages` at **3,846ms** self in the
compatibility run.

**TD-0007** now points at ADR 0036 instead of "wants an ADR".

**Left for implementers:** Plex @370 needs offset context; youtube white page is still TD-0013 +
Polymer/DI, with TD-0015 as a late font layer; reddit feed is TD-0016 first.

---

## 2026-08-08 — youtube.com watch plays decoded frames

**Status:** done (watch playback path; home feed still sparse)

**Landed:**

- Root thrown script completions across `error` event dispatch (`ValueRoot`) — flaky watch
  SIGSEGV reading `e.stack` after GC (same UAF ValueRoot already documented for RunCompiled).
- `canPlayType` answers `"probably"` from the MediaSource allowlist (was always `""`).
- `PageVideo` recreates surfaces from decoded frame size; track metadata was often `0x0` and
  `Surface::Update` rejected every VP9 frame.
- `currentTime` advances from frame `timestamp_us`; wake pacing stops when samples are exhausted.
- `videoWidth` / `videoHeight` from the last applied frame.
- Decoder counters: `media.video_sessions`, `media.decoder_samples_fed`,
  `media.decoder_frames` / `_applied` / `_errors`, `media.video_configure_failures`.
- Snapshot drains between `-eval` probes.

**Measured** (Release, `/watch?v=jNQXAC9IVRw`, click + muted `play()`):

| metric | value |
|---|---|
| `media.decoder_frames_applied` | 30–60 |
| `video.currentTime` | ~2 s |
| `videoWidth`×`videoHeight` | **320×240** |
| player-region unique colours | ~240 |

**Home** still ~82 commands / 5 images (history-off nudge / incomplete stamp). Search paints
(~969 commands) with few thumbnails. `js.steps_exhausted` still open on some loads (TD-0018).

**Also landed (same day):** TD-0019 closed — `PageVideo` writes Opus PCM into `AudioRing`,
`Application` owns `SdlAudioDevice`, mute/pause stop the device. Prefer audio
`PlaybackClock` when the sink is running. Test
`PageVideo/TheSinkFollowsMuteAndPauseWithoutDecoding`. Headless may still show
`audio.devices_opened` 0 (`AudioDeviceUnavailable`).

**Left:** codec-lib architecture lint, home-feed stamp, search thumbnail
coverage, TD-0001 layout passes, TD-0003 JS AST arena, A/V sync polish if Opus
rate ≠ device rate.

---

## 2026-08-09 — TD-0001 closed; youtube search layout ceases to be the wall

**Status:** done for the layout half; BuildBoxTree remains (TD-0021)

**Landed:**

- **TD-0001 closed.** `OffsetLaidOutSubtree` (already used for relative/absolute
  placement) now serves flex place, float place, and atomic-inline place-on-line
  when forced sizes match the measuring pass — including stretch that does not
  change the measured cross size. Counters `layout.measure_cache_hits` /
  `layout.measure_cache_misses` mean translations vs forced re-layouts.
- Box-tree invalidation guards: `RunScripts` only drops the tree when
  `MutationVersion` or cascade generation moved; `AddImage` attaches in place
  for declared-size and abspos-filled images, and does not invalidate when there
  is no tree yet. Tests cover both. Opens **TD-0021**.

**Measured** (Release, `/results?search_query=cats`):

| metric | before TD-0001 | after |
|---|---|---|
| `engine::LayoutBoxes` | **127 644 ms** / 101 calls | **~0.45–0.9 s** |
| `layout.block_passes` | **189 M** | **~139 k** |
| snapshot wall | **~157–272 s** | **~18–28 s** |
| `engine::BuildBoxTree` | hidden under layout | **~2–3 s** / ~95–110 calls (new wall) |

**Home** still history-off nudge only (`ytd-feed-nudge-renderer`, 0 rich items) —
server response, not a stamp failure. Search stamps ~10–14 `ytd-video-renderer`
and navigates; watch click-to-play path unchanged in intent.

**Left:** TD-0021 (dirty-subtree or style cache), home feed when the server
sends one, search lazy-thumbnail attach rate, TD-0003, TD-0020 facade
`playVideo`.

---

## 2026-08-09 — youtube search blank: `document.all` + `[hidden]` + pre-constructor strip

**Status:** search paints again (~1254 display-list commands, Release)

**Cause chain.** UA `[hidden]{display:none!important}` and `HTMLElement.hidden`
presence reflection were correct and necessary (expandable metadata / Polymer
boolean attrs). With them alone, search went blank: polymer_resin sanitizes
sinks with `!Z && Z !== document.all`. This engine had no `document.all`, so
`undefined !== document.all` was false, undefined became `"zClosurez"`, and
`hidden = "zClosurez"` stuck the attribute on `ytd-two-column-search-results-renderer`
(`hidden="[[data.hideContents]]"`). Separately, stripping binding tokens
*after* the constructor was too late — Polymer deserializes a present boolean
attribute as true during `_initializeProperties`.

**Landed:**

- `Object::Kind::HTMLAllCollection` / `IsHTMLDDA()`: ToBoolean false, typeof
  `"undefined"`, `== null` true, `!== undefined` true; installed as
  `document.all`.
- Strip binding-token attributes **before** ConstructValue (TD-0017 still:
  template contents stay inert).
- Tests for HTMLDDA, `hidden = undefined`, UA `[hidden]!important` vs author
  `display:flex`.

**Measured** (Release snapshot `/results?search_query=cats`): **1254** commands,
~74% non-white pixels, `js.steps_exhausted` = 1. Title still the URL string
(cosmetic).

**Left:** TD-0021 BuildBoxTree, TD-0020 playVideo facade, TD-0018 step budget,
home feed when the server sends one.

---

## 2026-08-09 — TD-0021 style cache: BuildBoxTree half cost on youtube search

**Status:** style half of TD-0021 landed; dirty-subtree box rebuild remains

**Landed:**

- `Document::StructureVersion` (insert/remove) vs attribute-only
  `MutationVersion` / `Element::AttrVersion`
- `StyleResolver` computed-style cache keyed by cascade gen + structure + attr
  + dynamic state + parent style id; adjuster still runs after every hit
- Invalidation counters: `_by_font`, `_by_due_work`, `_by_sheet`
- UA sheet moved to `UserAgentStyleSheet.cpp` (TU cap)

**Measured** (Release, `/results?search_query=cats`):

| metric | before | after |
|---|---|---|
| `engine::BuildBoxTree` | **~2.8 s** / 112 | **~1.4 s** / 108 |
| `css.styles_resolved` | ~132k | **~51k** (+57k hits) |
| invalidation mix | opaque | due_work 26, font 11, image 7, sheet 5, script 1 |

**Left:** dirty-subtree box allocation; TD-0020 facade (`fmt.unplayable`);
TD-0018; home feed when the server sends one.

---

## 2026-08-09 — TD-0020 facade: `fmt.unplayable` with working MSE

**Status:** click-to-play works; facade still `isError`

`-eval` on watch shows `#movie_player.playVideo` exists,
`getPlayerStateObject().isError === true`, `getVideoData().errorCode ===
"fmt.unplayable"`, while every adaptive mime type returns `canPlayType`
`"probably"`. Progressive `formats` is empty. Trusted click still plays via
`ToggleMediaPlaybackAt`. Documented on TD-0020; next is which player check
sets that error despite the type allowlist.

---

## 2026-08-09 — TD-0020 media surface: currentSrc, changeType, load, no false error

**Status:** facade still `fmt.unplayable`; several wrong edges closed

Shipped: `currentSrc` + `error===null`; `SourceBuffer.changeType`; empty
`<video>` uses `MarkNoSource` (no `error` event); MSE `blob:` attach calls
`ResourceSelected` so `play()` is not `NotSupportedError` before the first
buffer; `HTMLMediaElement.load()` resets instead of throwing. A temporary
"NoSource play succeeds" probe cleared `isError` only by skipping autoplay
policy — MSE was empty; not the real fix. Active refusals on watch are
`NotAllowedError` (no activation); player maps that to autoplay-blocked, not
`fmt.unplayable`. Still open: which SABR/player path sets the error code while
buffers hold ~19s.

---

## 2026-08-09 — snapshot `-prelude` + TD-0020 play hooks

**Status:** instrument landed; facade still open

`microbrowser_snapshot -prelude` runs before page scripts. Watch hooks show
only `NotAllowedError` on `play()` (blob src already set), many empty-`src`
`load()` calls, and `Woffle: PES is undefined` Errors — not MediaError / not
`NotSupportedError`. Click-to-play still works.

---

## 2026-08-09 — TextEncoder / TextDecoder (Encoding Standard UTF-8)

**Status:** landed; watch uses them; facade still `fmt.unplayable`

`TextEncoder` / `TextDecoder` are window globals (EncodingBindings.cpp). Only
UTF-8; other labels `RangeError`. Counters: `encoding.text_encoder_*` /
`encoding.text_decoder_*`. On `/watch?v=jNQXAC9IVRw` Release: 47 encodes /
~48KB, 257 decodes — the player PES path was calling `new TextEncoder` against
an undefined name. After this, `typeof TextEncoder === "function"` and those
counters move, but `Woffle: PES is undefined` still appears (3–4×) and
`getVideoData().errorCode` stays `fmt.unplayable`. Next platform gaps on that
path: `crypto.subtle` (`au()` wants `importKey`/`sign`/`encrypt`) and
IndexedDB + `BroadcastChannel` (Woffle `g.D8` / `plI`); page sends
`allowWoffleManagement:true`.

---

## 2026-08-09 — `crypto.subtle` subset (ADR 0037)

**Status:** landed; `au()` sees subtle; PES still undefined; facade open

AES-128-CTR + HMAC-SHA256 in `util`; `crypto.subtle.importKey` / `encrypt` /
`sign` in bindings. Watch probe: `crypto.subtle` truthy with the three methods,
but `crypto.subtle_import_key` stays **0** — Woffle fails before key import
(`indexedDB` / `BroadcastChannel` still undefined; `g.D8`/`plI` need both).
`Woffle: PES is undefined` ×3 and `fmt.unplayable` unchanged. Next: ADR 0021
IndexedDB (and BroadcastChannel) or isolate the Gal/`setmediasrc` throw that is
not the Woffle report.

**Left:** dirty-subtree box allocation; TD-0020 facade (`fmt.unplayable`);
TD-0018; home feed when the server sends one.

---

## 2026-08-09 — TD-0020: MSE `updateend` as macrotask + media step budget

**Status:** facade closed on `/watch?v=jNQXAC9IVRw` (`errorCode` null, `isError` false)

Sync `SourceBuffer` `updateend` re-entered SABR `Ty1` under `wSl`→`appendBuffer`
and emptied `d9` before the outer `vW` (`@2341091` ×27 → 0). Events now go through
`TimerQueue::QueueTask` like MessageChannel. `Interpreter::MediaEventBudget` still
tops up the hang guard for media-element events flushed from that task
(`js.media_event_budget_resets`). Release: buffered ~19s, `js.steps_exhausted` 2.

**Left:** TD-0018 residual; home feed when the server sends one; Accept still needs
scroll into `#content` (TD-0022 UX); `Intl` / `eval` throws on side scripts.

---

## 2026-08-09 — TD-0022: wheel reaches fixed overflow under a 0×0 host

**Status:** Accept dismissible via `-wheel` then `-click` without `scrollIntoView`

`Page::ScrollAt` walked `ScrollTargetAt`, which required every ancestor border
box to contain the pointer. youtube's consent `#content { overflow:auto }` sits
in a fixed dialog under a height-0 lightbox, so the wheel fell through while
`scrollTop =` and `elementFromPoint` worked. ScrollAt now starts from
`ElementAt` (same elevated abspos path as clicks). Snapshot: `-wheel x,y,dy`,
`-y` at viewport centre, `-click last` reads `"click":"x,y"`. Counters
`scroll.overflow_moved` / `scroll.viewport_fallback`.

**Left:** TD-0018 residual; home feed when the server sends only a nudge;
`Intl` / `eval` on side scripts.

---

## 2026-08-09 — click default actions follow UI Events retarget

**Status:** Accept over search no longer navigates to a result

Press stored `pointer_down_target_`; release fires `click` at the common
ancestor of press and release; `ResolveClickActivation` walks that element for
form / checkable / link / media defaults. A dialog that removes itself on
`mousedown` can no longer leave the release on a link underneath (youtube
consent Accept over `/results`). Counter `input.click_retargeted`.

Measured (Release, cats search): after `-wheel` + Accept, URL stays
`/results?search_query=cats`, `dialogs:0`, SOCS set, ~7 `ytd-video-renderer` at
500×281. Thumbnail `src` still unset until IntersectionObserver / lazy path
(TD-0018). Home `ytInitialData` still has only `feedNudgeRenderer` (server).

**Left:** search thumbnail `src` attach; TD-0018; home when the server sends a
feed; `Intl` / `eval` on side scripts.

---

## 2026-08-09 — TD-0023: recollect images after observation/`src` writes

**Status:** youtube search thumbs fetch once `img.src` is set

Attribute-only `img.src` (IntersectionObserver lazy path) never rebuilt
`pending_images`. `CollectImages` now runs on attribute-only due work, and
`RecollectDocumentImages` runs after observation callbacks in `PaintAndSend`
(`engine.images_recollected_after_observation`).

Measured (Release, cats search, Accept + `-y 400`): `withSrc`/`complete` 6,
first result thumb `naturalWidth` 720, ~1120 display-list commands / 26 images.
Empty-`src` before scroll or mid-stamp still under TD-0018.

**Left:** TD-0018 empty-src reliability; home nudge; `Intl` / `eval`.

---

## 2026-08-09 — inline replaced percentages fill the containing block

**Status:** youtube search thumb *geometry* fixed

`.ytCoreImageFillParentWidth/Height { width/height: 100% }` matched but
`ReplacedIntrinsic` skipped percentages and `InlineLayout` placed the baked
0×0 (or post-decode intrinsic). `ResolveReplacedSize` resolves against the
line's CB width and the block's definite height; `LayoutInlineChildren` takes
that height. Counter `layout.replaced_percent_resolved`.

Measured (Release, cats search, Accept, no scroll): above-fold `img` used size
**500×281** (was 0×0); `layout.replaced_percent_resolved` thousands per load.
`src` still often unset / `visibility:hidden` until IO assigns (TD-0018).

**Left:** TD-0018 `src` assignment; home nudge; `Intl` / `eval`.

---

## 2026-08-09 — NestedHostBudget: CE upgrades under live rAF frames

**Status:** youtube search in-view thumbs get `src` without scroll

`UpgradeElement`'s `BeginTask` was a no-op while an rAF stamp's frames were
live, so the first lazy-list chunk shared one spent 20M hang allotment and
aborted before Lit/`u5m` installed `onViewportEntered` /
`IntersectionObserver.observe`. `Interpreter::NestedHostBudget` generalises the
MSE media budget: refresh when the machine is empty, when nesting past the
first NestedHostBudget, or when the shared allotment is half spent — not on
every cheap upgrade. `ElementUpgradeBudget` in `UpgradeElement`; counter
`js.element_upgrade_budget_resets` (live-frame refreshes only).

**Check** (Release `microbrowser_snapshot`, cats search, `-wheel` + Accept,
no scroll): `inViewNoEnter:0`, `inViewNoSrc:0`, in-view rows
`hasEnter`/`src`/`visibility:visible` at 500×281; below-fold `hasEnter` true
and `src` false (correct lazy). `js.steps_exhausted` absent;
`js.steps_peak` ≈ 10.3M; `js.element_upgrade_budget_resets` nonzero;
`engine.images_loaded` 18.

**Left:** residual TD-0018 (home feed when server sends one; `Intl` / `eval`);
watch already plays (TD-0020).

---

## 2026-08-09 — `Animation` is constructible (SPA watch)

**Status:** `new Animation()` no longer aborts youtube search→watch listeners

SPA navigation threw `TypeError: Illegal constructor: Animation` (named after
the throw). Native `Animation` now constructs an idle/empty instance; prototype
gains `reverse` / `finish` / `playbackRate` / `startTime` for the
`web-animations-next-lite` completeness probe. Test
`Page/AnimationConstructorIsConstructible`.

**Check:** after Accept + thumb click, `new Animation().playState === "finished"`
and no Illegal constructor; `ytd-player` still often lacks `#movie_player` /
`<video>` on the soft-nav path (cold `/watch` plays). `Error: ad` in observer
callbacks remains (DI / ads slice).

**Left:** SPA watch player stamp; home nudge; `Intl` / `eval`.

---

## 2026-08-09 — snapshot settles youtube `/watch` until `<video>` exists

**Status:** soft-nav and cold watch no longer share the generic 2s drain

`RunLoadToCompletion`'s post-load drain was 2s for every non-reddit URL.
SPA search→watch marks load finished before Polymer stamps `#movie_player`, so
the tool evaluated an empty `ytd-player`. Watch URLs now drain up to 90s (or
until `video` / `#movie_player`), and keep waiting on sockets when there is no
timer deadline yet.

**Check:** cold `/watch?v=jNQXAC9IVRw` after Accept → `video`/`movie` true,
`readyState` 4. Search→watch still needs a reliable in-view thumb click when
the server returns rows.

**Left:** SPA stamp reliability when results are sparse; home nudge; `Intl` /
`eval`.

---

## 2026-08-09 — Symbol `in` unblocks Lit reactive merges (Error: ad)

**Status:** `Error: ad` gone on youtube; SPA watch player data still incomplete

Lit's signal write gate is `SSn in getter` where `SSn` is a Symbol (`gvU` in
desktop_polymer). `BinaryOp::In` used `ToString` on the key, so Symbol brands
never matched, `gvU` was always false, and `vvU` threw `Error("ad")` on every
observer merge (ResizeObserver / visibility). Proxy `getPrototypeOf` also
returned null (AllocateObject never sets a Proxy [[Prototype]]), which would
have broken Lit's `U3D` for proxied plain objects.

**Check:** `SSn in getter` true in jsshell; `Object.getPrototypeOf(new Proxy({},
{})) === Object.prototype`; cats search→watch: zero `Error: ad` lines.
`ytd-watch-flexy.data` keys still often only `["contents"]` (TD-0024 remainder).

**Left:** innertube/player application on soft nav; home nudge; `Intl` / `eval`.

---

## 2026-08-09 — SPA watch: navigate-start without innertube player fetch

**Status:** TD-0024 narrowed; Symbol `in` / snapshot settle already committed

After Accept on a cold watch (SOCS set) → `location.href` to `/results` →
thumbnail click, `yt-navigate` / `yt-navigate-start` fire with `pageType:"watch"`
but **`yt-navigate-finish` never fires** and the load timeline shows **no**
`/youtubei/v1/player`, `/next`, or `/get_watch`. Soft-nav therefore leaves
`ytd-watch-flexy.data` as `["contents"]` with no `#movie_player`. Cold watch
still plays via `ytInitialPlayerResponse` in the HTML.

desktop_polymer's `kxg` prefers `fetchPbj` when `o82` is false, else `WzB`
(player+next via innertube). `window.TextDecoderStream` is absent (gates the
streaming get_watch branch only — WzB is the non-stream fallback). Next: why
the navigation promise never settles (fetchPbj hang vs empty
`watchEndpointMap` / `getRequest`), not more settle time.

**Left:** finish TD-0024; home nudge; `Intl` / `eval`; optional Streams
(`TextDecoderStream` / `TransformStream`) as a real API when streaming watch
is required.

---

## 2026-08-09 — snapshot settles youtube `/results` and tightens `-click last`

**Status:** post-Accept restamp and click hygiene for search→watch harness

Consent Accept on `/results` reloads and can briefly leave zero thumbnails; the
generic 2s drain then let `-click last` reuse Accept's coordinates. Results URLs
now settle up to 45s (or until `a#thumbnail` / `ytd-video-renderer` exist) with
socket waits like watch. Watch settle restored to 90s. `-click last` requires
the immediately prior `-eval` to return x,y (throws and non-coordinate probes
clear the point). Eval stdout is fflush'd so a hung settle still leaves the
last probe on disk.

**Check:** after Accept, thumbs/vr > 0 before a thumb click; `-click last` after
`noThumb` exits 2 instead of clicking Accept again.

**Left:** TD-0024 soft-nav player data; home nudge; `Intl` / `eval`.

## 2026-08-09 — `eval` / `Function` + CSP `'unsafe-eval'` (ADR 0039)

**Status:** WebPO hang fixed; SPA watch still missing `#movie_player`

BotGuard's challenge script uses `(0,eval)(…)`. Refusing `eval` left WebPO's
`wne()` promise unsettled, so innertube never ran on soft nav. `eval` and
`Function` now exist; CSP without `'unsafe-eval'` throws `EvalError` via a host
hook. Successful external scripts also set `data-loaded` for YouTube's `_.VE`.
`MICROBROWSER_LOAD_TIMELINE` raised to 4096 entries (512 truncated mid-fonts on
youtube).

**Check:** jsshell `(0,eval)('(function(x){return x+1})')(41)` → 42; CSP tests;
search→watch after Accept: `ytd-watch-flexy.data` has `currentVideoEndpoint` /
`playerOverlays` / … (not only `["contents"]`). Still `video:false`,
`#movie_player` absent, `yt.player.Application.create` undefined.

**Left:** TD-0024 root cause B — player Application / VE stamp after OK nav data;
home nudge; `Intl`.

---

## 2026-08-09 — post-load player `base.js` fetch (TD-0024)

**Status:** `Application.create` is a function on SPA watch; still no `#movie_player`

`OptionsForSubresource` dereferenced `load_.base` after the navigation cleared
it. CSP trust for `'strict-dynamic'` is now stamped on `createElement('script')`
and flush runs when an already-trusted script is appended. Failed late fetches
dispatch `error`. Timeline: `player_ias/.../base.js` request+run, `/player` and
`/next` 200; probe `create:"function"`, `video:false`, `mp:false`.

**Left:** cause B — stamp `#movie_player` after create exists (eue / apiResolver);
home nudge; `Intl`.

---

## 2026-08-09 — script `load` before `data-loaded` (TD-0024 stamp)

**Status:** SPA search→watch stamps `#movie_player` / `<video>`

Pre-stamping `data-loaded` before firing `load` made YouTube's `P_U` onload
completion skip `OgC`, so `EHT.wja` never called `Application.create` with a
target. Fire `load` first; `hQn` sets the attribute. Regression test:
`Engine/ALateScriptFiresOnloadSoWaitersCanRun`.

**Check:** Release, Accept on `/results?search_query=cats` → thumb click →
`movie:true`, `video:true`, `html5:true`. `readyState` may still be 0 (MSE).

**Left:** SPA playback buffer (TD-0020); home nudge; `Intl`.

---

## 2026-08-09 — HTTP/2 GET retry + PageVideo detach (TD-0025)

**Status:** SPA search→watch reaches playable MSE like cold watch

A shared HTTP/2 session death marked every open stream
`FAILED the connection failed` with no retry. Soft-nav lost
`player_ias/.../base.js` in that blast (`fetch.failed` 81 on one run) while
later innertube `/player` succeeded on a fresh socket — so `#movie_player`
never stamped. GET/HEAD now retry once with `allow_reuse=false`; POST stays
on REFUSED_STREAM / GOAWAY only. Counter `net.h2_retried`.

Also `PageVideo::DetachBuffer` before `removeSourceBuffer` frees the buffer
(decoders held raw `SourceBufferState*` / `MediaTrack*`).

**Check:** Release, `/results?search_query=cats` → Accept → thumb →
`movie:true`, `rs:4`, `buf≈23s`, `isError:false`, `isPlayable:true`;
`base.js` 200; `net.h2_retried=4`; `fetch.failed` 10 (was 81).

**Left:** home feed / `Intl`; any remaining TD-0020 click-to-play UX.

---

## 2026-08-09 — masthead z-index hit-test + trusted input events (TD-0026)

**Status:** youtube home search field is clickable and typeable; Enter still
does not navigate

`#background.ytd-masthead` is `position:absolute; z-index:-1`. Hit-testing
treated every abspos as above in-flow, so `elementFromPoint` returned
`#background` and focus never reached `input[name=search_query]`. Banded
hit order (above / in-flow / below). `getComputedStyle().zIndex` serializes.
Typing fires trusted `input` (four events for `"cats"`).

Enter is still `preventDefault`'d by the page without a URL change — TD-0026.
Home's empty rich-grid is the history-off nudge, not a stamp miss.

**Check:** after Accept, click search centre → focus input; `-type cats` →
`value==="cats"`; `z-index` of `#background` is `"-1"`.

**Left:** TD-0026 search submit; home rich items when the server sends them.

---

## 2026-08-09 — HTMLAnchorElement pathname closes TD-0026 (youtube home search)

**Status:** home → type → Enter reaches `/results` with stamped video renderers

YouTube's Enter handler (`U` → `H` → `_.WFD` → `resolveCommand`) calls `n0n`,
which does `createElement('a'); a.href = location.href; a.pathname.startsWith`.
`pathname` was undefined; the throw aborted navigation. Landed
HTMLHyperlinkElementUtils on `HTMLAnchorElement` via shared `SplitHref` (also
dedups `location`/`URL` splitting). Companion fixes from the dig:
`KeyboardEvent` init dict, snapshot `code` on `-key`/`-type`,
`EvaluateScript` follows pending navigations, fragment location no longer
starves same-turn `requestSubmit`.

**Check (Release):** after Accept, focus search, `-type cats`, `-key Enter` →
`location.pathname==="/results"`, `search==="?search_query=cats"`,
`ytd-video-renderer` count 13. No `startsWith` of undefined in the Enter path.

**Left:** home rich items when the server sends them; residual
`undefined (bound)` / `ReferenceError: w` noise on the page.

---

## 2026-08-09 — `Location` interface (prototype + instanceof)

**Status:** `typeof Location === "function"` and `location instanceof Location`

`window.location` was a plain object with own URL parts. Pages and polyfills
that touch `Location.prototype` or `instanceof Location` saw a missing global.
`MakeInterface("Location")` now owns the prototype; `href` / parts /
`assign`/`replace`/`reload` live there; `WriteLocationFields` only refreshes
`#href`. Same `SplitHref` as anchors/`URL`.

**Check:** DomBindings Location tests; `data:` snapshot
`location instanceof Location` true.

**Left:** home rich feed when the server sends items; watch settle wall time;
residual `ReferenceError: w` / `undefined (bound)` throws.

---

## 2026-08-09 — `new URL(location)` coercion (TD-0027)

**Status:** closed for URL-taking bindings; home skeleton diagnosed

User-visible home was the CSS skeleton grid. Root cause of the broken consent
`continue=` URL was `new URL(location)` →
`https://www.youtube.com/[object%20Object]` because `URL`'s constructor used
pure `js::ToString` instead of `ToStringOf` (Location's `toString` → href).
Same helper applied to `fetch` / `Request` / `location.assign|href=` / XHR
`open` / history URL args / anchor `href` setter.

**Measured (Release):** `new URL(location).href` is `https://www.youtube.com/`;
consent `a[href]` list has zero `object Object` entries; after Accept, SOCS set,
dialogs 0, `ytd-feed-nudge-renderer` text "Your YouTube history is off…",
`#home-page-skeleton` absent, `ytd-rich-item-renderer` 0 with
`ytInitialData` contents `["richSectionRenderer"]` only — server history-off
nudge, not a failed rich-item stamp. No `/youtubei/v1/browse` (guide +
feedback only).

**Left:** home rich items only when the server sends them; TD-0021 dirty-subtree;
residual `ReferenceError: w` / `undefined (bound)`; non-URL `js::ToString`
audit (TD-0027 remainder).

---

## 2026-08-09 — Abspos `min-height:100%` uses the viewport ICB

**Status:** `ytd-app` fills the window; home feed hosts still content-size inside it

User-visible home was a short strip of masthead / skeleton over white. Probe:
`html`/`body` height 0 (every child abspos), `ytd-app { position:absolute;
min-height:100% }` at **128px** — `Layout()` placed root-level abspos against
the root padding box (also 0), and `LayoutAbsoluteBox` never clamped
percentage min/max-height against the containing block (LayoutBlock's own
clamp passes the used content height, making `min-height:100%` a no-op).

**Fix.** `Layout(root, width, viewport_height)` builds the ICB as
`{0,0,width,viewport_height}`; `Page::Layout` passes `viewport_.viewport_height`.
`LayoutAbsoluteBox` re-clamps with that CB and relayouts when min/max bites
(`layout.abspos_min_max_height_relayouts`). Test
`Layout/AbsposMinHeightPercentUsesViewportIcb`.

**Measured (Release, no wheel):** `ytd-app` **900×1280 at y=0** (was 128);
`layout.abspos_min_max_height_relayouts` ~479; display-list ~165 commands /
61 runs / 15 images (was ~40/0/1 before Accept paths). `#content` /
`ytd-page-manager` still ~130/74px content-sized inside the filled app —
**TD-0028**.

**Left:** TD-0028 (page-manager / nested flex height); rich items when the
server sends them; watch settle wall.

---

## 2026-08-09 — youtube skeleton: event UAF + `[object Object]` fetch

**Status:** interactive youtube home no longer ASAN-aborts on geometry; bad
`GET /[object%20Object]` gone

User report: app stuck on home skeleton. Two platform bugs stacked on the
consent / settle path:

1. **UAF (TD-0029).** `DispatchEventTo` drained microtasks then read
   `defaultPrevented` on an event that was only a C++ local. Script `load` on
   youtube allocated past the GC threshold inside a `then` → Collect freed the
   event → ASAN heap-use-after-free (Release: SIGFPE in `GetOwnProperty`).
   `ValueRoot` for the event across the drain (same root as thrown completions).

2. **`fetch({})` → network (TD-0027 extension).** A plain object with no
   Request shape coerced to `"[object Object]"`, resolved against the document
   base, and issued `GET https://www.youtube.com/[object%20Object]` (timeline
   + `fetch.object_object_url`). Chrome throws TypeError first. Also finished
   DOMString coercion on reflected attrs / `setAttribute` / `encodeURI*` /
   WebSocket / EventSource.

**Measured (Release):** no `object%20Object` request; `fetch.object_object_url`
0; ASAN youtube + `getBoundingClientRect` on the consent dialog completes.
`HTMLElement.click()` on Accept still sets SOCS; trusted `-click` on the
paper-button coords still focuses the dialog instead (hit-test — fixed in the
following entry).

**Left:** TD-0028 page-manager height / nudge visibility; home rich items when
the server sends them.

---

## 2026-08-09 — Hit-testing matches Appendix E (youtube Accept)

**Status:** trusted Accept click sets SOCS and dismisses the consent dialog

After `fetch({})` / event UAF fixes, Accept still failed for a real pointer:
`elementFromPoint` returned `TP-YT-IRON-OVERLAY-BACKDROP` (later `body` sibling,
`z-index:auto`) over `tp-yt-paper-dialog` (`z-index:2202` under `ytd-app`). Paint
already sorted by layer; hit-testing used a three-band sibling walk.

**Fix.** Share `layout/Stacking.h` with paint. Hit-test walks reverse Appendix E
order (collect units into the stacking context). Intervening overflow scroll
between the context and a collected unit is accumulated (`scroll_delta`) so a
`position:relative` Accept under `#content { overflow:auto }` is hittable after
`scrollTop` — without it the static `yt-button-shape` parent always won. Paint
applies the same delta. Floats still beat overlapping in-flow blocks.

**Measured (Release):** `elementFromPoint` on Accept → `BUTTON`; `-click last`
→ `SOCS` set, dialog gone; `ytd-rich-item-renderer` 0 with history-off nudge
(TD-0017). Counter: `engine.hit_tests`.

**Left:** TD-0028 page-manager / nudge height; intervening *clip* for collected
units (TD-0030); button width ~51px duplicate hosts.

---

## 2026-08-09 — Flex definite cross size + `flex: 1` = `0%`

**Status:** correct flex behaviour landed; youtube nudge still collapses

Trusted Accept works (prior commit). Home after Accept still shows masthead
over empty main: `#text-container` ~127px inside `#content-wrapper` /
`#dismissible` at 0 with `overflow:hidden`.

**Fixes.**
1. Pass row flex ForcedSize/stated height into LayoutFlexChildren as
   `definite_cross_size`; single-line stretch fills it (was: overwrite
   container height after children measured).
2. Resolve item `height:100%` against that cross size (stretch skips
   non-auto heights).
3. `flex: 1` / unitless `0` in the shorthand → `flex-basis: 0%`; percentage
   basis against indefinite main → treat as `auto` (CSS Flexbox), so
   auto-height columns size to content.

**Measured.** Flex unit tests green (incl. TD-0028 shapes). Natural youtube
after Accept still `page-manager` ~72 / `dismissible` 0; `-eval` setting
`flex:0 0 auto` on the nudge chain yields ~163px — remaining collapse is
not those three bugs alone.

**Left:** TD-0028 remainder (why wrapper stays 0); TD-0030 clips; natural
`height:100%` / `vh` on `#content` so page-manager fills `ytd-app`.


---

## 2026-08-10 — web-platform-tests, and a plan that several agents can work at once

**Status:** done
**Check:** `./build/microbrowser/microbrowser_wpt --testharness-only --areas tests/wpt/areas.txt`
over 14 areas: **2,432 tests, 260,186 subtests, 5,909 passed (2.3%), 1 crash, 815 timeouts**.
The committed expectations are that run. `dom/` alone: 667 tests, 5,253 subtests, 936 passed
(17.8%), 0 crashes, 228 timeouts.

**No wall-clock number in this entry is a measurement.** Every run was on a loaded machine and a
Debug build, which is four to seven times slower than the perf preset. Where a duration appears it
is context for the timeout entries, nothing more.

**Landed.** `tools/wpt/` — a single-threaded static server (`.sub.` substitution, generated
`.any.js`/`.window.js` tests, `.headers` sidecars), a manifest scanner that classifies 42,185
tests without Python, an expectation store, and a runner that forks a process per test.
`tests/wpt/expectations/` (68,012 lines over 12 files), `tests/wpt/areas.txt`,
`docs/adr/0040-web-platform-tests.md`, `docs/wpt-plan.md`, `docs/wpt-tasks.json` (87 tasks,
14 milestones), `ctest` registration, `tools/run-checks.sh wpt`.

**Found — five things a diff does not say.**

1. **The first bug it found was in the reporting path, and it made the harness report nothing
   at all.** testharness.js runs its completion callbacks in one `forEach` with no try/catch,
   so a throw inside `show_results` silently eats every callback registered after it — ours.
   `show_results` calls `insertAdjacentText`, which this browser does not implement. The page
   had run all eleven of its tests and rendered "1 Pass 10 Fail" into `#log`; the runner saw a
   timeout. `setup({output: false})` in `tools/wpt/harness/testharnessreport.js` is the fix and
   must stay after `insertAdjacentText` lands: the next missing method in that path fails the
   same way.

2. **`*.localhost` is why this needs no root and no /etc/hosts.** glibc resolves every label
   under it to loopback and `url::Host::IsLoopbackOrLocalhost` already treats the whole suffix
   as local, so `www1.localhost:8001` is a real cross-origin origin. WPT's own hostnames need a
   privileged edit; this changes nothing on the machine. The server binds 127.0.0.1 *and* [::1]
   because getaddrinfo answers `::1` for `www.localhost`.

3. **A blocking `read()` on a child's pipe cost the first full run.** `poll` says a descriptor
   is readable, not how much is there; reading in a loop until zero parks the parent inside the
   second `read` of a child that is still working. Symptom: eleven zombies, one live child, and
   a runner that never reaps another — `dom/` sat *stuck at 350 of 667* and finished all 667
   promptly once the read end was made non-blocking. Anything added to that loop must not
   reintroduce a blocking call.

4. **A harness status that is not OK subsumes the subtests, and this had to be learned the
   expensive way.** The first baseline wrote **188,172 `NOTRUN` lines** into `encoding.txt`
   (217,843 lines, one file) — every subtest after the point a test timed out. Those are one
   failure's consequence, not a thousand facts. With the rule, the same run is 29,503 lines and
   the whole store is 60,097 rather than 256,235.

5. **The 2.3% is dominated by `encoding/`**, which has ~250,000 subtests (one per code point in
   the legacy index tables) against ~5,000 for everything else combined. Do not quote the
   aggregate as "this browser passes 2% of the web platform" — `dom/` is 17.8% and the areas
   differ by an order of magnitude. Per-area numbers only, which is task B4.

6. **`TIMEOUT` is the one status that is a property of the machine as much as of the browser,
and it is why the runner retries.** The page's own testharness gives up after ten seconds; a test
that finishes in nine loses that race whenever the box is busy. Two runs of the *same binary*
disagreed on **66 of 2,432** tests, almost all flipping between OK and TIMEOUT. `--retries 1` (the
default) re-runs any result that disagrees with its expectation, and recording additionally re-runs
any TIMEOUT — otherwise the afternoon's load average is baked into the expectations and the next
agent's first run is red for a reason that has nothing to do with their change. Residual flake after
that was 43 of 2,432 on a heavily loaded machine. **The committed expectations were recorded under
load**, so some of the 815 timeout entries are artefacts; re-recording them on an idle machine is
part of task B1. `--retries 0` is how you look for a genuine intermittent.

**Left:** task B1 in `docs/wpt-plan.md` — the full baseline over every checked-out area, then
B2, which is the one that matters: the first 150 `dom/` tests had four distinct messages behind
most of their failures, and a ranked cause list is what turns 20,000 failures into 40 sessions.
Reftests (20,920 of the 42,185) are excluded from `ctest` until fuzzy matching exists (task F2);
without a tolerance an exact comparison against a reference rendered by the same rasterizer
reports antialiasing noise as a failure. `.py` handlers are refused rather than approximated, so
most of `fetch/` and nearly all of `cors/` are unrunnable — that is task H1 and it needs a
decision, not code.

---

## 2026-08-10 — M-B: the baseline, taken an area at a time

**Status:** done
**Check:** filled in below from the run that produced `docs/wpt-baseline.md`.

**Landed.** `docs/wpt-baseline.md` (generated), the expectation files behind it, three harness
fixes, and four runner options that exist because the first three attempts at this run failed.

**Found — six things a diff does not say.**

1. **`.any.worker.html` served a worker URL that nothing answered, and it cost five CPU-hours
   per baseline run.** The generated wrapper dropped `.html` from `foo.any.worker.html` without
   putting `.js` back, so `new Worker()` named `foo.any.worker`, got a 404, and the page then
   waited out its ten-second harness deadline for results that could never arrive. 1,763 of the
   suite's 42,185 tests are `.any.worker.html`. They still TIMEOUT — the worker now loads and
   dies on `importScripts`, which does not exist — but the cause moved from the harness to the
   browser, which is the difference between a bug nobody can act on and plan task G5.

2. **`/resources/WebIDLParser.js` is a rewrite rule, not a file**, and `/interfaces/` was not
   checked out. Every `idlharness` test in the suite — one per specification, each covering that
   specification's whole interface surface — 404ed its parser and reported one failed
   `idl_test setup`. With both fixed, `hr-time/idlharness.any.html` reaches
   `TypeError: cannot set property 'name' of undefined`, which is a bug in this browser and is
   what an idlharness test is for.

3. **`--update-expectations` writes only when the run finishes, and the run does not finish.**
   Three attempts died — two to my own `pkill`, one to a rebuild — and each lost everything.
   21,265 testharness tests is the better part of a day on this machine. The fix is
   `--summary-state`: the summary carries its counts between invocations in a tab-separated
   sidecar keyed by area, an area a run measured replaces what the file said about it, and the
   baseline is therefore taken an area at a time with a commit after each. **Causes are counted
   per area for exactly this reason** — a global count could not be un-counted when its area is
   re-run.

4. **Do not rebuild the binary a driver script is invoking.** ninja's link step leaves the
   output non-executable for as long as LTO takes; every area launched in that window died with
   exit 126 in milliseconds, and the driver cheerfully committed 35 empty results. The rebuild
   was a two-line comment fix. The driver now aborts on exit ≥ 126, and the script says so at
   the top.

5. **The long-timeout budget, not the number of tests, is what makes a baseline expensive.**
   2,946 tests say `timeout=long` — 1,001 of them in `referrer-policy/gen` alone — and it was
   `--timeout * 6`, so each one that will never report cost a minute, plus a minute of retry.
   `--long-timeout` is now its own option, set to 20000 for the baseline: a test in this browser
   reports in well under a second or never, and a long test that genuinely needed more is a
   TIMEOUT expectation, which is visible and revisable rather than six hours of run time.

6. **Reftests are not in the baseline, and that is a decision rather than an omission.**
   20,923 of the 42,185 are reftests; recording them projected the run at nine hours against
   ninety minutes for the testharness half, because a reftest renders two pages — and it buys an
   expectation file that plan task F2 rewrites in full, since an exact-pixel comparison against
   a reference rendered by the same rasterizer calls antialiasing noise a difference. ADR 0040
   §6 is the amendment, with the other five gaps and a decision for each.

7. **The expectations are ~3% flaky against themselves, and that is the first thing to fix.**
   A verification run over the committed baseline — same binary, nothing changed between
   recording and gating — reported **25 unexpected in the first 800 tests**. That is not a
   regression; it is the recorded TIMEOUTs being a property of the machine's load as much as of
   the browser, which session 0 of this ledger already warned about and which the baseline made
   worse by recording under a load average of 20–40. `ctest` would currently be red.
   **This is a measurement, not a diagnosis**: the run was killed at 800 of 21,265 to hand the
   machine back, so 3.1% is an early estimate and the failures were never printed. The three
   candidate fixes, in the order worth trying: raise `--retries` above 1 for a recording run;
   raise the default `--timeout` (the in-page harness deadline is what actually fires); or
   re-record the worst areas on an idle machine. Whoever does this should run the gate to
   completion first and read *which* tests flip — a rate is not a cause.

**Left:** the ranked cause list in `docs/wpt-baseline.md` is what the next sessions are, and the
plan's new §0.5 is what it changed about the plan — seven targets revised, three of the plan's own
assumptions falsified. C1 and C2 — `DOMException` as a real type, and native errors that are the
page's own — stay first because exception identity gates every negative test in the suite, but
they are *not* the largest causes, which is not what the plan assumed. The five that are bigger
are `importScripts` (1,380 tests), `NOTRUN` behind one earlier failure (1,603), `OffscreenCanvas`
(889), the `.py` handlers (512, task H1) and `action_sequence` (147). `testdriver.js` came out of
the triage with no decision attached to it at all and is now plan task B5: 1,158 tests need
synthesised input, this browser has an input path, and nothing exposes it to a page.

**Before anything else, though, finish item 7.** An expectation file that disagrees with itself
makes every later session's first run red for a reason that has nothing to do with their change,
which is exactly the failure mode ADR 0040 §5 exists to prevent.

## 2026-08-10 — C1 + C2: exception identity, and the expectation file that could not spell its own subtests

**Status:** done
**Check:** `microbrowser_wpt dom/nodes/Node-appendChild.html` — 0 unexpected, 7 of 11 subtests
passing against 5 before; `ctest -E microbrowser_wpt` 24/24 green. Areas re-recorded below.

**Landed.** `Object.getPrototypeOf(TypeError) === Error` (`src/js/ErrorBuiltins.cpp`, new);
`DOMException` as a real type with WebIDL's error-names table (`src/bindings/DomExceptions.cpp`,
new); every binding that raises a DOM error converted to the one `ThrowDom`; the DOM's
"ensure pre-insertion validity"; and a harness fix without which none of it could be measured.

**Found — five things a diff does not say.**

1. **One missing pointer was worth 117 tests and 2,014 subtests.** `assert_throws_js` does not
   use `instanceof`. It walks `Object.getPrototypeOf` up from the *constructor* looking for a
   function named `Error`, and in this engine that walk ended at `Function.prototype` — the
   NativeError constructors inherited from it rather than from `Error` (ECMA-262 §20.5.6.1).
   Every negative test in the suite that names a native error type failed on it, with a message
   about the constructor rather than about the thrown value, which is why the baseline's ranked
   cause list read `function TypeError() { [native code] } is not an Error subtype` and nobody
   could tell from that what was wrong. It is one line.

2. **The DOMException bug was not the missing type. It was fourteen sites that put the name in
   the *message*.** `call.Throw("Error", "InvalidStateError: BroadcastChannel is closed")` reads
   correctly to a human and answers `e.name === "Error"` to a page — and
   `catch (e) { if (e.name === 'AbortError') }` is exactly how every cancellable API is used. Two
   more threw a `TypeError` with a comment saying "the spec's NotFoundError" beside it. The audit
   C1 asks for is the whole task; the type was the easy half.

3. **`name`, `message` and `code` are own data properties here, and WebIDL puts them on the
   prototype as accessors.** That is a deliberate deviation with a measurement behind it: the two
   things that print an exception — `js::ToString`, which every console line and every uncaught
   report goes through, and the internal `String(e)` — are pure functions with no interpreter to
   invoke a getter with. With the accessors on the prototype, an uncaught DOMException logged as
   `[object Object]`, which is the most useful line a failing page produces reduced to nothing.
   Non-enumerable keeps the visible half of the shape: `Object.keys(e)` is empty.

4. **The expectation files disagree with themselves for a reason that is not timing, and this is
   the previous session's item 7.** Twelve of `dom/`'s subtests reported `FAIL (expected PASS)`
   beside `MISSING (expected FAIL)` — for the same subtest, deterministically, against the binary
   that recorded them. The cause is in `tools/wpt/Expectations.cpp`: the loader trimmed trailing
   whitespace from every line, and a subtest name can *end in a space* — `test(function(){...})`
   with no name takes the page's `<title>`, and a title written with spaces inside its tags has
   two of them — or contain a newline. Such a name could never match its own recorded
   expectation. Fixed by escaping, marked on the **key** (`FAIL:esc=`) rather than on the value:
   names holding a literal backslash and names beginning with a quote are both already in the
   corpus, written raw, so a value-side convention rewrites thousands that were fine and
   invalidates every file not re-recorded on the same commit. The round trip now has a unit test
   (`tests/WptExpectationsTests.cpp`) — the thing that did not exist and cost a session.

5. **A rate is not a cause, and this one was two causes.** The previous session estimated ~3%
   flake from 25 unexpected results in the first 800 tests and attributed it to `TIMEOUT`s
   recorded under load. At least twelve of `dom/`'s are the escaping bug above and have nothing
   to do with load. Whoever finishes that estimate should re-run the gate *after* re-recording an
   area rather than before: an area recorded by the fixed writer cannot produce this class of
   disagreement again.

**Not done, and named rather than left implicit.** `Node-appendChild.html`'s last two failures
need `frames[0].document` (plan task J1) and `document.doctype` as a node (C10); neither is about
exceptions. The document-specific pre-insertion constraints — one element child, doctype ordering
— are left to C4 with a comment where they would go, because this browser has one document built
by the parser and a page that reaches them is doing something no page does. `assert_throws_dom`'s
remaining suite-wide failure mode is `did not throw`, 88 tests, which is missing *checks* rather
than a missing type, and is C3/C4's work.

## 2026-08-10 — C3: the conversion layer, and the four absences it uncovered

**Status:** done
**Check:** `microbrowser_wpt dom/` — **23.5% → 44.0%** (1539 of 6543 → 2893 of 6570 subtests),
**0 unexpected results**. C3's target was +10 points; this is +20.5. A re-validation run started
while the full `ctest` was building reported 44.2% and two unexpected results, both a `TIMEOUT`
turning into an `OK` or the reverse — the documented flake class, and the reason a WPT number
should say what else the machine was doing. `ctest -E microbrowser_wpt` 24/24 green, 2068
assertions (six new cases). ASan is dirty and was dirty before this branch — see item 5.

**Landed.** `src/bindings/WebIdl.h`/`.cpp` (new): arity, DOMString through `ToStringOf`, nullable
DOMString, `unsigned long` with the Modulo/Enforce/Clamp rules, the DOM's name productions, and
DOMString offsets in UTF-16 code units. `src/bindings/TokenList.cpp` (new): `DOMTokenList` as a
real type. CharacterData's five mutation operations. `document.createEvent` over the legacy alias
table. Element and attribute name validation. `document.defaultView`. `Element.prefix` and
`Element.namespaceURI`. `insertAdjacentElement` and `insertAdjacentText`.

**Found — five things a diff does not say.**

1. **The conversion layer was not where the subtests were, and that is the lesson.** C3 is named
   "WebIDL argument conversion", and writing it took an afternoon and moved almost nothing on its
   own. What moved 1,354 subtests was what having it made cheap to *notice*: four DOM types that
   were absent or approximate, each of which failed a table-driven test file hundreds of times.
   `Element-classlist.html` alone went 535 → **1420 of 1420**. A task phrased as an *ability*
   ("convert arguments properly") is worth what it unblocks, and the way to find that is to rank
   the area's test files by failing-subtest count before writing any code — which took one
   `python3` over the expectation file and should be the first thing every M-C..M-N task does.

2. **`classList` was an object with four methods on it, and every one of the four was the same
   bug.** It treated the `class` attribute as a list of words rather than as an *ordered set*, so
   `"a a b"` had length 3; it had no `value`, no `item`, no indexed access, no `replace`, no
   iteration protocol and no type for `instanceof`; `el.classList !== el.classList`; and — the one
   that is a real-page bug rather than a conformance bug — **`classList.add("a b")` silently wrote
   a class attribute with a space inside a token**, which no selector can ever match again, and
   `classList.add("")` silently did nothing. The specification throws `InvalidCharacterError` and
   `SyntaxError` for those two, and that is the difference between a page's error handler running
   and a page quietly styling nothing. It is a live `Proxy` over (element, attribute) now, cached
   on the wrapper, and parameterised by attribute name so `rel` and `sandbox` cannot grow a second
   copy of the ordered-set algorithm.

3. **`CharacterData.length` was counting bytes.** `data`, `length` and nothing else was the whole
   of CharacterData here — no `substringData`, `appendData`, `insertData`, `deleteData` or
   `replaceData` — and `length` returned `std::string::size()`. So `"café".length` was 5, and every
   offset a page computed from it addressed the wrong character. A DOMString *is* a sequence of
   UTF-16 code units; `src/js/StringUnits.h` already converts between a code-unit index and a byte
   offset with an ASCII fast path, so it is `public:` now rather than reimplemented one seam over.
   Two answers to "what is position 3" is the bug this prevents.

4. **`document.defaultView` was missing, and that is worth more than one property.** It is how a
   script that was handed a *node* reaches the global its constructors live in —
   `ownerDocument.defaultView.DOMException`, `doc.defaultView.getComputedStyle(el)` — which is the
   only correct way to write that once more than one document can exist. Every such expression was
   `undefined.something`. `Document-createElementNS.html` went 1 → 111 subtests from this alone,
   because `assert_throws_dom` takes the global that way on **every negative test in the suite**.

5. **Two measurements that would have been wrong, and how they were caught.** First, `--summary`
   rewrites `docs/wpt-baseline.md` from `--summary-state` alone, and that state file lives in
   `/tmp` — so a regenerated summary silently described 38 areas instead of 200 and looked
   complete. Restored and hand-merged, as the previous session had to; the header now says the
   state file belongs in the repository. Second, ASan reports leaks in
   `ConstructableStylesheets`/`CustomElements` under the ShadowDom tests. Rather than assume they
   were pre-existing because the stack named no file of mine, the branch was stashed and the ASan
   build re-run on a clean tree: 368 bytes in 12 allocations, identical shape. Pre-existing,
   confirmed rather than asserted.

**Deliberate deviations, recorded as expectations rather than papered over.**
`document.createEvent` throws `NotSupportedError` for `BeforeUnloadEvent`, `CompositionEvent`,
`DeviceMotionEvent`, `DeviceOrientationEvent`, `StorageEvent` and `TextEvent` — 18 subtests — because
this browser has no interface behind any of them, and a table row mapping them to `Event` is exactly
ADR 0012's stub: a page that feature-tests one takes the native path into a wall. For the two sensor
events it is also an ADR 0029 fingerprinting surface opened by a table row.

**One choice between two wrong answers, written down where it is made.**
`createElementNS` validates the namespace and then stores the *qualified* name. `dom::Element` has
one name field, which feeds both `tagName` (the qualified name, upper-cased) and `localName` (the
local part) — so storing the qualified name makes the first right and the second wrong, and storing
the local name makes the second right, the first wrong, and loses the name
`getElementsByTagName("x:b")` has to match. Extracting the local name was tried first and cost four
subtests across three files, which is how the trade became visible. Elements carrying a namespace
and a prefix is task C4, and `dom/nodes/case.html` (285 subtests) plus the non-throwing two thirds of
`Document-createElementNS.html` are all waiting on that one field.

**Also: attribute names are looser than element names, and the suite is the reason we know.**
The DOM's "valid element local name" was applied to attributes first, which rejected `setAttribute`
names every browser accepts — `0`, `~`, `'`, `"`, `invalid^Name`, all of which
`dom/nodes/productions.js` lists as **valid**. An attribute name is only ever read inside a start
tag, so only the characters that would break the markup are refused. Getting that backwards is a
page whose `setAttribute` throws where no other browser's does, and it was caught by one subtest
named "Basic functionality should be intact."

## 2026-08-11 — C4, first half: an element that knows which namespace it is in

**Status:** in_progress — the namespace half of C4 landed; the task's 85% check did not, and §Left
says exactly what stands between the two.
**Check:** `microbrowser_wpt dom/` — **44.2% → 52.1%** (3471 of 6660 subtests), 15 unexpected
results and **every one of them an improvement** (a PASS where a FAIL was recorded, or a harness
that used to ERROR and now runs). `dom/nodes` **49.4% → 58.6%**, and `dom/lists` 67.3% → **76.2%**
on a denominator that grew from 49 to 189 subtests, because a test file that used to die on
`createElementNS` now runs to the end. `ctest -E microbrowser_wpt` 24/24 green.
Also measured for regressions, since the change touches every element in the tree:
`custom-elements/parser` 10.0 → **35.0**, `custom-elements/reactions` 15.3 → **21.0**,
`shadow-dom/leaktests` 40.0 → 46.7, `css/selectors` 24.4 → 24.7, `html/dom` +16 subtests. No area
went down. A second full pass over the same seven areas after recording reported **0 unexpected
results**, which is what says the six `TIMEOUT`s the first pass showed were the machine and not
the change — every one of them passes when run alone.

**Landed.** `src/dom/Namespaces.h`/`.cpp` (new): `dom::NamespaceRef`. `dom::Element` and
`dom::Attribute` each carry one plus a prefix *length*, so `tagName`, `localName`, `prefix` and
`namespaceURI` are four answers instead of two guesses at one field. `createElementNS` keeps what
it validates; `setAttributeNS`/`getAttributeNS`/`hasAttributeNS`/`removeAttributeNS`/
`getAttributeNodeNS`/`getNamedItemNS` match on (namespace, local name); `getElementsByTagNameNS`
is new and `getElementsByTagName` learned the case rule; `lookupNamespaceURI`, `lookupPrefix` and
`isDefaultNamespace` exist. `getAttributeNames` and `toggleAttribute` landed with them.
`tests/NamespaceTests.cpp` (new).

**Left.** Three things stand between `dom/nodes` at 58.6% and C4's 85%, and none of them is a
namespace:

- **The node document.** `ownerDocument` answers `document_` unconditionally, and a node script
  made has no document at all until something appends it. The DOM's "node document" is assigned at
  *creation* and survives detachment, so it is not derivable by the walk `Node::OwnerDocument`
  does. `DOMImplementation-createDocumentType.html` (81 subtests) is entirely gated on it —
  every one of its cases asserts `doctype.ownerDocument === aDocument` — and so are adoption,
  `importNode` and `node-creation-realm`. Deciding *where* it lives is the work: a `Document*` on
  every `dom::Node` is 8 bytes on the base class and a second invariant across every subtree move,
  which `src/dom/MODULE.deps` already argues against for `OwnerDocument`.
- **`Attr` as a real node.** `element.attributes[0]` is a record of what the attribute said when it
  was read, not a node with an `ownerElement` and a value that writes through. `attributes.html`'s
  remaining failures are `setAttributeNode`, `removeAttributeNode`, `InUseAttributeError` and the
  NamedNodeMap own-property names, all of which need the type.
- **XML documents.** 390 of `Document-createElement.html`'s and `Document-createElementNS.html`'s
  subtests run "in XML document" and "in XHTML document" — they load `/common/dummy.xml` in an
  iframe. That is task J1, not this one, and it is why those two files stay near the top of the
  failing-subtest ranking however good the namespace model gets.

**Found — four things a diff does not say.**

1. **Ranking the area's test files first paid again, and it pointed somewhere the task title did
   not.** C4 is "dom/nodes — mutation algorithms, node comparison, adoption, ownerDocument". One
   `python3` over `tests/wpt/expectations/dom.txt` put `Document-createElementNS.html` (485),
   `case.html` (254) and `Document-createElement.html` (98) at the top, none of which is a
   mutation algorithm, and all three of which are the one `dom::Element` name field the C3 session
   wrote down as a choice between two wrong answers. The mutation algorithms are further down the
   list than their billing.

2. **The prefix has to be a stored length, not a search for a colon.** An HTML parser meeting
   `<xml:lang>` in an HTML document produces one element whose *whole local name* contains a colon,
   with no prefix and no namespace — and `createElementNS(ns, 'f:o:o')` splits at the **first**
   colon into prefix `f` and local name `o:o`. Deriving the prefix would get both wrong in opposite
   directions. Same for attributes, which is what `Attr-prefix.html` is about.

3. **An intern table for namespaces is a memory leak a page can drive, and that is why
   `NamespaceRef` is a class.** Six URIs cover every real page, so the obvious design is an enum
   plus an append-only table for the rest. `for (i = 0; i < 1e7; i++)
   document.createElementNS('u' + i, 'a')` then leaks ten million entries the collector can never
   reach, because the elements holding them are long gone and nothing else refers to the table. The
   entries are reference counted and the slots are reused; `Namespaces/AnUnknownUriIsInternedAndThenReclaimed`
   is the test that says so, and `dom::InternedNamespaceCount` exists for it and for nothing else.

4. **`docs/wpt-baseline.md` was silently truncated from 297 area rows to 61, by exactly the trap
   its own header warns about, and the warning was not enough.** `--summary` writes the document
   from `--summary-state` alone; the state file lived in `/tmp`, and the one left there by the
   previous session covered **38 areas**, not all of them. Regenerating after a seven-area run
   therefore produced a correctly formatted, complete-looking document about a fifth of the suite.
   It was caught by counting rows in the diff, which is not a thing a reader should have to
   remember to do — so `SummaryAccumulator::Write` now **refuses** to overwrite a document that
   already describes more areas than the run does, and says what to pass instead. The table below
   it is still a hand-merge until one full run writes a state file that covers everything, which is
   now plan task **B6**. The hand-merge also turned up that the aggregate line above the table had
  been stale since M-B: the rows summed to 54,589 of 479,554 while the sentence said 53,220 of
  479,500, because the two previous hand-merges updated rows and not the total. It is recomputed
  from the merged table now, which is why the aggregate moves further than this session's +1,982
  subtests would explain.

## 2026-08-11 — C4, third pass: the mutation algorithms, and the records nobody was queuing

**Status:** in_progress — C4's title is "mutation algorithms, node comparison, adoption,
ownerDocument", and this is the mutation-algorithm half. The 85% check did not land and §Left
says why it cannot until J1 does.
**Check:** `microbrowser_wpt dom/` — **54.2% → 56.1%** (3850 of 7098 → 4025 of 7176 subtests),
**zero regressions**: all ten unexpected results in the verifying run were a PASS where a FAIL was
recorded or a harness that used to TIMEOUT and now runs. `dom/nodes` 62.2% → **64.5%**
(3389 of 5451 → 3567 of 5529 subtests), and its timeouts 85 → 79.
Re-measured for regressions because the change touches every insertion in the browser:
`shadow-dom/`, `custom-elements/`, `domparsing/` and `html/dom/` were run and recorded with it,
and every difference there is an improvement too. `microbrowser_tests` 2078/2078.

**The expectation diff is 29 deletions against 11 insertions, and all eleven insertions are the
same thing**: `Range.insertNode`/`deleteContents`/`extractContents`/`surroundContents` in the two
MutationObserver files, which were invisible while those files timed out. They are **C5**, not a
regression. Two shadow-dom focus tests swapped `harness=TIMEOUT` for a named `FAIL` and back —
both are `promise_test`s awaiting `testdriver`, which this browser does not implement (task **B5**),
so which of the two statuses they land on depends only on how far their setup gets before it hits
the wall. Neither passes either way.

**The bug behind the whole session was a hang, and it was reachable from any page.**
`el.append(el)` built a **cycle** in the document tree. Not a wrong tree — a hang: every walk from
the root loops forever, and the process never comes back. Three files in `dom/nodes`
(`ParentNode-append`, `ParentNode-prepend`, `ParentNode-replaceChildren`) ran the runner to its
wall-clock budget and reported **no subtest at all**, which is why the area's ranking never showed
them: a test that reports nothing has no failing subtests to count.

The cause is that six methods — `append`, `prepend`, `replaceChildren`, `before`, `after`,
`replaceWith` — inserted their arguments one at a time and validated nothing, while
`appendChild`/`insertBefore`/`replaceChild` had gone through `PreInsertionError` since C1. The
DOM does not define them that way and the difference is not cosmetic: all six begin with
**"converting nodes into a node"** (§4.2.6), which turns `(Node or DOMString)...` into *one* node
— the single argument when there is one, a DocumentFragment holding all of them otherwise — and
then **pre-inserts** it. One node means one validity check and one atomic insertion, so
`doc.append(a, b)` on a document that already has an element throws and changes nothing, where
before it half-succeeded.

The fragment that algorithm needs is a **stack** object, which is the one place in `src/bindings`
that a node is not registered in `unattached_`. It is never handed to script — inserting a
fragment inserts its children and leaves it empty — so registering it would be a per-call
allocation that lives until navigation, and `list.replaceChildren()` is how a page clears a list.
Two things follow and both are written where the code is: a refused insertion drains it into
`detached_` first, because script holds wrappers for what is inside it; and it goes in through
`InsertFragmentChildren` rather than `InsertNodeBefore`, because the latter answers
`WrapperFor(child)` and the wrapper cache is keyed by address — a wrapper for a stack fragment
would be a dangling entry that the next node allocated at that address would inherit.

**MutationObserver was failing on a field it never wrote, and that hid six whole files.**
`mutationobservers.js` — which every MutationObserver test in `dom/nodes` runs through — compares
all eight record fields against a default of `null`. Records carried no `previousSibling`,
`nextSibling` or `attributeNamespace`, so every one of those files failed on
`previousSibling didn't match` before reaching what it was about, and then hung to the harness
timeout waiting on an `async_test` that could no longer finish. 42 subtests in
`MutationObserver-attributes` alone, 0 passing. The two siblings are computed inside
`RecordMutation` rather than passed in, because every caller is already standing at the one moment
they are true: an insertion records *after* the nodes are in, a removal records *before* they
leave, so the run is still sitting between them either way.

**And a replacement is one record, not two.** `replaceChild`, `replaceChildren` and
`textContent =` each queued a removal record and then an insertion record; the DOM queues **one**
carrying `addedNodes` *and* `removedNodes`, and an observer that saw two would have to guess they
were related. That needed the specification's "suppress observers" flag, threaded as a `bool
record` parameter through `ClearChildren`, `DetachFromTree`, `InsertNodeBefore` and
`InsertFragmentChildren` — a parameter rather than a member, because that is how the specification
threads it.

**The ordering inside a replacement is observable, and WPT is where it was pinned down.** The
incoming node leaves its old parent *before* the outgoing child is removed, and that removal keeps
its own record. Two tests state the two directions:
`parent.replaceChild(parent.lastChild, parent.firstChild)` must report the last child leaving
*while the first is still beside it* (`previousSibling` is the first child, which is only true if
nothing has been removed yet), and `parent.replaceChild(x, x)` must report a removal and then an
insertion rather than one record replacing `x` with itself. Reading the spec's steps in the
other order produces a browser that passes neither, and the note in its step 11 — "the above can
only be false if child is node" — is the sentence that only parses under the right one.

**Landed.** `src/bindings/NodeMixins.cpp` (new): the six mixin methods and the conversion they
share. `PreInsertionError` moved to `BindingSupport.h` so both translation units can ask it, with
`ChildrenOf`, `InsertedNodesOf` and `PreviousSiblingOf` beside it. `Node.normalize()`, which did
not exist at all — iterative with an explicit stack, because the depth is a page's tree depth and
ADR 0009 exists because that number can be 100,000. `MutationObserver.observe()`'s argument
checking: the four TypeErrors, and the rule that naming `attributeOldValue` or `attributeFilter`
implies watching attributes — *presence*, not truth, so `{attributeOldValue: false}` still watches
them. `removeAttribute` on an attribute that is not there is no longer a mutation. Inserting a
fragment now queues the record its own observers are owed for being emptied.

**The instrument grew again, and it was needed within minutes.** `--verbose` now prints the
subtests behind a non-OK harness. The expectation format deliberately records one line for a
timeout — what is behind one is not yet a fact — but a session diagnosing 85 of them could see
`23 of 38 passed` in the summary and nothing about the fifteen. Every cause above came off that
list. This is the same argument the harness-message line won a session ago, and it is now two for
two.

**Two budgets fired and both were right.** `TreeMutation.cpp` went past the module's translation
unit cap, and the split it was asking for is the file above: the six mixin methods are one
specification section, not six methods, and keeping them together is what stops the next
`moveBefore` from being a seventh copy of the conversion. `DomBindings.h` went past its cap for
the *fifth* time; the four `bool record` parameters bought it back by deleting `AdoptInto`, which
had no callers anywhere in the tree. The manifest's own comment still reads true and is now five
raises old: **what is owed is a split of the class.**

**Left, and it is now three named things rather than a guess.**

- **`Attr` as a real node.** Part of it landed after the commit above and is worth separating,
  because it says what the rest costs. `attributes.js`'s `attributes_are` — the helper a third of
  `attributes.html` runs through — checks `attr.textContent` and `attr.ownerElement` on every
  attribute it is given, and the record this browser hands back had neither. Twenty lines put
  `textContent` on it and made `ownerElement` an **accessor** that re-asks the element whether it
  still carries the attribute, which is the one piece of the real thing that costs nothing: a
  record taken before a `removeAttribute` now answers null afterwards. **+24 subtests in
  `dom/nodes` for twenty lines**, and `Attr-prefix.html` went to green.

  What is left is the part that is a design rather than an addition: identity
  (`el.attributes[0] === el.getAttributeNode('x')` is false here and true everywhere else), a
  `value` that writes through, and `setAttributeNode`/`removeAttributeNode`/`createAttribute` with
  `InUseAttributeError`. All three want the same thing — a table of materialised `Attr`s on the
  element's wrapper, and a detach at every attribute removal and replacement — and that table wants
  two or three new members on `DomBindings`, which has **no header lines left**. It is the first
  piece of work that the class split is actually blocking rather than merely embarrassing.
- **The Range mutation methods** — `insertNode`, `deleteContents`, `extractContents`,
  `surroundContents`. Seven of the eleven remaining failures in `MutationObserver-childList` are
  these, and they are **C5**, not C4. ADR 0012 lists the content-mutation half of Range as
  deliberately absent; it is now the thing measurably in the way.
- **`innerHTML` and `outerHTML` still queue two records where the DOM queues one.**
  `MutationObserver-inner-outer.html` names all three subtests. The fix is the same "replace all"
  this session built for `replaceChildren` and `textContent`, but `InsertParsedHtml` makes its
  fragment internally so the caller cannot see the added nodes, and `DomBindings.h` had no line
  left to widen it with. It is a one-parameter change on the far side of the class split above.

**And the ceiling has not moved.** `dom/nodes` cannot reach C4's 85% from here: 390 of
`Document-createElementNS.html`'s 400 subtests, all 98 of `Document-createElement.html`'s, the 654
in the two `Document-characterSet-normalization` files, and roughly twenty-five `.xhtml`/`.svg`
files run inside an `<iframe>` or in an XML document. That is **J1**, and the honest reading is
that C4's target needs revising with that reason rather than another session of chasing it.

## 2026-08-11 — five tasks in parallel, and the arithmetic that says what 90% of `dom/` costs

**Status:** `dom/` **52.1% → 83.1%**. Six commits on master, four of them from agents working
disjoint areas in their own worktrees, every one verified with **0 unexpected results** before it
was merged.

| commit | what | measured |
|---|---|---|
| `c880e64` | C4: the mutation algorithms and the records they queue | `dom/nodes` 62.2 → 64.5% |
| `29d6e75` | C4: an `Attr` that knows whether its element still has it | +24 subtests for twenty lines |
| `7e01ca6` | C9: `DOMParser`, and the XML parser it turned out to require | `domparsing` 18.1 → 55.7% |
| `48ea116` | D5: `:has()`, and the measurement ADR 0016 priced it behind | `css/selectors` 23.9 → 31.7% |
| `2c787c5` | C6: the target is on both dispatch passes | `dom/events`+`uievents` 30.9 → 35.8% |
| `9e3b4eb` | C5: a Range that changes the tree | `dom/ranges` 8.1 → 80.4% |
| `35d84c2` | a `load` handler ran from inside the walk looking for it | a **segfault**, gone |
| `feea567` | setting a text node's value is "replace data" | 1,561 expectation lines deleted |

**Two of the five sessions found that the measurement was fake before the browser was.** C5's area
reported **259** subtests where a browser reports 31,000 — 24 of 57 files were `harness=ERROR` on
`dom/common.js` throwing at `new Document()`. Opening that one gate took the denominator to 31,129,
which is why `dom/`'s total went from 7,178 to 43,207 subtests *in the middle of a session*. A pass
rate computed against a denominator the browser itself is suppressing is not a measurement, and
this is the second session in a row where the instrument was wrong before the code was.

**The sharpest single finding is C5's, and it is about missing constants.** `dom/common.js`
computes every expected tree-order answer as
`x.compareDocumentPosition(y) & Node.DOCUMENT_POSITION_FOLLOWING`. The constant was undefined, so
`x & undefined` is `0` is falsy, so the helper answered "before" for every pair of nodes — and 930
subtests failed *against a correct `comparePoint`*. **A missing constant does not read as missing.
It reads as a wrong answer somewhere else.**

**A page could segfault this browser, and it took one `<img>`.** `Page::DeliverImageLoad` fired the
page's `load` handler from inside `ForEachDescendant`, which iterates a `children_` vector by
reference; a handler that removes anything invalidates that iterator. Found by ASan after C5
reported the crash in passing. The fix is the rule the event dispatcher already followed —
collect, then dispatch — and only one other walk in `src/engine` calls into script, which already
took a copy.

**`--update-expectations` was deleting the comments the format requires.**
`tests/wpt/expectations/README.md` asks for a `#` naming the ADR on every deliberate refusal; the
writer dropped every one. C5 wrote twenty; re-recording one unrelated test removed all twenty. A
rule the tool undoes is not a rule. Comments now belong to the test they sit above and survive a
re-record — and are dropped only when the test starts passing, because a note saying why something
cannot pass is wrong the moment it does.

**The runner learned to print failing subtests when the harness is OK**, which is the third
instrument fix in two sessions and the one with the widest reach. It printed only *disagreements*
before, so a fully-expected 3-of-25 printed `ok` and stopped — a green light on the exact file you
opened the runner to read. Every cause in the C6 commit came off that list within ten minutes of
it existing.

### What 90% of `dom/` costs, in numbers

The session goal was `dom/` at 90%. It is at 83.1% (35,899 of 43,207) and the remaining gap is one
feature, which is worth writing down precisely so nobody re-derives it:

- **2,280** subtests are visibly failing. Fixing *every one of them* reaches **88.4%**.
- **~5,028** more are invisible, behind 208 tests whose harness never reported.
- **~4,176 of those** are seven files — `Range-{cloneContents,deleteContents,extractContents,`
  `insertNode,surroundContents,set,compareBoundaryPoints}.html` — which are driven entirely by
  `iframe.onload` over two `<iframe>`s. Every assertion sits in an `async_test` that never starts.
- Another **498** are `Document-createElement{,NS}.html`, also iframes, also XML documents.

**So 90% is reachable only through ADR 0027, nested browsing contexts, and no combination of the
other work gets there.** Those seven files need a *fully scriptable* same-origin child — the test
mutates `iframe.contentDocument` and calls `iframe.contentWindow.run()` — so a stub does not pay.
It is the next session's task and it is a large one: `src/engine`'s `Page` is 16,000 lines that all
assume there is exactly one document.

Two further things the goal should be judged against. **431 subtests are tentative proposals**
(`dom/observable/`, `OpaqueRange`) and **126 are a WICG proposal** no browser ships — 1.3 points of
the remaining gap is specs that are not stable, and ADR 0012's rule about stubs is the argument for
leaving them where they are rather than chasing a percentage into them.

## url/ — one URL parser, and the two things that are not URL work · 2026-08-12

**Status:** done
**Check:** `microbrowser_wpt url/` prints
`71 tests in 108032 ms: 9909 subtests, 9695 passed (97.8%), 0 crashes, 31 timeouts` —
from 3,690 of 9,909 (37.2%). `microbrowser_tests` is 2097/2097. `dom/` re-measured at
36,003/43,206 (83.3%) against the 35,899 the C6 entry recorded, so nothing regressed there;
`xhr/`'s nineteen "unexpected" results are all test *names* carrying the server's port number,
which moves between runs.
**Landed:** "One URL parser, and it is the standard's"; "The base element, and three URL failures
that were not URLs"; "FormData, a live iterator, and a string that was two surrogates";
"A sequence is an iterator, and a function's name is not a key"; "A host object is not clonable,
and an href is not a scalar string".

**Left:** 190 subtests and 31 timeouts, and **none of them is a URL bug**:

- **188** are `failure.html`'s iframe third: `frame.contentWindow.location = badUrl` must throw
  that frame's own `SyntaxError`. Plus `data-uri-fragment.html`, `javascript-urls.window.html` and
  `percent-encoding.window.html`, which are three of the 31 timeouts and are entirely
  `iframe.onload`. **ADR 0027**, and the C6 entry above reached the same wall from `dom/`.
- **24** of the timeouts are `*.any.worker.html`. `engine::Workers` runs a worker's script on its
  own thread with its own heap, and its global has `self`, `postMessage`, `name` and nothing else
  — no `importScripts`, no binding layer, so no `URL` or `URLSearchParams` to test. That is
  ADR 0022 §2 rather than a gap: the file says "no fetch, no DOM, no storage" on purpose.
- **1** is JavaScript conformance rather than URL: assigning to a getter-only accessor must throw
  in strict mode, and `Interpreter.cpp` treats the whole engine as sloppy and says so in a comment.
  Nothing anywhere records strictness. TD-0056.
- **1** is `idlharness`, which wants the whole IDL surface introspectable.

**Found:**

**The parser was 97% right and the 3% was structural.** `urltestdata.json` went 865/891 -> 891/891,
but not by fixing a list of cases: **a null host and an empty host were the same state**, so
`sc://x` and `sc:x` serialized alike, and there was **no state override**, which is how the
standard defines every setter — so there were no setters at all, and `url-setters*` (975 subtests)
could not have passed whatever else was fixed. Structure first; the remaining cases (`^` in the
path percent-encode set, an IPv6 tail that stopped early so `[::1.2.3.4x]` parsed, a too-large
IPv4 number becoming a *domain* rather than a failure) fell out in an afternoon after it.

**`tools/urlconf` is why this took an afternoon rather than a week, and it is the transferable
part.** 3,900 pinned vectors against `src/url` directly, in one second, printing the field that
differed. The same vectors through web-platform-tests take three minutes and report
`subtest failed`. Build the direct runner first for any area whose data is a checked-in table —
`encoding/`, `css/parsing/`, `mimesniff/` all qualify.

**The bug was never in the parser anyway.** `URL`, `location.pathname` and `a.host` were answered
by a *string cut* in `src/bindings` — find the first colon, find the next two slashes — because
`src/bindings` may not see `src/url`. That module's `allow:` line is a security boundary about
`js` and `dom`; `src/url` can see neither, so adding it does not widen the model, exactly as
`html`, `css` and `xml` did not. Deleting the cut moved 4,000 subtests. **When a module's
allow-list forces a second implementation of something the tree already has, check whether the
line was actually about that dependency.**

**`<base href>` was not implemented at all**, and it was worth 1,763 subtests on its own: the
standard's `<a>` test page sets `<base>` before each of nine hundred resolutions and got the same
answer for all of them. It is not an ordinary reflected attribute — the getter reads back absolute
— which is presumably why it was skipped when the reflection table was written.

**IDNA needed Unicode 17.0.0, and the version is load-bearing.** Four code points found by running
the vectors: U+04C0 and U+2183 became *mapped* rather than disallowed in 16.0, U+180E became
ignored, and CJK Extension J went from reserved to valid in 17.0. The rest of `src/text` is on
15.1.0 and the two sets do not meet — nothing here reads a line-break class and nothing there
reads an IDNA status — but a session that regenerates one should not assume the other moves.

**The URL Standard's ASCII fast path is not an optimisation.** An all-ASCII domain is lowercased
and returned *whatever UTS #46 thinks of it*: `xn--a` is invalid Punycode, decodes to U+0080, and
reaches a real host in every browser. `xn--a.ß` still fails, because one non-ASCII code point
anywhere puts the whole domain back on the full path. Both halves are in the standard in as many
words, and neither is derivable from UTS #46.

**Three bugs found here were not about URLs at all**, which is the argument for running an area
you did not write: a surrogate *pair* written as two `\u` escapes was stored as two WTF-8
surrogates rather than one code point (so it compared unequal to the identical `\u{...}` literal
and encoded as six bytes where every engine writes four); a function's `name` was an **enumerable**
own property on every function and interface object in the browser; and `new Response(body,
{headers})` dropped the headers, so a response a page built itself arrived with no type.

**Two things about the checks themselves, found while verifying the above.**

**`tools/run-checks.sh` writes to a fixed `/tmp/microbrowser-<target>.log`, and worktrees share
`/tmp`.** A concurrent agent in `.claude/worktrees/wpt-declarative-shadow-dom` was running its own
`asan` at the same time, and its three `heap-use-after-free` reports (in
`TreeBuilder::FlushDeclarativeShadow`, code that does not exist in this worktree) landed in the log
this session was reading. **Check the paths in an ASan report before believing it is yours** — the
file names in the stack are the giveaway. The durable fix is a per-worktree log path; until then,
run the sanitizer binary directly (`./build/microbrowser-asan/microbrowser/microbrowser_tests`)
rather than through the wrapper when another worktree is active.

**The `ubsan` preset had not compiled since `c52dfbf`** and nothing had noticed, because only that
preset carries `-Werror`. Four `-Wsign-conversion` errors in two copies of the same UTF-8 decoder.
Fixed in its own commit. Under it: 2097/2097 and zero runtime errors, which is the first time the
undefined-behaviour checker has said anything about this tree in a while.
## 2026-08-12 — declarative shadow DOM, and three false passes it exposed

**Status:** done
**Check:** `microbrowser_wpt --testharness-only --long-timeout 180000 shadow-dom/declarative/`
prints `7791 subtests, 7731 passed (99.2%) … 0 unexpected results`, from **114 passed (1.5%)**
at the start. `microbrowser_tests` is 2097/2097 including `ArchitectureInvariants`.
`shadow-dom.txt` lost 8,568 lines net.

**Landed.** The whole of `<template shadowrootmode>`, in the four places it lives:

- `dom` — `ShadowFlags` (open / delegatesFocus / clonable / serializable / declarative /
  manual-slot-assignment / template-content) as one mask on the *root*, and
  `Element::AttachShadow` rewritten as the DOM's "attach a shadow root" with the safelist, the
  mode check and the declarative reuse. `Element` **lost** a member doing it (`shadow_open_`
  moved into the mask) and `DocumentFragment` gained none, because `template_content_` folded in.
- `html` — the §13.2.6.4.4 steps, behind an opt-in that defaults to **off**. Document parsing and
  `setHTMLUnsafe` turn it on; `innerHTML`, `insertAdjacentHTML`, `DOMParser` and
  `createContextualFragment` do not, which is the security-relevant half and is what
  declarative-shadow-dom-opt-in.html spends 60 subtests checking.
- `bindings` — `setHTMLUnsafe`, `getHTML({serializableShadowRoots, shadowRoots})`, the four
  `shadowRoot*` reflections on HTMLTemplateElement, `delegatesFocus`/`clonable`/`serializable`/
  `slotAssignment` on ShadowRoot, `attachShadow`'s full init dictionary, and clonable-root cloning.
- `dom` again — one serializer instead of six. `SerializeNode` is a switch and the six virtual
  `Serialize()` overrides call it with default options, so the shadow-aware serializer and the
  plain one cannot disagree.

**Found — the one bug worth the whole session.** `getHTML` was serializing an element's children
and **not the element's own shadow root**. That is 2,176 of gethtml.html's 6,908 subtests, and the
symptom named the wrong thing: every failure had `serializable=true` in its title, which reads as
"the serializable flag is broken". It is not. `serializable=true` is simply the only branch of that
test that calls `getHTML()` *on the host itself* rather than on its wrapper. HTML's "serialize an
HTML fragment" emits the root of the subtree you asked about, and a child walk never does.
`dom::SerializeFragment` is that step, and it is why `innerHTML` and `getHTML` are now different
functions rather than one with a flag.

**Found — three sets of passing tests that were passing for no reason.** All three were exposed by
this work rather than broken by it, and all three are recorded as failures now:

- **45** in `shadow-dom/reference-target/tentative/property-reflection*.html`. They build hosts with
  `setHTMLUnsafe` + `shadowrootreferencetarget`. With no `setHTMLUnsafe` and no declarative shadow
  DOM, no root was ever attached and a chunk of the reflection matrix agreed with the expected
  answer by coincidence. They now reach their actual subject, which is unimplemented.
- **2** in `dom/ranges/Range-in-shadow-after-the-shadow-removed.html`, and this one is a **harness**
  bug: **TD-0054**, the runner does not expand `<meta name="variant">` for a plain `.html` test.
  The file declares `?mode=open` and `?mode=closed`, runs bare, reads `null` out of an empty query
  and calls `attachShadow({mode: null})` — which a correct engine throws on. It passed until now
  only because `attachShadow` treated everything that was not `"closed"` as `"open"`. **A variant
  test run bare does not fail loudly; it silently measures one arbitrary configuration**, and how
  many other areas that has quietly mis-measured is unknown.
- **6** in `custom-elements/element-internals-behaviors.tentative.html`, which asserted that
  `attachInternals({behaviors: [x]})` throws TypeError and got one because `attachInternals` did
  not exist. Now it exists and validates `behaviors` — no behavior type is implemented, so every
  entry is invalid and TypeError is the *correct* answer, not a placeholder.

**Found — the run that measures this is not the run to trust.** The first sweep of
`dom/ shadow-dom/ custom-elements/ domparsing/` took 564 s; the `--update-expectations` sweep of the
same tests took 1,004 s on a loaded machine and produced **31 extra shadow-dom "failures" and a
dozen dom ones that were pure timing flakes** — whole tests flipping `FAIL`→`harness=TIMEOUT`,
scroll/wheel/testdriver files mostly. Committing that would have baked load into the expectations.
Each suspect block was re-run on its own against the *old* expectations to see whether it was really
a regression; 31 of 78 shadow-dom additions and all but two of the dom ones were not. **Re-run a
suspicious block alone before you write it down** — `--update-expectations` believes whatever the
machine was doing at the time.

**Left.** 60 declarative subtests, in three groups, none of them declarative shadow DOM: six need a
script to run *during* the parse (ADR 0030 — each puts an inline `<script>` inside the template so a
MutationObserver fires mid-parse), two need iframes (ADR 0027), and 34 are
`tentative/shadowrootadoptedstylesheets`, which is HTML PR 12339 — import maps resolving CSS module
scripts — and wants the module loader first.

**Eight of that tentative suite were reachable anyway, and the last two came from somewhere else.**
`shadowrootadoptedstylesheets` splits cleanly into three halves and only one of them needs a module
loader: the DOMString reflection is ordinary, keeping the authored value verbatim so `getHTML`
round-trips it is ordinary, and *resolving* a specifier is the part that cannot be done. Doing the
first two is not the stub ADR 0012 forbids, because an **unresolvable specifier is specified to be
skipped silently** — so `adoptedStyleSheets` staying empty is the correct answer rather than an
approximation of one, and it stays correct on the day the resolver lands. The obligation that
creates is written at the accessor in `Node.h`.

The last two needed two unrelated absences that only this suite had reached. Its `support/helpers.js`
uses `import(url, {with: {type: "css"}})` — **dynamic import's second argument**, which the parser
rejected — and a `SyntaxError` is not local: it killed the whole file, so `createStylesheetHost` did
not exist and every test that called it failed on a name. The options bag is parsed and dropped
(there is nowhere to put an import attribute yet), which is enough. Behind that was
`shadowRoot.getElementById` — the DOM's NonElementParentNode mixin, which Document and
DocumentFragment have and Element does not. A component looking inside its own root by id has
nowhere else to go, because the root is not in the document by design. The comment block at the head of the declarative
section in `shadow-dom.txt` says the same thing where the next agent will trip over it.

**Found — ASan is red, and not because of this.** `run-checks.sh asan` fails four of
twenty-five shards. Every *direct* leak in the entire run has one allocation site:
`ConstructableStylesheets.cpp:148`, where `new CSSStyleSheet()` does `storage.release()`
into a hidden slot that nothing ever deletes. It reproduces on a single untouched test
(93 bytes, 3 allocations, every run) and a page drives the count — `for(;;) new CSSStyleSheet()`
leaks forever. That is **TD-0055**. What it cost *this* session is the thing worth noting: ASan
being expected-red is why the real use-after-free below took a deliberate re-read to find rather
than being obvious the first time the suite went red.

ASan did earn its keep once: the first draft of the `</template>` handler flushed the declarative
shadow — which destroys the parser-owned template, since `declarative_shadows_` is its only owner —
*before* `PopUntil` read `TagName()` off the open-element stack. A use-after-free reachable from
`<div><template shadowrootmode=open></template></div>`, i.e. from the feature's happy path, found by
one of the eight new unit tests. Capture the pointer, pop, then flush.

Also left: **TD-0054**, and the `DomBindings` split it forced a first step of. The class has been
"the missing module" for five cap raises; this session moved the two installers that were never
really members (`InstallTemplateShadowReflection`, `InstallElementInternals`) out to a private
`ShadowDom.h`, the standing `LiveRanges.h` already has. A *member* cannot leave a class's header, so
that is where the split has to start.

## 2026-08-12 — C10: reflected IDL attributes, as a table and twelve algorithms

**Status:** `html/dom/` **35.8% → 95.9%** (21,450 of 59,930 → 57,694 of 60,138 subtests).
`html/dom/reflection-*.html` is **56,660 of 56,660**, from 35,560 recorded failures to none.
**35,847 expectation lines deleted.**

**The shape of the work is the finding, and it is not "add the missing attributes".** HTML states a
dozen reflection *algorithms* — a string, a URL, an enumerated value limited to known keywords, a
boolean, five integer kinds, two doubles — and then applies them a few hundred times across the
element interfaces. The browser had about fifty hand-written accessor pairs, which is the shape
that guarantees `td.colSpan` clamps and `col.span` does not, from the same paragraph of the same
specification, with nothing to compare them against. So:

- `src/bindings/Reflection.h` is the vocabulary (the twelve kinds and their per-attribute
  parameters), `ReflectionTable.cpp` is ~430 rows of data, and `ReflectedAttributes.cpp` is the
  algorithms, once each.
- The parsers are transcriptions of HTML's "rules for parsing integers", "…non-negative integers"
  and "…floating-point number values". They are **not** `util::ParseInt`: that one rejects trailing
  garbage and these must stop at it, and `"1. 1"` is the number 1 to HTML and an error to everyone
  else. A vertical tab is whitespace to C and is not to HTML — that difference alone is tested
  thirty-odd times per numeric attribute.

**A table of a few hundred rows needs a checker, and it got one that is not the suite.**
`tools/`-free: a throwaway script read WPT's own `elements-*.js` and the browser's tag→interface
table and compared every (interface, property, attribute, kind) against `ReflectionTable.cpp`. The
suite proves the tested rows behave; that comparison proves the *untested* rows spell their content
attribute right — and a mistyped content-attribute name reads as an absent property, which is
silent.

### Three things this uncovered that the reflection tests do not test

**`<canvas>`'s `width` and `height` were a resize that only one spelling reached.**
`InstallCanvas` owned an accessor pair that wrote the attribute and resized the surface, so
`canvas.width = 50` resized and `canvas.setAttribute('width', '50')` did not. It also parsed with
`ToNumber` rather than HTML's rules, so `width="50px"` was a **zero-width canvas** where every
other browser draws 50. The accessors are gone: `width`/`height` are ordinary reflected
`unsigned long`s with the defaults 300 and 150, and the resize hangs off the attribute *write*,
which is the one place every spelling converges. `html/canvas/element/canvas-host/` 24.2% → 50.0%,
0 regressions.

**`<a href>` did not resolve, so `a.pathname` was not a path.** A reflected URL answers with the
attribute resolved against the document's address — that is what a page follows — while the setter
still stores what was written. The hyperlink parts (`protocol`, `host`, `pathname`, …) now split
the *resolved* href rather than the attribute, so `a.href = 'c.png'` on a page at `/a/b` has the
pathname `/a/c.png` instead of `c.png`. `<area>` got the same set; it had none.

**`nonce` reflects in one direction, and that is a security property rather than a quirk.** The
content attribute feeds an internal slot and the IDL setter does not feed it back, because a
stylesheet may say `script[nonce^=a] { background: url(…) }` — an attribute a page can read back
through the DOM is a nonce an injected stylesheet exfiltrates one character at a time. The suite
asserts the attribute does not move.

### The remaining gap in `html/dom/`, ranked, because "95.9%" is not a plan

Two thirds of what is left is one file, and it is a file this browser should not pass:

| subtests | file | what it needs |
|---:|---|---|
| 1,160 | `aria-attribute-reflection-enumerated.tentative.html` | **nothing — a refusal** |
| 505 | `the-innertext-and-outertext-properties/*` | `innerText`, which is a layout-dependent serialisation |
| 181 | `partial-updates/tentative/*` | a proposal no browser ships |
| 26 | `aria-element-reflection.html` | element *references* + named access on `window` |
| ~90 | `dom-tree-accessors/{title,body,getElementsByName,nameditem}*` | four separate accessors |
| 26 | `global-attributes/dir-*` | `dir=auto` directionality, which is a bidi question |

**The 1,160 are a deliberate refusal, and the refusal was *measured* rather than argued.** That
file is w3c/aria PR 2484 — a proposal to convert twenty `aria-*` attributes from `DOMString?` to
enumerated — and it contradicts the shipped specification rather than extending it: with
`aria-busy` absent the proposal wants `"false"` and `aria-attribute-reflection.html` wants `null`.
Reading two specifications and concluding "these disagree" is not evidence, so the proposal was
implemented, both files run, and it was reverted:

| implementation | `aria-attribute-reflection.html` | `…-enumerated.tentative.html` |
|---|---:|---:|
| `DOMString?` — shipped, and what landed | **41 / 41 (100%)** | 562 / 1722 (32.6%) |
| enumerated — PR 2484 | 28 / 41 (68.3%) | **1696 / 1722 (98.5%)** |

**The proposal is worth +1,134 subtests there and −13 here, and it is still refused.** That number
is the reason to write the refusal down rather than to reverse it: those 13 are the shipped
behaviour of every browser — `el.ariaBusy` on an element with no `aria-busy` is `null` in all of
them — so taking the trade buys 1,134 points by giving a page an answer no other engine gives.
AGENTS.md puts correctness first and a pass rate nowhere. The stable behaviour is what landed:
every `aria-*` and `role` is a nullable string on `Element`, which took
`aria-attribute-reflection.html` and its `.tentative` sibling from 3 of 44 to **44 of 44** and is
worth 282 subtests across the area.

**This is the shape to expect from the rest of the gap, and it is why "`html/dom/` at 100%" is not
a goal anyone should adopt.** Of the 2,311 subtests left, 1,341 are two tentative proposals and 505
are `innerText`; a browser that passed all of them would be one that had implemented a
contradiction and a layout-dependent serialisation, in that order.

**`html/dom/render-blocking/` is flaky by construction and it is not this change.** Two *identical*
runs of the same binary against the same expectations reported **0 unexpected and then 2**. Those
tests measure "did rendering block" with timers, and this browser paints a page when it is finished
rather than as it arrives (ADR 0030), so they race the runner. Re-recording the area does not
settle it. The residual unexpected results in any `html/dom/` run are confined to that directory
and to `partial-updates/tentative/`; nothing else in the area moves between runs.

### What was checked, and what was not

`ctest` **2,099 tests, 0 failed** (two new: one for the twelve algorithms, one for URL
resolution). ASan: 2,099 tests, no memory errors — the one LeakSanitizer report is
`ConstructableStylesheets.cpp` and is **byte-for-byte identical with these changes stashed**
(368 bytes in 12 allocations, before and after), so it is pre-existing and now TD-worthy on its own.
`html/dom/` re-measured with **0 subtests going PASS → FAIL**.

**What was compared against the pre-change binary**, which is the only way to tell a regression from
an expectation file that was already stale: `dom/`, `custom-elements/`, `shadow-dom/`,
`domparsing/`, `content-security-policy/` and `html/canvas/element/canvas-host/`. The only
difference anywhere is **45 new subtests** in `shadow-dom/reference-target/tentative/` that exist
only because `label.htmlFor` exists now (813 subtests in that area became 858), and all 45 die on
the test's own cleanup line, `host_container.setHTMLUnsafe("")`, which this browser does not
implement. The area scores **0.0% in both directions**, so those are not reference-target failures
yet; they are recorded with a `#` naming the cause. Every one of the 90 PASS → FAIL lines a full
sweep reports sits in those three files and nowhere else. Four `dom/` timeouts and two in `html/dom/render-blocking/` are load flakes — each passes
run alone, and the pre-change binary times out on the same two.

**Two areas were deliberately not re-measured, and both are the next agent's inheritance.**
`html/browsers/` is 751 tests of navigation this browser cannot do, almost all of them 20-second
timeouts: four hours to learn nothing about reflection. `html/semantics/` is 2,597 tests at roughly
eight a minute — a six-hour run that was started, watched to **367 tests with 3 subtests going
PASS → FAIL** (all three in one `.tentative` file, all three `action_sequence() is not implemented
by testdriver-vendor.js`, i.e. B5), and then stopped. **Its expectation file is now stale in the
optimistic direction**: reflected attributes are read all over `html/semantics`, so its recorded
failures overstate what fails today. Re-measuring it is one machine-hours task with no thinking in
it.

`docs/wpt-baseline.md`'s `html/dom` row was merged **by hand**, for the reason the note at the top
of that file already gives — `--summary` refuses to write a document describing fewer areas than it
already has, and the state file it wants lives in `/tmp`. That is task B6 and this is the fourth
session to pay for it.

### Known gaps, written down rather than half-built

- **`img.width` / `img.height` / `video.width` return the attribute, not the rendered size.** HTML
  says the rendered width when the image is being rendered, the density-corrected natural width
  when it is not, and 0 otherwise. The reflection suite marks these `customGetter` and does not
  test the getter at all, which is why they read as passing. The setter is correct.
- **A `nonce` set from script on a *detached* element is invisible to CSP.** The spec has CSP read
  the internal slot; `src/csp` reads the content attribute, and `src/csp` may not see `src/js`. The
  failure is closed rather than open — a style this browser cannot prove was nonced is blocked —
  and it is unreachable from the paths that exist today, because scripts are collected once at
  parse.
- **`Document`'s five colour reflections and `document.dir` resolve against the binding layer's
  one document URL**, like every other URL answer here.
- **ARIA *element* reflection is absent** (`ariaActiveDescendantElement`,
  `ariaLabelledByElements`): 26 subtests, and it is a different mechanism -- an IDL attribute
  holding an explicitly-set *element* that survives the target's id changing, plus a FrozenArray
  form. Those tests also want named access on `window` (`ReferenceError: input1 is not defined`),
  which is its own feature and is what half of `dom-tree-accessors` measures too.
- **`usvstring-reflection.https.html` is not a reflection problem.** Its 19 failures are unpaired
  surrogates surviving where the Web IDL USVString conversion should replace them with U+FFFD, and
  they are spread across `location`, `window.open`, `EventSource` and `sendBeacon` -- a string-layer
  question, not a table one.

## 2026-08-12 — `document.title` and `document.body` were lookups, and both are algorithms

**Status:** `html/dom/` **95.9% → 96.0%**; `html/dom/documents/` **203 recorded failures → 141**.
`document.title-0{3,5,7,9}.html` and `Document.body.html` are **72 of 72**, from 5.

Ranking `html/dom/` after the reflection work put ARIA first (done) and this cluster second. It is
not reflection, and it is included here because the ranking is what found it and because both
accessors were **silently wrong in the same way**: written as one-line lookups beside
`documentElement`, the getters answered with the first matching tag *anywhere in the tree* and the
setters did not exist at all. `document.title = 'x'` succeeded, read back the old title, and left
the tab named whatever the markup said.

**What the specification actually says, and what a descendant search gets wrong.** The body element
is "the first of the html element's children that is either a body or a frameset element". Three
clauses, and a search for the first `<body>` breaks all three:

- **A child of the html element**, not a descendant of the document. A `<body>` inside something
  else is not the body, and a `<body>` that *is* the document element has no html element above it,
  so that document has no body at all.
- **A frameset counts.** A frameset document's body element is its `<frameset>` — which is where
  `onload` is set on either kind of page.
- **In the HTML namespace.** `createElementNS('urn:x', 'body')` appended to `<html>` is not a body,
  and answering with it hands a page an element with none of the members it is about to use.

`dom::Document::Head` had the same three-clause shape and the same bug, and it matters for the same
reason: `document.title = x` appends into the head, so a head found anywhere in the tree is a title
put where no parser would have placed one.

**Two failures where there was one, and Web IDL decides which.** `document.body = 'a string'` is a
**TypeError** — the setter's type is `HTMLElement?` and a string fails the argument conversion
before any algorithm runs — while `document.body = someDiv` is a `HierarchyRequestError`. Answering
one for both reports a tree problem for what is a type problem.

**`document.title` was missing "strip and collapse ASCII whitespace" entirely**, which is what makes
`<title>\n  Hello\n  world\n</title>` the tab name `Hello world` rather than the five lines the
author indented. It also reads *child* text content rather than `textContent`: `<title>a<b>c</b></title>`
is `a`, because a title is not supposed to contain elements and a browser reading the whole subtree
names the tab after markup nobody meant as a title. And an SVG document's title is an SVG `<title>`
*child of the root* — one nested deeper titles a shape.

### The instrument bit somebody again, and the tell was the denominator

A `--update-expectations` run of `html/dom/` on a loaded machine took **169s where the same run
takes 88s**, three `reflection-*.html` files exceeded their deadline, and the writer recorded
`harness=TIMEOUT` for each. That would have **silently deleted 24,000 subtests** from the
measurement and left the area looking three points worse forever, with an expectation diff of
eleven added lines that reads as ordinary churn.

**It was caught by the subtest count falling from 60,138 to 32,743, not by anything in the diff.**
The rule that follows: re-record an area at low concurrency, and check the denominator before
committing. This is the same class as the two "the measurement was fake before the browser was"
findings from 2026-08-11 — the third in two days, and the first where the tooling would have
written the wrong number down rather than merely reported it.

**Checked:** `ctest` 2,099 tests 0 failed. ASan 2,099 tests, no memory errors. `html/dom/`
re-verified at `--jobs 4`: 57,759 of 60,138, **0 subtests going PASS → FAIL**, and the four
unexpected results are three `render-blocking/` flakes and `idlharness`.

## 2026-08-12 — `dom/`, one tractable block at a time, and what the remaining gap actually is

**Status:** five commits in a worktree, every one verified with **0 unexpected results** before it
was recorded. Per-area, measured in isolation on a quiet machine:

| area / file | before | after |
|---|---|---|
| `dom/abort` | 10 of 37 | **35 of 37** |
| `dom/traversal` | 1572 of 1608 | **1584 of 1608** |
| `dom/events` | 312 of 677 | **342 of 677** |
| `dom/ranges/StaticRange-constructor.html` | 0 of 17 | **16 of 17** |
| `dom/nodes/moveBefore/Node-moveBefore.html` | 2 of 32 | **29 of 32** |
| `dom/nodes/Node-properties.html` | 37 failures | **0** |
| `dom/nodes/Node-textContent.html` | 30 failures | **10** |
| `dom/traversal/NodeIterator-removal.html` | 21 failures | **2** |

### The three bugs that were not the feature they were filed under

**A listener removed mid-dispatch still ran.** The dispatch loop walks a *copy* of the listener
list, which is right — the set that runs is the set that existed when the event reached the node —
and it is only half the rule. The DOM also gives each listener a "removed" flag, and one taken off
the list does not run even though the copy still holds it. Without it, `controller.abort()` from
inside a handler still called every later listener the same controller was meant to cancel, so
`AddEventListenerOptions-signal.any.html` failed in a way that looked like the signal option was
missing rather than like dispatch was.

**A duplicate listener was added twice.** The DOM's identity for one is (type, callback, capture)
and re-registering is a no-op. Here it took as many `removeEventListener` calls as registrations —
which on a real page reads as a handler firing twice, and is a leak a re-rendering component drives
by itself.

**`acceptNode` was read with `Object::Get`, which answers nullptr for an accessor.** A filter
written as `{ get acceptNode() { … } }` therefore read as *no filter at all* and the walk accepted
everything. That is the shape worth remembering: the wrong read did not fail, it succeeded with a
different answer. Fixing it needed the accessor-aware read *with* the abrupt completion, which is
now `Interpreter::GetPropertyOrThrow` — the three-argument `GetProperty` is private on purpose,
because inside an evaluation propagating is not optional, and this is the door for callers outside
one.

### `composedPath()` was installed by the dispatcher, so it existed only sometimes

It was put on the *event object* by the node dispatch path. So it was "not a function" before
dispatch, on `window`, on an `AbortSignal`, and on any `EventTarget` a page constructed — and it
kept answering after dispatch was over, when the specification says the path is empty. It is on
`Event.prototype` now, over a slot dispatch sets and clears, which is also what makes
`currentTarget` null once dispatch ends.

### `AbortSignal.any` flattens, and the flattening is observable

The three statics were absent for the right reason — the note said each needs a decision (a timer,
a composed signal) that ADR 0012 says not to fake. Both decisions are made now. The timer is
`TimerQueue::QueueDelayedTask`, a deadline the *browser* chose: deliberately not a call to the
page's `setTimeout`, which a page may replace and whose ids a page can guess.

The composition is the part that is easy to get wrong. `any` does not chain — a signal composed
from a composed signal links to the **original sources**, and a source marks every dependent
aborted *before* any of their handlers run. Chaining instead fires them depth-first, which is a
different order for the same tree, and `abort-signal-any.any.html` asserts the order explicitly
(`"01234"`).

### `moveBefore.length` is 2, and that is load-bearing

WPT's shared pre-insertion helper branches on it: `parent[method].length > 1` decides whether to
pass the reference at all, because passing null blindly would move nodes before the validation it
is testing. With a length of 0 all nine of those cases took the one-argument path and reported a
TypeError where the test wanted a HierarchyRequestError — nine failures that say nothing about
`moveBefore` and everything about a property nobody thinks of as behaviour.

The other trap in that method is its tree check. The specification says **the same shadow-including
root**, not "both connected", and the difference runs both ways: two disconnected nodes under one
detached root move fine, and a connected node and a disconnected one never share a root. Being in
the document is a consequence of the rule rather than the rule.

### What is left, and why the goal was "all of `dom/`"

The session was asked for every `dom/` subtest passing. It is not reachable from here, and the
reasons are worth having in one place because three of the four are decisions rather than gaps:

- **~570 subtests need `<iframe>`** — `Document-createElementNS.html` alone is 389 of them, all in
  its "XML document" and "XHTML document" variants. ADR 0027 is accepted and unimplemented; the
  previous session priced the same wall at ~4,700 subtests across `dom/ranges`. This is the single
  largest item in `dom/` and it is a browsing-context tree, not a DOM fix.
- **251 subtests are `Observable`**, a tentative proposal no engine ships, now refused in one block
  naming ADR 0012.
- **126 are attributes on a *processing instruction*** — WICG/declarative-partial-updates, which
  the test file's own `link rel=help` points at. The DOM gives a ProcessingInstruction no
  `getAttribute` at all. Also refused, with the reason written down.
- **~130 are `OpaqueRange`**, tentative, refused before this session.

So roughly 1,080 of the remaining failures are one unbuilt feature and three deliberate refusals.
What is left after them is real work and it is mostly two things: **`Attr` as a `Node`** (~86
subtests across `Document-createAttribute`, `attributes.html` and `Range-attribute-nodes`), which
the code comment in ElementQueries.cpp already describes as a design rather than an addition; and
**event handler content attributes** — `div.setAttribute("onclick", …)` compiles nothing here, which
is 47 subtests in `Body-FrameSet-Event-Handlers.html`, the tail of `remove-unscopable.html`, and
part of the 121 in `Event-dispatch-single-activation-behavior.html`.

### The measurement is the least trustworthy thing in this session, and it is the machine

Three separate full-`dom/` runs disagreed with each other by **15,000 subtests** — 43,207 then
27,494 then 40,399 — with no code change between two of them. Every difference was a handful of
large tests crossing their timeout: `Range-comparePoint.html` alone is 5,580 subtests and takes
between 19 and 59 seconds *on the same binary*, depending on what else is on the machine. Under
`load average: 54` on twelve cores, with other agents running their own WPT sweeps, a `TIMEOUT` is
a fact about the room.

**So the per-area numbers above were each taken in isolation, and the full-area sweep was
abandoned rather than recorded.** A run whose expectations would have written `harness=TIMEOUT`
against `Range-comparePoint.html` is a run that files a machine's bad afternoon as a browser
regression, and the next session pays for it. `--timeout-multiplier` is the mitigation;
`docs/wpt-baseline.md` is **not** regenerated here for the same reason, and because `--summary`
rewrites it from a `--summary-state` file that lives in `/tmp` and described one area (task B6
already names this).

### The next block is `on*` content attributes, and it was measured rather than started

`<div onclick="…">` compiles nothing here, and neither does
`setAttribute("onclick", …)`. `RunListenersOn` reads the wrapper's `on<type>`
**property**, so `el.onclick = fn` has always worked and markup never has. That is
worth ~200 subtests and the implementation shape is settled — HTML calls an unset one an
*internal raw uncompiled handler*, so it compiles **lazily** at the point that read already
happens, with no parser hook and nothing to pay for an element that has no such attribute.

It was not started because it has a security half that a session in a hurry would miss.
**An inline event handler is exactly what CSP `'unsafe-inline'` governs**, and `src/bindings`
cannot see `src/csp` — that `allow:` line is a security boundary (ADR 0008), and
`DocumentPolicy::AllowsInline` lives in `src/engine`. So the gate has to be a flag the engine
sets on this layer, the same inversion `GeometrySource` and `NetworkSource` use. The good news
is that it is *one boolean per document*: nonces and hashes do not apply to handlers, only
`'unsafe-inline'` does (`script-src-attr` falling back to `script-src`). Landing the compilation
without that flag would open a script-execution path CSP cannot see, and the browser would still
pass more tests — which is the shape of the mistake worth naming. It is task **C11** now, in
`docs/wpt-plan.md` and the ledger.

### C11 landed after all, and the gate is the interesting half

`<div onclick="…">` compiles now, lazily, at the point `RunListenersOn` already
asks for the `on<type>` property and finds nothing — HTML's *internal raw
uncompiled handler*, so there is no parser hook and an element without the
attribute pays a lookup it was already paying. `remove-unscopable.html` went
0 → 6 of 6 with it, which is the whole test finally reaching the behaviour it
was written for.

**The CSP question turned out to be its own function rather than a call to the
existing one, and the difference is a real case.** `Policy::AllowsInline`
implements the rule that a nonce or a hash *cancels* `'unsafe-inline'` — which
is the whole mechanism by which a modern policy stays safe on a browser that
understands nonces and usable on one that does not. That rule is about
`<script>` **elements**, which can carry a nonce. An attribute cannot: it has
nowhere to put one and CSP never hashes it. So a policy of
`script-src 'unsafe-inline' 'nonce-abc'` must still permit a handler while
refusing an un-nonced inline `<script>`, and calling the general form would
have refused both. `AllowsInlineHandler` says what it means, and the unit test
asserts exactly that divergence.

The answer crosses into `src/bindings` as a **flag**, because that module may
not see `src/csp` — its `allow:` line is a security boundary (ADR 0008). It
defaults to *deny*: a path from markup to running code that is on until
somebody remembers to turn it off is the wrong default for the one gate between
the two.

**And the compile needed a bound, which is the part that would have been easy
to miss.** `Interpreter::Run` calls `BeginHostTurn`, which resets the step
budget, and retains the parsed program for the life of the page — both correct
for a `<script>`, of which a document has a handful. A handler compiled from a
*dispatch* is neither: `for(;;){ el.setAttribute('onclick', i++); el.dispatchEvent(e) }`
would compile a new text every iteration, refresh the budget the loop is being
metered against, and add an AST. That is a hang a page can drive — the exact
thing `RunCompiled`'s own comment says it fixed once already for microtasks. It
is capped at 10,000 compiles per document, refused rather than truncated past
it, and the cache is still written so a page past the bound pays one lookup per
dispatch.

505 tests across `dom/events/` and `dom/nodes/` re-run afterwards: **two
improvements and no regressions.** The two `harness: expected OK, got TIMEOUT`
in that run both finish in 0.7 seconds when run alone — the sweep took 29
minutes under `load average` in the forties, which is the same measurement
caveat as everywhere else in this entry.

What is still missing from C11 is the *IDL* half: HTML makes `el.onclick` and
the content attribute one slot, so reading `el.onclick` on an element that has
only the markup should compile it and hand it back. Here it still answers
undefined — the attribute is reachable only from dispatch. That is the half
that wants the reflected-attribute table, which is task C10.

**Merge note (2026-08-12):** C10 landed in the same merge as this entry, so the reflected-attribute
table now exists and the IDL half of C11 is unblocked -- nobody has written it, and `el.onclick`
still answers undefined on an element that carries only the markup.

## 2026-08-12 — the legacy multi-byte encoders, and the two bugs that were hiding them

**Status:** `encoding/legacy-mb-*` — every file that does not need an `<iframe>` now passes.
21 of 105 files, **31,932 subtests** that were failing, plus the 8 files whose harness never
reported at all. The other 84 files are blocked on ADR 0027 and nothing in this session moves them;
the arithmetic is at the end.

**Check:** `microbrowser_wpt encoding/legacy-mb-` — 0 subtest failures anywhere, and
`microbrowser_tests` 2100/2100 with three new cases in `tests/EncodingTests.cpp`.

### The feature: an encoder is not a decoder read backwards

`src/html` had five legacy decoders and no encoder at all, so `<a href="?q=日本">` on a Shift_JIS
page sent UTF-8, and a form on one sent UTF-8 too. Both are wrong in the way that matters: the
server on the other end reads those bytes back in the encoding *it* served, so the bytes decide
what the user searched for.

**Every one of the standard's `index pointer for code point` operations has its own exclusion and
its own tie-break, and each exists because the index maps two pointers to one code point.**
Shift_JIS drops pointers 8272-8835 *before* taking the first match; Big5 drops the Hong Kong
supplement and takes the **last** match for six code points; GB18030 carries a side table of
eighteen private-use code points that do not encode where its own index says. Inverting a decode
table without those rules produces bytes that decode back to the right character and are still not
the bytes any other browser sends — a wrong answer no round-trip test can see. The rules live in
`tools/unicode/generate_encodings.py`, where the tables are built, and the tables are generated for
that reason.

ISO-2022-JP arrived with them, decoder and encoder, and it is the first stateful encoding here: an
escape sequence decides whether `3C` is a `<`. That is a security property before it is a rendering
one — a sanitiser that scanned the bytes scanned the wrong thing — so it is implemented with the
standard's pushback rather than approximated. `Gbk` is now an encoding rather than a label for
`Gb18030`: they share a decoder and **not** an encoder, and a page labelled `gbk` whose form sent
four-byte sequences would be sending bytes its own server has no decoder for.

Also landed because the same tests need them: EUC-JP's JIS X 0212 index (decode only — no encoder
in the standard produces a `0x8F` sequence) and GB18030's four-byte form, which is what makes it
the one legacy encoding that can say anything Unicode can.

### Two bugs that had nothing to do with encoding, and one that was in the harness

**The WPT server was sending `charset=utf-8` on every document.** One header, and it silently
disabled the whole of the Encoding Standard for the whole of the suite: a `charset` in
`Content-Type` outranks a `<meta charset>` in the bytes, so all 105 files under `legacy-mb-*` were
decoded as UTF-8 and tested nothing they meant to. wptserve sends `text/html` with no charset and
lets a `.headers` sidecar ask for one; 25 files under `encoding/` do exactly that.
`gbk-encoder.html` carries the comment *"if the server overrides this, it is stupid"*, which is
this bug written down by an author who had met it before. **A harness that answers the wrong
question is worse than a missing test**, and this is the second time in three sessions that the
first thing a new area found was in `tools/wpt/`.

**The JavaScript collector was quadratic in the size of the live set.** `kCollectionThreshold` was
a flat 4096 allocations, and a mark-sweep pass costs what is *live* — so a program that grows to N
live cells and keeps allocating pays O(N) every 4096 allocations. Measured on
`eucjp-encode-href-errors-han.html`, which builds 21,269 testharness subtests and holds every one
of them: batches of 2,000 took 1.8s, 3.2s, 4.1s, 4.6s — a straight line in the size of the heap —
and the page never finished. The threshold is now `max(4096, live/2)`, the conventional
grow-by-half rule, and that file finishes in **19 seconds**. It is the whole reason the eight
`-errors-han` / `-errors-hangul` / `-encode-href` files stopped timing out; no encoding change
would have touched them.

Nothing could see it, which is the part worth keeping. `js.heap_live_peak` is one number at one
moment and cannot tell a collector that runs twice from one that runs ten thousand times over the
same heap. **`js.collections` and `js.cells_traced` are the replacement**, and they are a pair on
purpose: `cells_traced / collections` is the average live set a pass costs, and `cells_traced`
against the clock is what says whether a slow script is running or collecting.

**`"💩"` was two characters.** The lexer decoded each `\uXXXX` into its own three-byte
sequence and never paired them, so an astral character spelled with two escapes — which is how
source code spells one — had `codePointAt(0)` answer 55357 and came out as two replacement
characters wherever it was later encoded. Found by `gbk-encoder.html`, which spells U+1F4A9 that
way; the language has no way to say those two strings are different.

### What is left, and why no amount of encoding work reaches it

84 of the 105 files are `<iframe>`-driven, and they split cleanly:

- **38 decode files** load `<iframe src="…_chars.html">` and read `iframe.contentDocument`.
- **46 encode-form files** create iframes, submit a form into one with `target`, wait for
  `iframe.onload`, and read `iframe.contentWindow.location.search`.

Neither needs script *inside* the frame and neither needs the frame painted, but both need a second
document the parent can reach — **ADR 0027, which the 2026-08-11 entry above already named as the
blocker for 90% of `dom/` and as "a large one"**. The same seven-line summary applies here: this is
now the second area whose remaining gap is one feature, and the two gaps are the same feature.

The encoder half is complete and independent of it. `EncodeWithNumericEscapes` and
`html::DocumentQueryEncoder` are already wired into both places that will need them — a link's
query (`Engine::ResolveDocumentUrl`) and a form's data set (`BuildFormSubmission`, including
`accept-charset`) — so when frames land the form tests should pass without further encoding work.

**Two things a next session should know.** `url::QueryEncoder` is an interface `src/url` *declares*
and `src/html` implements, joined in `src/engine`: `src/url` may see only `util`, which is what
keeps the bottom of the web stack at the bottom, and a dependency on the encoding tables would put
a character-set question underneath every origin check. And the document's encoding lives on
`DocumentPolicy` beside the base URL rather than on `Page`, because HTML's "encoding-parse a URL"
takes both and nothing else — holding them apart is how a `<base href>` and a `<meta charset>` end
up describing different documents.

## 2026-08-12 — ADR 0027, first increment: a browsing context is a tree

**Status:** in_progress. The context tree, frame loading, `contentDocument` and `frame-src` are in
and green (`microbrowser_tests` 2100/2100, `encoding/legacy-mb-` 0 unexpected). Nested layout,
nested display lists, hit-test descent and the cross-origin half are not. **No test moved**, and
the reason is in "what is next" below — it is not this commit's shape, it is a missing feature two
layers away.

### What landed

`engine::Page` owns `Frame`s and a `Frame` owns a `Page`. That shape was chosen because `Page`'s
own header already called it "the unit that a second tab duplicates" — the child needs a document,
a style resolver, a box tree, a script interpreter and a loader state, which is exactly what `Page`
is. Nothing in it holds a pointer into its parent, which is ADR 0027 §5's first constraint.

**The origin check is the absence of a pointer, and that is the part to preserve.**
`dom::Element` gained a borrowed `Document*` that only `src/engine` writes and only for a
same-origin child. `src/bindings` cannot compare origins — it may not see `src/url` — and now it
does not have to: a cross-origin frame has *nothing there to return*. Every alternative shape
(a flag on the binding, a check in `contentDocument`) is one forgotten test away from a universal
cross-origin read, and this one cannot be.

### What is next, in the order it blocks things

1. **Event handler content attributes.** `<body onload="showNodes()">` never fires. This engine
   implements `el.onload = fn` as a JavaScript property and has **no path at all from an `on…`
   *content attribute* to a function** — `<div onclick="…">` is inert on every page this browser
   has ever rendered, and nothing had noticed because the target sites all use
   `addEventListener`. All 38 `encoding/legacy-mb-*` decode files are started from
   `<body onload>`, so they cannot pass until it exists.

   It is not a small change and the reason is worth knowing before starting: an event handler
   content attribute is *compiling source at runtime*, and `src/js` deliberately has no `eval` and
   no `Function(source)` — there is a test that says so. What is needed is a C++-only
   `Interpreter` entry point for compiling a function body, reachable from `src/bindings` and
   nowhere else, gated on CSP's `AllowsInline(Script, …)` exactly as an inline `<script>` is.
   That gate is not optional: `<img onerror=…>` is the most common XSS payload on the web, and an
   engine that compiles one without asking the page's policy is a worse browser than one that
   compiles none.

2. **Nested layout and nested display lists** (ADR 0027 §6 step 1's other half). `layout_.box_by_element`
   already gives the iframe element's box, and `PushTransformCommand` + `PushClipCommand` already
   exist — so a child's list splices under a transform and a clip, which is the same thing a child
   in another process would deliver. Until this lands a frame loads and is readable and paints
   nothing, which is a hole ADR 0012 would call a stub if it were left standing.

3. **Hit-test descent**, then the cross-origin half of §2: `WindowProxy`, `postMessage`,
   `X-Frame-Options`, `frame-ancestors`.

4. **`contentWindow` is not the child's global**, and cannot be as things stand. Each context has
   its own `js::Interpreter` and therefore its own heap — which is what makes ADR 0027 §5's process
   split an extraction — and an object from one heap handed to another is a use-after-free waiting
   for the first collection. What is installed today answers `.document` and nothing else.
   **A same-origin page reaching a global its own frame's script set needs a realm concept in
   `src/js`: one interpreter per *site*, with one global per browsing context.** That is the
   correct end state (ADR 0004's "a process hosts one site" says same-site contexts share a
   process, so they may share a heap) and it is a change to the JavaScript engine rather than to
   the engine layer. It was not visible from the ADR and it is the largest single cost left in it.

### One thing found on the way that has nothing to do with frames

`privacy::ResourceType::Document` on a *subresource* request is refused by the blocking engine, and
silently: the request never reaches the network and nothing anywhere says why. A frame started with
it looked exactly like a frame whose URL did not parse. `Subdocument` is the right type and is what
every blocklist means by a frame load, but the silence is worth a look — it is the second
"refused with no way to see it" in this area in two sessions.

## 2026-08-14 — G5: a worker had no global scope, and it cost 1,763 test files

**Status:** done
**Check:** `dom/abort/event.any.worker.html` TIMEOUT/0 subtests -> OK/16 subtests.
`xhr/abort-after-receive.any.worker.html` TIMEOUT -> OK, every subtest passing. On a fixed
400-file sample of the suite's worker variants: **20 of 13,742 subtests passing -> 4,215 of
18,000**, with 352 of the 400 files changing result. `microbrowser_tests` 2117/2117.
**Landed:** `A worker had no global scope…`, `A worker's URL parser is the page's URL parser…`,
`A worker's fetch is the page's fetch with the two ends moved`.

### The finding, and it is not about workers

`engine::Workers` has owned a thread and a heap since session 38 and every test of it passed.
What a script standing in that heap could **see** was `postMessage`, `self`, `name` and
`onmessage`. No `importScripts`, no `addEventListener`, no `location`, no `setTimeout`, no
`DedicatedWorkerGlobalScope`.

That is not a partial feature. It is an unreachable one, and the measurement says so exactly:
**1,763 of web-platform-tests' 42,185 files are a `.any.worker.html` variant** — the same
assertions as the `.any.html` file beside them, in a global that did not exist here — and every
one was a twenty-second timeout. 1,726 of the suite's 7,981 recorded timeouts, in one cause.

The lesson generalises past this feature: **a subsystem with a complete implementation and no
surface is invisible to every test of the subsystem.** `Workers`' own tests exercised the thread,
the queues, the join and the structured clone, and all of them passed while nothing a page could
write would run.

### Four things had to be true before one worker test could report

Each was false, and three of them are not obvious from the specification.

1. **`self instanceof DedicatedWorkerGlobalScope`.** testharness.js decides what environment it is
   in with exactly that expression, falling back to `WorkerGlobalScope`. With neither, it concludes
   it is in a *shell* — which has no channel to report on at all. A worker that ran every assertion
   correctly still said nothing.
2. **`importScripts`, which is specified as synchronous.** The worker thread files a request, wakes
   the main loop through the pipe it already had, and blocks on a condition variable. That is what a
   worker is *for*: the one thread allowed to block on a resource is the one that is not drawing.
   The wait's predicate includes `stop` and `JoinAndClose` notifies it, so a worker blocked in
   `importScripts` when its document navigates is freed rather than joined forever.
3. **Timers.** The run loop waits until the earliest deadline rather than forever. Zero idle CPU is
   unchanged — a worker with nothing pending still blocks, one with a timer blocks *until* it.
4. **`addEventListener` unqualified.** testharness.js calls it both as `self.addEventListener` and
   as a bare global, so every name is declared in the global scope *and* set on the global object.

### Two bugs found on the way, and the second is the larger one

**Every message from every worker was delivered twice.** `DeliverWorkerMessage` called the page's
`onmessage` property and *then* `dispatchEvent` — and `RunListenersOn` reads `on<type>` off the
target as an implicit listener, which is the specification's rule for a handler attribute. The
comment in `EventBindings.cpp` explaining that rule was already there. Invisible on a page that
counts side effects; fatal to a harness that counts results.

**Both test tools drove the engine as `if (Advance() || HasRunnableWork()) continue;` with
`RunDueWork()` only on the else** — while `RunDueWork` is what drains a worker's outbox and
`HasRunnableWork` is true precisely when there is something in it. The loop span at full speed
until its deadline while the delivery that would have finished the page sat in a queue nothing was
draining. `Application::Turn` calls both every turn; these now do too. **The same shape had already
been found once in `tools/snapshot` for timers and the comment there says so** — this is the second
bug of that exact shape in that exact line, and it is worth treating the pattern as the defect:
a tool loop that is not `Application::Turn` will diverge from it again.

### What the API surface cost, which was almost nothing

`URL`, `URLSearchParams`, `TextEncoder`, `TextDecoder`, `atob`, `btoa`, `crypto`,
`structuredClone`, `Blob`, `fetch`, `Headers`, `Request`, `Response`, `FormData`,
`AbortController` and `XMLHttpRequest` in a worker are **the same implementation the page uses**.
Not one line of them was rewritten.

Two properties made that possible and both are worth keeping:

- **`DomBindings::document_` was already a pointer.** A `DomBindings` with a null document is a
  binding layer that *cannot* reach the tree — there is no document to walk from — so a worker
  thread provably never touches `src/dom`, whose namespace intern table is process-wide and would
  be a real data race. `EnsureInterfaces` stops after creating the table when there is none, so
  `Node`, `Element` and the ninety per-tag interfaces are absent in a worker, which is what the
  specification says.
- **`bindings::NetworkSource` was already an interface.** A worker's `fetch` is a second
  implementation of it whose other end is the worker's thread: same privacy verdict, same CORS
  check inside `net`, same pool keyed by the same partition. `StartScriptRequest` and
  `ScriptResponseFrom` are shared with the page's path, because everything in the first is a policy
  decision and a second copy is a second place to make one of them differently.

**`src/url` is safe to call from a second thread and `src/dom` is not**, and that difference is
the whole architecture of this change. `src/url` is a pure parser over generated const tables with
no lazy initialisation anywhere in it. Check that property before reaching for any other module
from a worker.

### What is left in a worker, in the order it blocks tests

1. **`WebSocket`** — 20 of the 400-file sample's remaining harness errors, and `websockets/` is
   532 tests. `bindings::SocketSource` is the seam and it is the same shape `NetworkSource` was.
2. **`IndexedDB` and `localStorage`** — both want a store keyed by an origin and reached from the
   main thread. Absent rather than stubbed.
3. **`OffscreenCanvas`** — 889 tests suite-wide (task F6) and most of the canvas worker variants
   now report a clean "not defined" rather than a timeout, which is what makes them countable.
4. **`SharedWorker`** stays refused: ADR 0022 §1 names it as the one thing the model exists to
   avoid.

### A warning about measuring this area

Worker results are **load-sensitive in a way the rest of the suite is not**: a worker is a real
second thread per test process, so `--jobs 12` on a twelve-core machine oversubscribes badly. The
same 400 files gave 4,215 passing subtests at `--jobs 12` on an idle machine and 40 on the same
binary while a build was running. Re-record at low concurrency, and check the *denominator* before
believing a number — the existing warning in `docs/wpt-baseline.md` about `html/dom/` applies here
with more force.

## 2026-08-14 — H1 and B5: the two causes that were in the *harness*, not the browser

**Status:** done
**Check:** H1 — `fetch/api/basic/` 237 of 462 subtests (51.3%), 3 timeouts; `cors/` +
`fetch/api/cors/` + `fetch/api/redirect/` 348 of 1,553; `xhr/` 296 of 1,597. B5 — on a 150-file
sample of the tests that load testdriver.js, 2,244 subtests counted with 247 passing and **zero**
`not implemented by testdriver-vendor.js` in the run; `uievents/click/click_events_on_input.html`
TIMEOUT → OK. `microbrowser_tests` 2123/2123.
**Landed:** `The .py handlers, transcribed`, `testdriver.js, over the input path this browser
already has`, `` `element.click()` was trusted ``.

### The shape both of these share

Neither was a browser bug. Both were **a capability the browser already had with nothing exposing
it to the test**, which is the same shape as the worker global earlier the same day — and that is
now three in one session. It is worth stating as a rule for whoever reads this next: *when an area
is at single-digit percent, check whether the harness can reach the feature at all before reading
the failures as a specification gap.*

### H1: the `.py` handlers, and why ADR 0040 §2 was right and still changed

§2 refused to implement them and its reason stands verbatim: "a handler is arbitrary Python;
approximating one makes a test pass for the wrong reason, which is worse than a failure." It also
named the condition — *"until somebody measures that those specific tests are what is blocking"* —
and the measurement is stark: **one run of `xhr/` asks for `xhr/resources/content.py` 117 times,
`status.py` 110 and `delay.py` 74.** Ten files account for most of it.

What keeps this on the right side of the objection is two structural properties, not an intention:

- **Each handler is a transcription of one specific file with that file's source quoted above it.**
  Same parameters, same defaults, same order of operations, so a reviewer can put the two side by
  side. The defaults are where a transcription silently diverges — `status.py` answers the reason
  phrase `OMG`, an absent request header is reported as the literal string `NO` — and each is
  pinned by a test.
- **Dispatch is on the repo-relative path**, because three different `redirect.py` files exist in
  the checkout with three different behaviours. A handler keyed by basename would apply one of them
  to all three, which is exactly "passes for the wrong reason".

**`?pipe=` was the larger half and nobody had counted it.** Hundreds of tests ask for a status or a
header on an *ordinary static file* with `?pipe=status(404)`. A server that ignored the query served
the file as itself — a **wrong** answer rather than a missing one, and therefore worse than the 501
§2 was protecting. That is the part of this that was never a trade.

**And the server did not read request bodies.** It answered before reading one, so the bytes stayed
in the buffer to be parsed as the next request line. Invisible while every request was a GET, and
most of what `fetch/` and `xhr/` do is not.

One thing could not be transcribed: upstream's slow handlers are `time.sleep` under a
thread-per-connection server. This one is single-threaded by ADR 0040 §4's own rule and six test
processes talk to it at once, so a sleep would stall the run and produce a cascade of timeouts that
reads exactly like a browser bug. A delayed response is **held** in its connection and written on a
later turn of the poll loop.

### B5: testdriver, and why it has to be the real input path

1,146 in-scope tests load `/resources/testdriver.js`. Upstream's `testdriver-vendor.js` is empty —
it is the hook `wptrunner` fills with WebDriver calls — so each got `not implemented by
testdriver-vendor.js`. B3 predicted the answer's shape exactly and it was right: a harness-only
global drained through `EvaluateScript`, the same seam the report already uses.

**The part worth not getting wrong:** ADR 0017 makes a page's own synthetic event untrusted by
construction, so a `test_driver.click()` implemented as `element.click()` would pass the half of
each test that counts handler calls and fail the half that checks what the click *did*. The runner
drives the real `ipc::PointerInputMessage` and `ipc::KeyInputMessage` instead — move, down, up,
because a page listening on `pointerdown` is ordinary.

**It found a real bug within minutes of existing**, which is the argument for the seam in
miniature: `element.click()` produced a **trusted** event, and a test in `tests/` asserted that it
did, on the reasoning that "untrusted would send youtube's handlers down a no-op path". That
reasoning was wrong twice — a page's handlers see the same event either way, and ADR 0017 §3 is
explicit that there must be no way for a page to make `isTrusted` true, because every gate that
reads it reads it as a statement about a person.

### What is next here, measured

- `xhr/`'s remaining 501s are 3–12 requests each rather than 117: the handler tail is a tail.
  `common/security-features/subresource/*.py` is the exception on paper (`referrer-policy/gen` is
  1,001 tests) but those need iframes, https and cross-origin contexts as well, so the handler is
  not what blocks them.
- **`websockets/` is blocked on the server, not the browser.** Its *window* variants time out too:
  this server speaks no WebSocket, so the 532 tests there are an H1-shaped problem rather than a
  worker one. Worth knowing before anyone implements `WebSocket` in a worker expecting a gain.
- **HTML's pre-click activation steps are not implemented**, and
  `html/semantics/forms/the-input-element/checkbox.html` measures it: a checkbox must be toggled
  *before* the click event is dispatched and restored if the event is cancelled, where this engine
  records the activation and applies it after. Four of that file's six subtests are that one
  difference.

## 2026-08-14 — `hsl()`, CSSOM's parse-before-store, and one wrong turn

**Status:** done
**Check:** `css/css-color/` **11.2% → 45.4%** (5,145 of 11,338 subtests).
`css/CSS2/syntax/colors-007.html` **0 → 1,192 of 1,192**.
`html/dom/aria-attribute-reflection.html` 41 of 41 (unchanged, after a revert).
`microbrowser_tests` 2132/2132.
**Landed:** `` `hsl()` did not exist… ``, `CSSOM parses before it stores…`, and a revert.

### `hsl()` computed to black

Not refused -- **black**. `ColorText.h`'s own comment says what that costs: "an invalid colour
makes a declaration invalid, and a declaration that silently became black would be worse than one
that was dropped." The function it describes was doing the second thing for an entire notation, and
`hsl()` is how a stylesheet writes a colour it means to vary.

The other half is the grammar split. CSS Color 4 gives `rgb()` and `hsl()` two forms that are
**not** interchangeable — legacy uses commas throughout, modern uses spaces with a slash before the
alpha — and this parser replaced every comma with a space and split on whitespace. That accepted
`rgb(1, 2, 3 / 0.5)`, which no browser takes, and rejected every modern form with an alpha, because
`/` came back as a fourth component. Telling them apart by *whether the body contains a comma* is
what makes the mixed form fall out rather than needing a rule of its own.

### The two systemic CSSOM bugs behind it, and both were one line

- **`'color' in getComputedStyle(el)` was false.** A `Proxy` with only a `get` trap answers `in`
  from its target, which is an empty object — and that expression is the *first assertion* of every
  `css/**/parsing/*-computed.html` in the suite. `color-computed-hsl.html` alone is 3,753 subtests,
  every one reporting that this browser does not support `color`. The failures behind it were real;
  they were unreachable.
- **`el.style.color = 'nonsense'` stored the word, and `'#000'` read back as `'#000'`.** CSSOM
  parses before it stores and serializes on the way out. `css::CanonicaliseDeclaration` has three
  answers and the third is the honest one: `Unknown` keeps the old behaviour for every property
  whose grammar `src/css` cannot check, because a property wrongly canonicalised silently changes
  what a page reads back where one left alone behaves as it always did. Only the `<color>`
  properties are on the list, **by explicit name** -- a suffix test on `-color` would have been
  wrong on its first member, since `border-color` is a shorthand of four.

A named colour serializes as its *name* (`'red'` reads back `"red"`) and only the numeric notations
collapse to `rgb()`. The computed value is `rgb(255, 0, 0)` either way, which is a different
question asked in a different place -- and that place now uses the same serializer in `gfx` rather
than its own copy of the spelling.

### The wrong turn, written down because the check that would have caught it is ten seconds

I implemented ARIA's enumerated reflection from `html/dom/elements-aria-enumerated.js` -- 21
attributes, ~1,200 subtests -- and reverted it an hour later. **That data table is included by
exactly one file and it is marked `.tentative`**, and the non-tentative
`aria-attribute-reflection.html` says the opposite: these reflect as nullable strings whose missing
value default is `null`, not as enumerations defaulting to `"false"`. The change traded 13 subtests
of the shipped test for ~1,200 of an unshipped proposal.

The check I skipped: **`grep -rl <data-file> third_party/wpt` and look for `tentative` in the
names of the files that include it.** Ten seconds, before the first line of code.

### What the measurement says to do next in `css/`, which is not what I expected

The largest remaining files there are all `*-interpolation.html`, and they fail on
`CSS.supports(property, from)` returning an **honest** false. `CSS.supports` works -- verified
against seventeen property/value pairs -- and `translate`, `box-shadow`, `shape-outside` and
`grid-template-rows` are genuinely not implemented. There is no systemic harness bug left behind
them: each is one property, and that is the long tail.

Also corrected in `CLAUDE.md`: `min()`/`max()`/`clamp()` and the viewport units are **done** and
have been for some time. That roadmap entry named them as the next thing for several sessions.

### Addendum — ThreadSanitizer, on the first thread that runs page code

`setarch -R ./build/microbrowser-tsan/microbrowser/microbrowser_tests`: **2132/2132, clean.** That
covers the four worker tests, which between them exercise the thread's whole life -- the script
running on it, both message queues, `importScripts` blocking on the main loop and being answered,
a `fetch` crossing the boundary in each direction, an uncaught throw becoming an `error` event, and
`terminate()` joining. ADR 0022 §1 calls this "the first thread in the browser that runs a page's
code" and "the model for every thread after it"; a clean TSan run over it is the evidence for that
claim rather than the design note being the evidence.

The colour parser got the other half: `color_text_fuzzer`, **22,911,161 runs in 181 seconds** under
AddressSanitizer and UndefinedBehaviorSanitizer, asserting the serialize/parse round trip as well
as memory safety.
