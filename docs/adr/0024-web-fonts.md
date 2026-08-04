# ADR 0024 — Web fonts, and the text a page brings with it

**Status:** accepted · **Date:** 2026-08-04

## Context

The font stack matches a `font-family` list against the system font database. There is no
`@font-face`, and no way for a page to supply a font at all.

youtube.com's first stylesheet is `fonts.googleapis.com/css2?family=Roboto:…&display=swap`, which
returns rules of this shape:

```css
@font-face {
  font-family: 'Roboto'; font-style: normal; font-weight: 300;
  font-stretch: 100%; font-display: swap;
  src: url(https://fonts.gstatic.com/s/roboto/v51/…woff2) format('woff2');
  unicode-range: U+0460-052F, U+1C80-1C8A, U+20B4, U+2DE0-2DFF, U+A640-A69F, U+FE2E-FE2F;
}
```

Sixteen of those for one family, split by script. Every one of the features in it is missing here,
and one of them — WOFF2 — needs brotli, which ADR 0010 explicitly deferred to an ADR of its own.

The Hacker News run already taught this project what font matching failing looks like: font stacks
were never split, so `font-family: Verdana, Geneva, sans-serif` matched nothing and **no page with a
stylesheet rendered any text at all**. Web fonts are the same mechanism one level up. A page that
sets `font-family: Roboto` and ships Roboto has no fallback in its stack, because from its point of
view the font is guaranteed. If `@font-face` is unimplemented, the family does not resolve and the
text falls back to whatever the user agent stylesheet says — which is usually readable, and is the
lucky case rather than the designed one.

There are two problems underneath the feature, and they pull in opposite directions.

**Security.** A font file is a program. OpenType has a bytecode interpreter in it, and font parsing
is second only to image decoding as a source of browser remote-code-execution bugs. `@font-face`
means downloading attacker-supplied fonts by default, on every page.

**Privacy.** A web font is a third-party request tied to the page the user is reading, and
`unicode-range` makes it worse in a way that is easy to miss: which subset files get fetched depends
on **which scripts appear in the text on the page**. A font server learns something about the
content, not just the visit.

## Decision

### 1. `@font-face` is implemented, with the descriptors that decide matching

`font-family`, `src` with its `format()` hints and comma-separated fallbacks, `font-weight` and
`font-style` and `font-stretch` ranges, `unicode-range`, and `font-display`.

The matching algorithm gains a step it does not have: **a page-supplied family is a candidate
alongside the system families, and it wins where it matches.** The descriptors are not decoration —
`unicode-range` decides which of sixteen Roboto faces is used for a given character, and getting it
wrong means downloading all sixteen and picking arbitrarily.

`font-display` is where the user-visible behaviour is decided, and the default matters:

- **`swap` is honoured**: the fallback face is used immediately and replaced when the web font
  arrives.
- **`block` is capped at a short timeout**, then behaves as `swap`. A page that asks the browser to
  show nothing until a font arrives is asking for a blank page on a slow network, and we decline —
  invisible text is worse than the wrong text.
- **`optional`** means the font is used if it is already available and otherwise skipped, and it is
  the honest behaviour for a browser without a font cache.

Replacing a face after layout **relayouts the affected text**, because metrics change. That is the
reflow the web calls FOUT, it is visible, and it is correct — the alternative is text that overlaps
its box.

### 2. WOFF2 is the format, and it brings brotli in

`format('woff2')` is what every font service serves. WOFF2 is a container: a brotli-compressed
stream plus a table transform that reorders and re-encodes the glyph outlines. So supporting it is
two pieces — the container, which is ours, and brotli, which is not.

**This ADR takes brotli as a sanctioned dependency**, under ADR 0001's rules, and the argument is
that it is now needed twice:

- WOFF2 cannot be decoded without it. There is no fallback format that every font service also
  serves; `format('woff')` responses are increasingly not offered at all.
- ADR 0010 deferred `Content-Encoding: br` as "an optimisation on a solved problem", which was
  correct when gzip was the alternative. Once brotli is linked for fonts, sending `br` in
  `Accept-Encoding` costs nothing and recovers the 15–20% ADR 0010 measured over gzip.

The dependency is the **decoder only** (`brotlidec`), not the encoder, which halves the attack
surface and most of the code. It is a decompressor over attacker-controlled bytes, so it inherits
every rule ADR 0010 wrote for gzip without amendment: **an absolute output ceiling, a maximum
expansion ratio, failure rather than truncation, and a fuzz target on the same commit.** A font is a
decompression bomb by default, exactly as a response body is.

The WOFF2 container itself — the table directory, the transforms, the glyph reconstruction — is
**ours**, parsed with the same discipline as the HTML tokenizer, with its own fuzz target. That is
ADR 0013's containers-are-ours split applied to fonts, and for the same reason: the container is what
decides what the underlying parser is asked to parse.

Bare `format('truetype')` and `format('opentype')` are also accepted, because they need no
decompression at all and some pages still ship them.

### 3. The font is parsed by FreeType, and FreeType is where the risk is

FreeType is already a sanctioned dependency and already parses every system font. Web fonts point it
at attacker-supplied bytes for the first time, and that is a genuine change in exposure even though
no new dependency is involved.

What that buys, and what it costs, stated rather than assumed:

- **Every downloaded font is validated as a container before FreeType sees it** — table directory
  bounds, table lengths against file length, no overlapping tables. Cheap, and it removes the
  malformed-container class entirely.
- **The bytecode interpreter is disabled for downloaded fonts.** FreeType's TrueType hinting VM is
  the most exploited part of it, and its benefit — grid-fitting at small sizes — is one this
  browser's analytic-AA rasterizer largely obtains anyway. System fonts keep whatever the build
  configures; downloaded fonts are unhinted.
- **When ADR 0004's process split lands, font parsing goes where image decoding goes.** It is the
  same argument, the same interface shape (bytes in, glyph outlines out), and it is written here so
  that the decoder process is designed with two clients rather than retrofitted for the second.

### 4. Fetching a font is a request like any other, and the privacy layer sees it

A web font request passes `privacy::Verdict`, is keyed by the ADR 0005 partition key, and is subject
to the page's `font-src` CSP directive (ADR 0020).

Two consequences fall out of the partition key that are worth naming, because both look like bugs and
are not:

- **The font cache is per partition key.** youtube.com and a page embedding youtube.com fetch Roboto
  separately. That is the same cost ADR 0005 accepted for every other cache, and it removes a
  well-known cross-site timing oracle: an unpartitioned font cache lets a page learn which sites the
  user has visited by timing how fast a font loads.
- **`unicode-range` fetches leak page content.** The set of subsets requested is a function of the
  characters on the page. The mitigation is not to disable `unicode-range` — that would fetch *all*
  sixteen subsets and leak nothing but waste megabytes — but to note that it is one more reason the
  request is subject to the blocking engine (ADR 0006) like any third-party request, and that a user
  who blocks third-party fonts gets a page that still renders in a system font.

**Local font access is refused.** `src: local(…)` resolves only against fonts the browser would offer
anyway, and `navigator.fonts` / the Local Font Access API is not implemented: the installed font
list is one of the highest-entropy fingerprinting signals in a browser, and ADR 0029 owns that
decision.

### 5. Order

1. **`@font-face` parsing, `src` resolution, and matching against page-supplied families** — with
   plain TrueType/OpenType only. Testable with a local file and no new dependency.
2. **`unicode-range` and `font-display`** — the descriptors that decide *which* file and *when*.
3. **brotli, then WOFF2** — the dependency and the container, together, with both fuzz targets.
4. **`Accept-Encoding: br`** — one line, once brotli is linked, closing out ADR 0010.
5. **`FontFace` and `document.fonts`** — the scriptable half. Low priority; no target site needs it.

## Consequences

- **The sanctioned dependency list grows by one**, and ADR 0001 should record brotli's decoder with
  the reason above.
- **`gfx::FontCatalog` stops being a view onto the system database** and becomes a matcher over two
  sources with different lifetimes. Page-supplied faces die with the document; system faces do not.
- **Text can reflow after first paint**, which is new. `font-display: swap` makes it a designed
  behaviour rather than a glitch, and the dirty-region path has to handle a relayout triggered by a
  network completion — which ADR 0011 already made possible and nothing has yet exercised.
- **A page can make the browser download and parse arbitrary font files.** That is the exposure this
  ADR adds, and the container validation plus disabled hinting plus the eventual process move are
  what pay for it. It is the second-highest-risk item on this roadmap after fragment parsing.
- **Pages will look right for the first time.** Every design system on the web ships its own fonts;
  until this lands, every such page renders in a substitute and the difference is obvious in a
  snapshot.

## Alternatives considered

**Refuse web fonts and always substitute a system font.** Genuinely defensible on privacy and
security grounds, and rejected on correctness. Icon fonts — still common — render as arbitrary glyphs
from a substitute face, so a page's controls become random letters. That is not a degraded page, it
is a broken one, and unlike a missing image it is not obvious *why* it is broken.

**Support WOFF (version 1, zlib) and not WOFF2.** Rejected on the measurement: font services serve
WOFF2 to anything that accepts it and increasingly do not offer WOFF at all. It would be a decoder
for a format we would rarely receive.

**Write a brotli decoder rather than take the dependency.** Rejected on ADR 0013's reasoning applied
to compression: brotli's static dictionary and context modelling are large, the format is not
security-simple, and a hand-written decompressor on attacker bytes is the same category of mistake as
a hand-written video decoder. `util::Inflate` was worth writing because DEFLATE is small; brotli is
not.

**Keep FreeType's bytecode interpreter on for downloaded fonts, for rendering quality.** Rejected.
The quality difference is small with analytic anti-aliasing and the exposure is a VM executing
attacker-supplied bytecode in the browser process. If it is ever re-enabled, it should be after the
process split, not before.
