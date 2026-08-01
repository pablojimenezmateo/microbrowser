# ADR 0001 — Third-Party Dependencies

**Status:** accepted · **Date:** 2026-08-01

## Context

This is a from-scratch browser. "From scratch" needs a boundary, or it becomes either a pose (vendor
everything and call the glue "ours") or a decade (write a TLS stack and a font shaper before
rendering a paragraph).

The boundary has to be decided once, up front, because dependency creep is not a series of
decisions — it is the absence of one. Every individual library looks reasonable at the moment it is
added.

## Decision

A dependency is sanctioned only if it meets all four:

1. **It is a solved problem with a correctness-critical, standards-defined answer** that we would
   reimplement identically and worse. TLS record framing. Bézier hinting. Unicode shaping.
2. **It sits behind a seam.** Exactly one module names it, declared in that module's `extern:`
   field, and its types never cross a module boundary.
3. **It is small enough to read.** If we cannot audit it, we cannot claim the privacy properties
   this project is built on.
4. **Writing it ourselves would not teach us anything about browsers.**

### Sanctioned

| Library | Purpose | Only in | Why not ours |
|---|---|---|---|
| **SDL3** | Window, input, event loop, texture present | `platform` | Cross-platform windowing is thousands of lines of X11/Wayland/Win32/Cocoa trivia with zero browser content. |
| **FreeType** | Glyph outline rasterization | `gfx` | Hinting and the outline formats are decades of accumulated correctness. Getting them wrong is visible on every character. |
| **HarfBuzz** | Text shaping | `gfx` | Arabic joining, Indic reordering, and OpenType feature application are a specialty. Ladybird uses it too. |
| **OpenSSL** | TLS record layer only | `net` | Writing your own TLS is the canonical example of what not to do. We still own HTTP entirely. |
| **zlib**, **brotli** | Content decoding | `net` | Format-defined, ubiquitous, small. |
| **stb_image** | PNG/JPEG decode, temporarily | `gfx` | Placeholder so images render before M6. Replaced by our own decoders — image decoding *is* browser work, and it is a fuzzing surface we want to own. |

### Rejected

**Skia** — what Chrome and Ladybird use, and genuinely excellent. Rejected because it is enormous:
long builds, a large binary, and heavy memory, all cutting directly against the low-footprint goal.
Writing the rasterizer is also a large fraction of the point of the project.

**Blend2D** — a much better size argument than Skia and a real temptation. Rejected for the same
second reason: the rasterizer is not incidental to a browser, it is one of the interesting parts.

**libcurl** — battle-tested, and HTTP/2 for free. Rejected because the privacy layer needs total
control over every request: connection reuse partitioning, DNS routing, redirect policy, header
ordering. Configuring curl not to do things is a weaker guarantee than not implementing them.

**ICU** — huge, and we need a small slice (UTF-8/16, UAX #14 line breaking, UAX #29 segmentation).
We generate the tables we need.

**Qt / GTK** — a browser draws its own UI. A widget toolkit would be a second, conflicting
rendering model.

**vcpkg / Conan** — the dependency list is short enough to install from the system package manager.
A package manager is infrastructure for a problem we have chosen not to have.

## Consequences

- We write: HTTP/1.1, cookies, cache, URL parsing, HTML parsing, DOM, CSS parsing, cascade, layout,
  paint, compositing, the rasterizer, image decoders, JavaScript, and the GC. This is the project.
- The build needs only `libsdl3-dev`, `libfreetype-dev`, `libharfbuzz-dev`, `libssl-dev`,
  `zlib1g-dev`, `libbrotli-dev`.
- Adding a dependency requires a new ADR and an `extern:` declaration. The lint rejects an
  undeclared third-party include, so this cannot be done by accident.
- We will be slower to first useful page than a project that vendors an engine, and we accept that.

## Open

**HTTP/2 and HTTP/3** are unresolved. HTTP/1.1 with a connection pool is enough for a long time, and
h2's HPACK plus stream multiplexing is real work. Revisit with measurements, not assumptions —
per-origin connection limits matter less when a third of a page's requests are being blocked anyway.
