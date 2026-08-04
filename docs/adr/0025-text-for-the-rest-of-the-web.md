# ADR 0025 — Text for the rest of the web: encodings, bidi, and the Unicode data question

**Status:** accepted · **Date:** 2026-08-04

## Context

This browser assumes the web is UTF-8 and left-to-right, and both assumptions are load-bearing in
code that never states them.

**Encoding.** `<meta charset>` is not read. `Content-Type: text/html; charset=…` is carried as a
string in `Loader` and never consulted. Bytes go to the tokenizer as if they were UTF-8. A page in
windows-1252 — still a meaningful share of the long tail, and the default for a great deal of
pre-2010 content — renders with mojibake wherever a byte above 0x7F appears. A page in Shift_JIS,
GB18030, EUC-KR or Big5 renders as noise.

**Direction.** There is no bidirectional algorithm. Arabic, Hebrew, Persian and Urdu text is stored
in logical order and would be painted in logical order, which is backwards. HarfBuzz is present and
will shape the glyphs correctly; nothing reorders the runs, so the shaping is applied to a sequence
that is then drawn in the wrong direction.

**Line breaking.** `src/layout` breaks lines at spaces. That is right for Latin scripts and wrong for
Chinese, Japanese and Thai, none of which use spaces between words. CJK text in a narrow column
becomes one long unbreakable line and overflows its box.

The survey does not measure any of this, and that is worth admitting up front: reddit, youtube and
Plex are all UTF-8, left-to-right, and Latin-scripted. **This ADR is not justified by the target
sites.** It is justified by the goal that came after them — being able to load and use any page —
and it is the largest thing standing between this browser and roughly a third of the world's text.

Underneath all three sits one question that has to be answered once: **where does the Unicode data
come from?** Encoding tables, bidi character classes, line-break classes, and — from the JavaScript
side, per `docs/js-conformance-roadmap.md` — the tables that `String.prototype.normalize` and the
rest of `\p{…}` need. Four subsystems, one dependency decision.

## Decision

### 1. The Unicode data is baked in, generated from the Unicode Character Database

**No ICU.** Not because ICU is bad — it is the reference implementation and it is correct — but
because it is enormous, it carries a full locale database and a formatting engine this project does
not want, and taking it would make it the largest dependency in the build by a wide margin for the
sake of four lookup tables.

Instead: a **generator checked into the repository** that reads the UCD data files and emits C++
tables into `src/util`. What is generated is exactly what is needed and nothing else —

| Table | Needed by |
|---|---|
| bidi class + mirroring pairs | UAX #9, this ADR |
| line-break class | UAX #14, this ADR |
| grapheme cluster break | cursor movement, ADR 0017 |
| general category, script | `\p{…}` in `src/js` |
| canonical/compatibility decomposition + combining class | `normalize`, `src/js` |
| case folding | `toLowerCase` and `String.prototype.localeCompare`'s crude path |

The generator's output is committed, so the build has no code-generation step and no network access,
and the UCD version is a single line in a header. Regenerating on a Unicode release is a diff a
reviewer can read.

**This is not a third-party dependency in ADR 0001's sense** — it is data, not code, and no
third-party code enters the build. Saying that explicitly matters, because the alternative reading
would require an ADR 0001 amendment for what is effectively a table of integers.

`Intl` stays where `docs/js-conformance-roadmap.md` puts it: out of scope, and honestly absent. Real
`Intl` is where ICU actually earns its size, and this browser does not need it to render a page.

### 2. Encoding: the specification's sniffing algorithm, and a decoder set chosen by usage

**The encoding is determined by the WHATWG Encoding Standard's algorithm, in order:**

1. a byte-order mark, which wins over everything
2. an explicit `charset` in `Content-Type`
3. a **prescan of the first 1024 bytes** for `<meta charset>` or `<meta http-equiv="Content-Type">`
4. the fallback, which is **windows-1252**, not UTF-8, because that is what the specification says
   and because a page with no declaration is overwhelmingly old

Then decoding runs, and the tokenizer's input becomes a stream of code points rather than a stream of
bytes. That is a real change to `src/html`'s entry point: today it takes `string_view` and assumes.

**The decoder set**, in landing order:

1. **UTF-8**, with correct handling of malformed sequences — every ill-formed sequence becomes U+FFFD
   by the specification's exact substitution rules, never a skipped byte and never a raw byte passed
   through.
2. **windows-1252** and **ISO-8859-x**, which are single-byte table lookups and cover the long tail
   of Western content.
3. **UTF-16LE/BE**, which the BOM rule makes reachable.
4. **Shift_JIS, EUC-JP, GB18030, Big5, EUC-KR**, which are multi-byte tables from the same generator.

**Encoding correctness is a security property, not only a rendering one**, and this is the reason it
is worth doing carefully rather than approximately. A decoder that emits a `<` where the specification
says U+FFFD turns a sanitised document into a script-executing one, and that class — encoding
confusion leading to XSS — is a real, repeatedly exploited bug family. The substitution rules are
therefore implemented literally, from the specification's tables, and fuzzed on the commit they land
like every other parser on the hostile path.

A page that declares an encoding it then contradicts — a `<meta charset>` after 1024 bytes, or a
`charset` that does not match the BOM — follows the specification's resolution rather than ours.
There is no reload-on-mismatch: the prescan exists precisely so that the decision is made once,
before parsing starts.

### 3. Bidi: UAX #9, at the layout boundary

The Unicode Bidirectional Algorithm, applied per paragraph, producing **visual runs from logical
text**, with the `direction` and `unicode-bidi` CSS properties feeding the paragraph level and the
explicit embedding controls.

Where it goes matters: **between line breaking and shaping**. A line box is built from logical text,
the algorithm reorders it into runs of uniform direction and level, and each run is handed to
HarfBuzz separately — because a run is the unit that shapes correctly. Doing it after shaping
produces reordered glyphs from a shaping context that was wrong; doing it before line breaking is
wrong because the reordering is per line, not per paragraph.

The pieces that come with it and are individually easy to forget: **mirrored characters** (a `(` in
right-to-left text paints as `)`), **the caret**, which has two positions at a direction boundary,
and **`dir="auto"`**, which infers direction from the first strong character.

`writing-mode` — vertical Japanese and Chinese — is **explicitly out of scope** here. It is a second
axis through all of layout, not a text feature, and it is deferred with the reason written down
rather than left to be discovered.

### 4. Line breaking: UAX #14, and the two languages it does not solve

The line-break class table plus the pair table gives correct break opportunities for almost
everything: CJK breaks between most ideographs, Latin breaks at spaces and after hyphens, and the
no-break rules around punctuation stop a line from starting with a closing bracket.

**Thai, Khmer, Lao and Burmese are the exception**, because they need dictionary-based word
segmentation, which is a dictionary and therefore a size decision. The honest position: UAX #14
gives them almost no break opportunities, so a paragraph becomes one long line. That is a known,
written-down limitation — the same shelf as `writing-mode` — and not something to paper over with a
break-anywhere heuristic that would produce confidently wrong output.

`word-break`, `overflow-wrap` and `hyphens: manual` come with the table. `hyphens: auto` needs
hyphenation dictionaries and does not.

### 5. Order, and the honest ranking against everything else

This ADR is **lower priority than every ADR from 0015 to 0024** for the named target sites, and
higher priority than most of them for the stated goal of any page. The order within it is by
breakage severity:

1. **Encoding sniffing + UTF-8 + windows-1252.** Without it a page in the wrong encoding is
   unreadable, and it is also the security item.
2. **UAX #14 line breaking.** Without it CJK pages overflow their boxes and are unreadable in a
   different way.
3. **The remaining legacy decoders.**
4. **UAX #9 bidi.** Largest of the four, and the one that makes right-to-left languages work at all.

## Consequences

- **`src/html`'s entry point changes** from bytes-assumed-UTF-8 to a decoded code point stream, which
  touches the tokenizer's input handling and every test that feeds it a literal.
- **`src/util` gains generated tables and a generator**, and the tables are large enough that their
  size should be measured and recorded — a few hundred kilobytes of static data in a browser that
  cares about footprint is worth knowing rather than assuming.
- **`src/js` gets `normalize` and the rest of `\p{…}` nearly free**, once the tables exist. The
  roadmap lists them as remaining gaps waiting on exactly this data.
- **Layout's line breaker moves from a space scan to a table lookup**, which is a hot path. It needs
  a measurement before and after, per `guidelines/performance.md`.
- **Bidi makes text layout genuinely harder to reason about**, permanently. Every subsequent text
  feature — selection, caret movement, `text-align`, justification — has to consider two directions.
  That cost is paid once and never refunded, which is an argument for doing it before the text code
  grows rather than after.
- **Two written-down limitations remain**: no `writing-mode`, and no dictionary segmentation for Thai
  and its neighbours. Both are decisions here rather than gaps discovered later.

## Alternatives considered

**Take ICU.** Rejected on size and scope. It would be the largest dependency in the project, most of
it for locale and formatting services this browser does not offer, and it would make the Unicode data
question answered-by-import rather than decided. If `Intl` ever becomes a requirement this should be
revisited, and that is the condition under which it would be.

**Assume UTF-8 and treat everything else as a broken page.** Rejected, though it is tempting: UTF-8
is the overwhelming majority of new content. It fails on the archive — a great deal of what people
actually read is old — and it fails as a security position, because the fallback encoding is where
the confusion bugs live and "assume UTF-8" is itself a choice of fallback, just an undocumented one.

**Implement bidi only in the shaper, since HarfBuzz already handles right-to-left runs.** Rejected on
a misunderstanding worth naming: HarfBuzz shapes a run whose direction it is *told*. It does not
determine direction, does not split paragraphs into runs, and does not reorder them. Passing it
logical text with a right-to-left flag produces a correctly shaped, incorrectly ordered line.

**Break CJK lines anywhere, since it is nearly right.** Rejected. "Nearly right" here means breaking
before a closing quotation mark or after an opening bracket, which is visibly wrong to any reader of
the language and is the kind of error that says the implementer did not check.

**Defer all of this until after the target sites work.** This is the actual plan and it is not an
alternative — the roadmap sequences it after ADR 0015–0024. What is rejected is deferring the
*decision*, particularly the Unicode data question, because four separate subsystems will otherwise
each answer it locally and differently.
