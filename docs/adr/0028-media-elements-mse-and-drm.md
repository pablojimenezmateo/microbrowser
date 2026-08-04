# ADR 0028 — The media element, Media Source Extensions, and the refusal of DRM

**Status:** accepted · **Date:** 2026-08-04

## Context

ADR 0013 decided the two things that had to be decided before any media code existed: **a video
frame reaches the screen through a surface the compositor owns, never through the display list**, and
**decoders are third-party, sandboxed and out of process**, with the specific codec dependency
deliberately deferred until the media stack says what it needs.

This ADR is the media stack saying what it needs, measured rather than imagined.

| | occurrences | where |
|---|---|---|
| `MediaSource` | 33 | 10 youtube, **23 plex-main** |
| `.play()` | 34 | all three |
| `SourceBuffer` | 3 | plex |
| `AudioContext` | 6 | youtube |
| **`requestMediaKeySystemAccess`** | **2** | **plex-main, and nowhere else** |

Two findings reorder ADR 0013's picture.

**Plex is the MSE site, not YouTube.** 23 of the 33 `MediaSource` references are in Plex's own
bundle. Plex's web client streams from a server the user runs, transcoding on demand to HLS or
direct-playing an MP4, and it drives the media element through MSE either way. So the first consumer
of the media stack is the site whose content is the user's own files.

**Plex is also the DRM site, and it is the only one.** `requestMediaKeySystemAccess` — the entry
point to Encrypted Media Extensions — appears twice, both in Plex, and nowhere in YouTube's 10.7MB.
ADR 0007 already noted that regular YouTube videos are not Widevine-protected. The DRM question is
therefore narrower than expected and it is not avoidable: Plex uses EME for studio content and not
for a user's own library.

There is a third thing neither ADR has said, and it is the one that gates everything else. **There is
no `<video>` element and no `<audio>` element** — no media element at all, no `play()`, no
`currentTime`, no `timeupdate`, no controls. MSE is an API for feeding bytes to a media element, and
without one there is nothing to feed.

## Decision

### 1. The media element first, and it is mostly a state machine

`HTMLMediaElement` — shared by `<video>` and `<audio>` — with the readiness and network state
machines implemented as the specification defines them rather than approximated. `play()`,
`pause()`, `currentTime`, `duration`, `volume`, `muted`, `playbackRate`, `buffered`, `seekable`,
and the event set (`loadedmetadata`, `canplay`, `playing`, `timeupdate`, `waiting`, `seeking`,
`ended`, `error`).

The state machine is the whole difficulty and it is worth being explicit about why. Every player on
the web is written against those states; a `readyState` that jumps to `HAVE_ENOUGH_DATA` when it
should be `HAVE_CURRENT_DATA` produces a player that stalls with no error and no way for the page to
tell. This is ADR 0012's rule in its most literal form: the states are the API.

**`play()` returns a promise that rejects when playback cannot start**, and autoplay without user
interaction is **refused** — `play()` on a document with no prior user activation rejects with
`NotAllowedError`, unless the element is muted. That is what every browser does, it is the behaviour
pages are written against, and it reads `isTrusted` from ADR 0017, which is where user activation is
defined.

**Default controls are user-agent chrome inside a page**, the same category as the overflow
scrollbars in ADR 0018 — boxes the user agent stylesheet creates, not widgets `src/ui` owns.

### 2. Containers and demuxing are ours, per ADR 0013, and HLS is the surprise

ADR 0013 said containers are ours and gave the reason: a demuxer is a parser over
attacker-controlled bytes, it belongs to the same discipline as the HTML tokenizer, and **it is the
layer that decides what the codec is asked to decode**, so owning it is what keeps the codec's input
constrained.

What has to be demuxed, in order of what the target sites send:

1. **fragmented MP4** — `ftyp`/`moov`/`moof`/`mdat`. YouTube's DASH and most of Plex's direct play.
2. **WebM/Matroska** — the other half of YouTube's DASH.
3. **HLS** — which reddit also uses (its front page references two `.m3u8` playlists) and which Plex
   uses for transcoded streams. HLS is a *playlist format*, not a container: an `.m3u8` text file
   naming segments that are themselves fMP4 or MPEG-TS. Parsing the playlist is cheap; MPEG-TS
   demuxing is a third container.

An `.m3u8` on reddit's front page is a small, useful fact: HLS is not a Plex-only concern, and the
playlist parser is worth having early because it is text and therefore easy.

Each demuxer lands with a fuzz target on its own commit, and each **saturates every size it reads**
— a box length, a track count, a sample count. `AGENTS.md` names the canonical image-decoder overflow;
the container equivalent is a 64-bit box size that wraps when added to an offset.

### 3. MSE, and the buffer model it implies

`MediaSource`, `SourceBuffer`, `appendBuffer`, `remove`, `SourceBuffer.buffered`, `duration`,
`endOfStream`, and the append/coded-frame-processing algorithm underneath.

The part that is easy to underestimate: MSE is **not** "hand bytes to the decoder". `appendBuffer`
runs the coded frame processing algorithm, which parses initialization and media segments, computes
presentation timestamps, coalesces buffered ranges, and handles overlapping appends by removing what
they overlap. `buffered` has to be right because the page's adaptive-bitrate logic reads it every few
hundred milliseconds and decides what to fetch next from it. A `buffered` that lies produces a player
that downloads the wrong thing forever.

**MSE is where a page can allocate unbounded memory**, one `appendBuffer` at a time. The quota is
enforced by the browser and `QuotaExceededError` is thrown when it is exceeded — which is the
specified behaviour and one every player handles, because it is how they are told to evict.

`MediaSource` objects are reached through `URL.createObjectURL`, which means the object URL registry
(6 occurrences of `createObjectURL` in the survey) lands here too, per-document and revoked with it.

### 4. Audio is first, and it is the thread ADR 0013 already described

ADR 0013 called audio "a separate problem and probably the first one" and required an
ADR-0004-style ownership statement for its thread. That stands, and the ordering is confirmed rather
than revisited: audio is smaller than video, an audio-only path can be tested end to end, and the
synchronisation problem — video follows the audio clock, not the other way round — is unsolvable
until there is an audio clock.

Per `AGENTS.md`'s requirement of any thread:

- **It owns** the device handle, the ring buffer it drains, and the playback clock.
- **It borrows** nothing. Decoded samples arrive by handoff into the ring buffer; it never touches a
  document, a decoder, or the heap.
- **It is joined** when the last playing element stops, before the document is destroyed, and before
  `main` returns.
- **It does not exist when nothing is playing**, which is how it stays compatible with zero-idle-CPU.

**`AudioContext` (Web Audio) is not implemented.** Six occurrences, all YouTube, all for
visualisation and volume normalisation that degrade. It is a large API and it is not what makes a
video play.

### 5. EME is refused, and the refusal is the most consequential line in this ADR

**`navigator.requestMediaKeySystemAccess` is not defined.** A page that feature-detects EME finds it
absent and, per ADR 0012's rule, takes whatever path it has for a browser without DRM.

Encrypted Media Extensions is a specification whose entire content is *loading a closed-source
binary module (a Content Decryption Module) into the browser and giving it privileged access to
decoded media and to a device-unique identifier*. Every clause of that is incompatible with something
this project has already decided:

- **Security.** ADR 0004's threat model isolates decoders because they are the highest-value RCE
  target. A CDM is a decoder we cannot read, cannot audit, cannot fuzz, and cannot patch. Shipping it
  is shipping an unauditable binary into the process that renders web pages.
- **Privacy.** Widevine provisioning involves a device identifier that persists and is transmitted.
  `AGENTS.md`'s privacy rules forbid state that identifies the machine, and a DRM device ID is
  precisely that.
- **Dependencies.** ADR 0001 sanctions a small, readable, replaceable dependency set. A CDM is
  distributed as a binary blob under an agreement, which is the opposite of every property that list
  is selecting for.
- **From scratch.** The project's own mission statement is "own HTML parser, own CSS engine, own
  layout, own software rasterizer, own HTTP client, own JavaScript engine". A closed binary decoding
  the video is a strange thing to find in that browser.

**What it costs, stated without softening**: no Netflix, no Disney+, no Amazon Prime Video, no Spotify
web player, and **no Plex playback of studio-licensed content** — Plex's own "watch free movies and
TV" catalogue. A user's own library, which is what most people run Plex for, is not encrypted and
plays.

This is the hardest refusal in the document and it should be re-examined if the project's goals ever
change. It is written at length so that re-examining it means arguing with the reasons rather than
rediscovering them.

### 6. Order

1. **Audio output and the audio thread** — the clock everything else needs.
2. **`HTMLMediaElement`** and its state machine, driven by a progressive fMP4 over HTTP. A `<video
   src="…mp4">` that plays is the first end-to-end proof.
3. **The codec dependency** — ADR 0013's deferred choice, made now that the demuxers say what
   they emit. The evidence points at needing H.264, VP9, AV1, AAC and Opus, which is the set that
   makes ffmpeg's breadth tempting and its size the counter-argument; that trade is ADR 0013's to
   settle in its own follow-up, in a sandboxed process either way.
4. **The video surface** — ADR 0013's decided design, built.
5. **MSE**, and with it the object URL registry.
6. **HLS playlists**, then MPEG-TS if Plex's transcode path needs it.

## Consequences

- **This is the largest single body of work on the roadmap** and the only one that cannot be
  meaningfully partially delivered: a video that plays with no audio, or audio that drifts, is not a
  half-working feature.
- **The codec dependency stops being deferrable at step 3**, which is where ADR 0013's follow-up ADR
  is due. This ADR deliberately does not pre-empt it; it only supplies the codec list the choice
  needs.
- **The first real-time deadline in the project arrives with the audio thread.** Everything until now
  has been "wake up when there is work"; an audio callback that is late produces an audible click.
  That is a genuinely different engineering constraint and it deserves its own measurement discipline.
- **A page can hold hundreds of megabytes in `SourceBuffer`s.** The quota is the only thing between
  a media page and the machine's memory.
- **The refusal of EME will be reported as a bug** by anyone who tries a streaming service, and the
  answer is a documented decision rather than a missing feature. That distinction only holds if it is
  written where a user can find it, not only here.

## Alternatives considered

**Implement EME with a real CDM.** Rejected on four independent grounds above, any one of which would
be sufficient. It is the single largest compatibility loss this project accepts.

**Implement EME with a stub CDM that fails at the licence step.** Rejected as a textbook ADR 0012
stub, and a worse one than usual: the page detects EME, selects the encrypted stream, fetches it, and
fails at decode with no fallback path left. Absence sends it to the unencrypted stream where one
exists.

**Support `<video src>` progressive playback only, and skip MSE.** Rejected on the measurement: 33
`MediaSource` references, and both YouTube and Plex use MSE for everything that is not a trivial
case. Progressive playback is step 2, not the destination.

**Use the system media framework (GStreamer) for the whole stack.** ADR 0013 raised this and rejected
it as a default while keeping it open. Nothing measured here changes that, and one thing sharpens it:
HLS on reddit means the playlist layer is ours regardless, and GStreamer's value was mostly in the
layers below.

**Skip audio, since the video is the visible part.** Rejected. Video timing is derived from the audio
clock in every real player; a video-first implementation has to invent a clock and then throw it away.
