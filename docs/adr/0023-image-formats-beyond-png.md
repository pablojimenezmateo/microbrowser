# ADR 0023 — The image formats a page actually sends, and who decodes them

**Status:** accepted · **Date:** 2026-08-04

## Context

`src/gfx` decodes PNG and renders SVG. That is the whole list.

reddit's front page references **25 PNG and 8 JPEG** image sources across
`styles.redditmedia.com`, `preview.redd.it`, `b.thumbs.redditmedia.com`, `i.redd.it` and
`emoji.redditmedia.com`. So roughly a quarter of the images on the first target page after Hacker
News do not decode, and they do not decode in the most annoying possible way: the layout is right,
the box is there, and the box is empty.

That is the immediate blocker. The larger one is that the format list is not a preference:

- **JPEG** is every photograph on the web.
- **WebP** is what every large site serves to a browser that says it accepts it — and we control
  that, because it is an `Accept` header we send. A browser that omits `image/webp` gets JPEG, which
  is a legitimate strategy with a measurable cost in bytes.
- **GIF** is dead except that it is not: it is animated reaction images, and it is the only reason
  anything on this list needs multi-frame support.
- **AVIF** is the newest and the least necessary, and it shares a decoder family with video.

Against that sits ADR 0001, which sanctions a small dependency set and requires an ADR to add to it,
and ADR 0004, which says the sharpest thing anyone has written in this repository about image
decoders:

> the image decoder is already isolated there, on the argument that decoders are the highest-value
> RCE target and their interface is small enough that isolating them is nearly free.

That is not a theoretical ranking. Image decoding is where browser remote-code-execution bugs
actually come from, year after year, because it is complex binary parsing of attacker-chosen bytes
that every page reaches by default and that no user action gates.

## Decision

### 1. JPEG is ours; WebP and AVIF are not; GIF is ours

The split is by *what the format is*, and the reasoning is the same one ADR 0013 used for containers
versus codecs.

**JPEG: written here.** Baseline and progressive, from the specification. It is a well-understood
format — Huffman, dequantise, IDCT, upsample, colour-convert — with no compression-format
dependency, and writing it means the bounds are ours to enforce rather than ours to trust. It is a
few thousand lines and it is the single highest-value decoder on the list.

**GIF: written here.** LZW plus a frame loop. Smaller than JPEG and it forces the animation question
below to be answered once.

**WebP and AVIF: third-party, or not at all.** WebP's lossy mode is VP8 and AVIF is AV1 — both are
*video codecs* in a still-image container. Writing either is exactly what ADR 0013 refused, for
exactly its reason: "a hand-written video decoder is a memory-safety catastrophe with a decade of
CVEs already written for it". If they land, they land through the codec dependency that ADR 0013
defers, in the same sandboxed decoder process, chosen at the same time. They are **not** a separate
dependency decision, and this ADR deliberately does not make one.

Until then, **the `Accept` header does not claim them.** That is the honest-absence rule at the HTTP
layer: a browser that does not send `image/webp` is served JPEG by every major CDN, including
reddit's and YouTube's. It costs bytes and it costs nothing else, and it is a one-line change on the
day a decoder exists.

### 2. Every decoder runs where a compromise is contained

The decoder interface is designed now, before there are three of them, so that isolation is a
deployment choice rather than a rewrite:

**bytes in, a bitmap and its dimensions out, no callbacks, no allocation the caller does not see, no
access to anything else.** That is already nearly what `PngDecoder` looks like, and keeping it that
narrow is what makes ADR 0004's isolated decoder process cheap when it arrives.

Until the process split lands, decoders run in-process, and the bounds do the work:

- **Every size computed from input saturates.** `AGENTS.md` names `width * height * 4` in `int` as
  the canonical image-decoder heap overflow; it is named because it is the one everybody writes.
- **A maximum decoded pixel count**, enforced before allocation, not after. A 65535×65535 JPEG is
  four lines of header and 17GB of output.
- **A fuzz target on the commit the decoder lands**, per `guidelines/security.md` — non-negotiable
  and the reason to write these one at a time rather than three at once.
- **No decoding of a format the bytes do not claim.** Content sniffing is limited to a magic-number
  check that selects a decoder, and a mismatch between `Content-Type` and magic is resolved toward
  the magic and logged — never by trying decoders until one succeeds, which turns every decoder into
  a target for every image.

### 3. Animation is a frame source, not a paint loop

GIF, animated WebP and animated AVIF are all the same shape: a sequence of frames with delays. The
decision follows ADR 0011 and ADR 0013 rather than inventing anything:

- An animated image **advances on a deadline registered through `IdleWaitState::next_deadline_ms`**,
  like every other timed thing in this browser.
- An animated image that is **not visible does not advance** — off-screen, in a collapsed subtree,
  or in a background tab. This is where a browser silently burns a core, and the check is the
  scrollport intersection ADR 0018 already computes.
- Decoding is **incremental**: frame N is decoded when it is needed, not all of them at load. A
  hundred-frame GIF at 1000×1000 is 400MB fully decoded, and there is a reaction image on reddit that
  will find that out.

The frame is a bitmap swap under a display-list command whose geometry did not change, which the
two-frame diff already handles correctly — this is the small, tractable version of the problem
ADR 0013 solved with a surface for video, and it does not need a surface.

### 4. `srcset`, `<picture>` and `loading="lazy"` come with the images

reddit's front page uses `srcset` 6 times and `loading="lazy"` 27 times. Both are image *selection*
rather than image decoding, and both are cheap once the pieces exist:

- **`srcset` / `sizes` / `<picture>`** pick a candidate from density and viewport width. Without it,
  a browser picks the first source, which is usually the wrong resolution — visibly blurry or
  wastefully large. It needs `devicePixelRatio` and the viewport, both of which exist.
- **`loading="lazy"`** is an `IntersectionObserver` against the scrollport (ADR 0018), and it is the
  difference between reddit's front page fetching 33 images and fetching the six that are on screen.
  On this browser that matters more than on most, because ADR 0010's transport work is not finished.

### 5. Order

1. **JPEG.** It unblocks reddit today and every photographic page after it.
2. **`srcset` / `<picture>` / `loading="lazy"`.** Cheap, and they change what gets fetched.
3. **GIF**, with the animation policy above.
4. **WebP**, if and when ADR 0013's codec dependency lands — and then `Accept` grows to claim it.
5. **AVIF**, same condition, later.

## Consequences

- **`src/gfx` grows two decoders and the module's line budgets will fire.** Each decoder is its own
  translation unit and its own fuzz target; the module is the right home because a decoder produces a
  bitmap and knows nothing else.
- **The decoder interface becomes a contract that has to survive being moved out of process.** It is
  cheap to design that way now and expensive to retrofit, which is ADR 0003's argument reused.
- **The browser will keep sending `Accept: image/png, image/gif, image/jpeg, image/svg+xml, */*`
  until a WebP decoder exists**, and will therefore transfer more bytes than a browser that lies.
  That is the same class of decision as the honest `User-Agent` and it is made the same way.
- **Animation introduces the second recurring wakeup source in the browser** (after
  `requestAnimationFrame`). The visibility rule is what keeps it from being the one that costs the
  project its central property, and it needs the same kind of test `bindings::AnimationFrames` has.
- **Decoded images are the largest thing in the browser's memory after the JavaScript heap**, and
  there is currently no image cache with an eviction policy. `CLAUDE.md` already notes that a
  display list serializes bitmaps inline; adding three more formats makes that note more expensive
  and is a second reason to fix it.

## Alternatives considered

**Take libjpeg-turbo, libwebp and libavif as dependencies.** Rejected for JPEG, accepted in principle
for WebP/AVIF. The asymmetry is deliberate: JPEG is a format we can implement correctly and bound
ourselves, and every third-party decoder is code we cannot audit running on the most-attacked input
path in the browser. For WebP and AVIF the calculation inverts, because writing a VP8 or AV1 decoder
is worse than depending on one — which is precisely ADR 0013's finding.

**Write a WebP decoder for lossless mode only, since it is not VP8.** Rejected as a trap. Lossless
WebP is a different format sharing a container, so a "WebP decoder" that handles only lossless fails
on most WebP in the wild — and it would make `image/webp` in `Accept` a lie, which is worse than
omitting it.

**Send `image/webp` in `Accept` and show nothing when it arrives.** Rejected as the HTTP-layer
version of a stub. The `Accept` header is feature detection over the wire, and lying in it produces
exactly the wall ADR 0012 describes.

**Decode every format lazily, only when the image scrolls into view.** Rejected as a default: it
makes the page reflow as the user scrolls, because intrinsic size is not known until decode. The
header is parsed eagerly for dimensions; the pixels can wait, and that split gets the benefit without
the reflow.
