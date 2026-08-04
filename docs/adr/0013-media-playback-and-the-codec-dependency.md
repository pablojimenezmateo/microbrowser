# ADR 0013 — Media playback, and the codec dependency

**Status:** accepted · **Date:** 2026-08-04

## Context

ADR 0007 names youtube.com as the last compatibility target and says why it is the hardest: the
video is fragmented MP4/WebM over DASH, fed to the decoder by script through Media Source
Extensions, in VP9/AV1/H.264 with Opus or AAC audio. It also draws the distinction this ADR is built
on, and it is the most useful sentence in it:

> **playing a YouTube video** given its stream URL is a bounded media problem, and a far smaller one
> than rendering youtube.com.

Nothing of this exists. There is no audio output at all, no container demuxing, no decoder, and no
video surface. What does exist is a display list with a two-frame diff and a software rasterizer,
and that is the part that makes the design decision urgent rather than distant: **a video frame must
not go through the display list.** A 1080p frame is about 8MB. Diffing two of them sixty times a
second, to discover that the rectangle changed, would spend the entire CPU budget establishing that
a video is playing.

There is also a rule this runs straight into. ADR 0001 sanctions a small dependency set and requires
an ADR to add to it. A video codec is not something to write from scratch — that is not
conservatism, it is that a hand-written video decoder is a memory-safety catastrophe with a decade
of CVEs already written for it. So the dependency question cannot be dodged, and it interacts with
ADR 0004's threat model, which already treats image decoders as the most productive source of
browser RCE and isolates them for that reason.

## Decision

### The split: a video surface, decided now; the media stack, built later

**A video frame reaches the screen through a surface the compositor owns, never through the display
list.** The display list carries a *hole* — a rectangle, a surface identity, and the geometry to
place it — and the presenter composites the surface into that rectangle. The display list diff sees
a command that has not changed, because it has not: what changed is the surface's contents, and that
is not the display list's business.

This is decided now, before any of the rest exists, for the same reason ADR 0003 defined the IPC
seam before the process split: it is the constraint that everything else is built against, and
retrofitting it means rewriting the paint path rather than adding to it.

It also fixes the thing `CLAUDE.md` already lists as crude — a display list carrying an image
serializes the bitmap inline. A surface is named rather than serialized, which is the same resource
table that ADR's known-crude note asks for. The video surface is the case that forces it.

### The dependency: one library, isolated, and not chosen yet

**Decoders are third-party, sandboxed, and out of process.** This follows from ADR 0004 rather than
being a new position: the image decoder is already isolated there, on the argument that decoders are
the highest-value RCE target and their interface is small enough that isolating them is nearly free.
A video decoder is the same argument with a much larger attack surface and a much larger prize.

The candidates are dav1d (AV1), libvpx (VP8/VP9) and ffmpeg (everything). **The choice is not made
here**, because it is not yet informed — it depends on what the media stack turns out to need, and
choosing a dependency before that is how a project acquires ffmpeg by accident. What is decided is
the *shape*: whichever is chosen enters through its own ADR against ADR 0001, and lands in a
sandboxed process with a narrow message interface, never linked into the engine.

**Containers and demuxing are ours.** An MP4/WebM demuxer is a parser over attacker-controlled bytes
and belongs to the same discipline as the HTML tokenizer and the PNG decoder — bounds-checked, fuzzed
on the same commit. It is also small compared to a codec, and it is the layer that decides what the
codec is asked to decode, so owning it is what keeps the codec's input constrained.

### Audio is a separate problem and probably the first one

Audio output does not exist. It is smaller than video, it needs a real-time-ish callback from the
system, and that callback is the first thing in this project that does not fit "the process blocks
in one place". **The audio thread is a thread, with an ADR-0004-style statement of what it owns, what
it borrows, and who joins it** — `AGENTS.md` requires that of any thread, and this is the one where
it will be tempting to skip because the API hands you a callback.

It does not violate zero-idle-CPU: an audio device with nothing playing is not opened, and a browser
with no media has no audio thread at all.

### The bounded path stays available

ADR 0007's distinction is kept as an explicit option rather than a remark. "Play this stream URL"
needs a demuxer, a decoder, a surface and audio out. "Render youtube.com" needs all of that *plus*
MSE, plus the application framework running well enough to drive it. If the goal is ever the former,
it does not wait on the latter, and the surface decision above is what keeps that true.

## Consequences

- **The display list gains a command it cannot diff meaningfully**, and the dirty-region policy has
  to learn that a surface rectangle is damaged every frame while it is playing and not at all when
  it is paused. That is a policy change in `src/app`, not a paint change.
- **Painting a video is a copy, not a rasterization**, so the software rasterizer's cost model stops
  applying to the largest thing on the screen. This is the first place where the no-GPU-requirement
  stance has a real cost: scaling and colour-converting 1080p in software is measurable, and it will
  need the same measurement discipline `docs/performance/` already uses rather than an assumption.
- **A sandboxed decoder process arrives before the rest of the process split**, or the split arrives
  first. Either order works; what does not work is linking a codec into the engine "for now",
  because that is the version that never gets undone.
- **This is the largest single dependency decision the project will make.** It is deferred
  deliberately, and the deferral is recorded so it is a decision rather than an oversight.

## Alternatives considered

**Composite video through the display list like any other image.** Rejected on arithmetic: ~8MB per
frame through a two-frame structural diff at 60Hz, to learn that a rectangle changed. The diff
exists to avoid repainting what did not change, and video is the case where it can only ever answer
"all of it".

**Write the decoders.** Rejected. VP9 and AV1 are years of work each, and the failure mode of
getting them wrong is remote code execution from a video, which is the single most exploited path in
browser history.

**Use the system's media framework (GStreamer, or the platform's own).** Genuinely attractive — it
is somebody else's sandbox and somebody else's decoders — and rejected as a *default* rather than
outright. It is a large dependency with a large API surface, it varies by platform, and it takes the
container parsing out of our hands, which is the layer that decides what the codec sees. Worth
revisiting with the codec ADR rather than assumed away.

**Ship without media and treat YouTube as render-only.** Not rejected — it is the honest interim
position, and it is what the ordering above produces anyway. Rendering youtube.com is reachable long
before playing a video on it, and the two are separable.
