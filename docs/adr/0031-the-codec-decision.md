# ADR 0031 — The codec decision: which decoders, from where, and in what process

**Status:** accepted · **Date:** 2026-08-06

## Context

ADR 0013 deferred this deliberately and said why:

> The candidates are dav1d (AV1), libvpx (VP8/VP9) and ffmpeg (everything). **The choice is not made
> here**, because it is not yet informed — it depends on what the media stack turns out to need, and
> choosing a dependency before that is how a project acquires ffmpeg by accident.

It is informed now. The media stack around the decoder exists: the audio ring, the device and the
playback clock (session 24), the element's state machines and its `play()` promise (session 25), and
all three demuxers — fragmented MP4, WebM/Matroska and the HLS playlist parser (sessions 26 and
earlier). What is missing is the thing that turns a byte range into a frame.

**What the codec has to be, measured rather than assumed.** ADR 0028 §6 names the set as H.264, VP9,
AV1, AAC and Opus. Two independent checks today agree with it:

| source | container | video | audio |
|---|---|---|---|
| `test-streams.mux.dev` HLS master (5 variants) | MPEG-TS segments | `avc1.64001f`, `avc1.42000d`, `avc1.420016` | `mp4a.40.2`, `mp4a.40.5` |
| a WebM produced locally by ffmpeg | Matroska | `V_VP9` | `A_OPUS` |
| YouTube DASH (ADR 0007's survey) | fMP4 + WebM | VP9, AV1, H.264 | Opus, AAC |
| Plex direct play (ADR 0007's survey) | fMP4 | H.264 | AAC |

So the set is five codecs, and **two of them — H.264 and AAC — are unavoidable**: they are what the
HLS stream measured above serves and what Plex direct-plays. A browser that shipped only the open
formats would fail the sites this project chose as targets, which is ADR 0007's measurement rather
than a preference.

**What the dependency rules say.** ADR 0001 sanctions a small, readable, replaceable dependency set
and requires an ADR to add to it. ADR 0004's threat model treats decoders as the highest-value RCE
target and isolates them for that reason. ADR 0013 already decided the *shape* — third-party,
sandboxed, out of process, never linked into the engine — so what is left here is which libraries,
and where the boundary is drawn inside the sandbox.

## Decision

### 1. The libraries: the reference decoder where one exists, `libavcodec` where none does

| codec | library | why this one |
|---|---|---|
| AV1 | **dav1d** | The reference-grade decoder the AV1 ecosystem is tested against, ~50k lines, no encoder, no container parsing. Exactly ADR 0001's shape. |
| VP9 / VP8 | **libvpx** | Same argument: it is what YouTube's VP9 is encoded by and validated against, and a decode-only build is a narrow library. |
| Opus | **libopus** | Small, single-purpose, the reference implementation, and already the audio codec of every WebM this project has produced. |
| H.264 | **libavcodec**, decoders only | There is no small clean-room H.264 decoder worth trusting. openh264 exists and is narrower, but it is a Cisco-licensed binary-distribution arrangement rather than a library one audits and patches, which is the property ADR 0001 selects for. |
| AAC | **libavcodec**, decoders only | Same: fdk-aac is a licence question rather than a code-quality one, and its licence is not one this project can accept. |

This is a **mixed** decision rather than "ffmpeg for everything", and the reason is ADR 0001's
criteria applied honestly rather than uniformly. Where a small, readable, replaceable decoder exists
that the web is actually tested against, using it is strictly better: it is auditable, it is the
implementation the encoders were validated against, and replacing it later is a contained change.
Where none exists — H.264 and AAC, both patent-encumbered, both without a credible small
implementation — the large dependency is the honest answer, and confining it to those two codecs is
what keeps the rest of the stack out of it.

The cost is two APIs behind one interface. That cost is real and it is paid once, in the broker
described below, because the broker exists anyway.

### 2. The allowlist lives in our code, not in a build flag

**The decoder process refuses to decode anything the demuxer did not name, and the list of what it
will decode is a table in this repository rather than a set of `configure` options.**

This is the load-bearing decision of the ADR and it is worth being precise about why. ADR 0013 says
the container is ours because "it is the layer that decides what the codec is asked to decode, so
owning it is what keeps the codec's input constrained". A build flag does not keep that true:
`--disable-everything --enable-decoder=h264,aac` is correct on the day it is written and drifts the
first time somebody debugs a build, and a drifted flag re-enables a hundred parsers we own the
replacements for. A table in our own source, checked in the broker on every configure request, does
not drift silently — it fails a test.

The table is five entries. Anything else — a codec id from a container we do not read, a codec name a
page put in a `MediaSource` type string, an id `libavcodec` would happily accept — is refused before
the library is called at all.

### 3. The boundary: bytes in, frames out, and nothing else crosses

The decoder process gets:

- **A configure message**: one of the five codecs, from our own enumeration, plus the codec's
  configuration record as *bytes* (an `avcC`, a `vpcC`, an Opus head) — which the demuxer already
  reports as a byte range and which nothing in the engine interprets.
- **Encoded samples**: bytes, a timestamp, and whether it is a sync sample.

It answers with **decoded frames in shared memory** and a small descriptor: the surface name, the
dimensions, the format, and the timestamp. It never answers with a pointer into its own address
space, and the engine never maps its heap.

Three things deliberately do not cross:

- **No URLs.** The decoder never learns where a sample came from. It cannot fetch, and it has no
  reason to know a network exists.
- **No file paths.** A configure message carries bytes, not a path, so a compromised decoder cannot
  ask for a file it was not given — which is the whole point of the seam being narrow rather than
  convenient.
- **No display list, no DOM, no JavaScript.** ADR 0013's video surface already means a frame reaches
  the screen as a *hole plus a surface*, so the decoder's output has no path into the paint tree.

The message set is typed and versioned, in `src/ipc`, for the reason every message there is: a
renderer's messages are attacker-controlled and the deserializer is the trust boundary.

### 4. The sandbox: what the decoder process may do

Per ADR 0004, and stated concretely because "sandboxed" without a list is a wish:

- **No filesystem.** It is handed its shared-memory regions and its socket at start-up and opens
  nothing afterwards.
- **No network.** No sockets beyond the one it was given, which is not an outbound one.
- **No `fork`/`exec`.** A decoder that can spawn a process is a decoder that can escape a policy
  applied to itself.
- **A seccomp-bpf allowlist** on Linux: read, write, mmap of already-owned regions, futex, exit. A
  syscall outside it kills the process rather than returning an error, because a decoder that
  gracefully handles being denied `open` is a decoder that keeps trying.
- **A crash is a normal event.** The engine treats a dead decoder as a decode failure — the element
  fires `error`, the media state machine already has that transition (session 25) — and does *not*
  restart it for the same sample. A restart loop on a hostile file is a denial of service the page
  chose.

One decoder process per **codec instance**, not per browser: two videos are two processes. That is
more expensive and it is the right cost, because it is what makes a crash local to one element.

### 5. Hardware decode is refused for now, and this is the one to revisit first

**No VA-API, no NVDEC, no VideoToolbox.** A hardware decoder is a kernel driver interface reached
through a userspace library that is often vendor-supplied, and it is exactly the class of dependency
ADR 0001 exists to keep out — with the added property that a GPU driver bug is a *kernel*
compromise rather than a process one. It also cannot be sandboxed the way §4 describes: the whole
point of the interface is direct device access.

What it costs is stated plainly: 4K video will not play smoothly, and a laptop playing 1080p will
use noticeably more power than a browser that offloads it. That is a real cost for a real reason, and
it is the first line of this ADR to re-examine once the process model is proven — because unlike
EME's refusal (ADR 0028 §5), nothing about hardware decode is *incompatible* with this project's
values. It is deferred on risk and complexity, not on principle.

### 6. What this does not decide

- **Which library decodes MPEG-TS's contents.** HLS segments are often MPEG-TS, which is a fourth
  container. It is ours to write per ADR 0013 and it is not written; until it is, HLS plays only
  fMP4 segments.
- **Resampling and colour conversion.** A decoder emits what it emits — YUV at some subsampling, a
  sample rate the file chose. Converting is not the codec's problem and not the sandbox's; `src/gfx`
  already does triangle chroma upsampling for JPEG, and `src/media` will need one resampler. Where
  they live is a `MODULE.deps` question for the session that needs them.

## Consequences

- **Four new entries on ADR 0001's sanctioned list**: dav1d, libvpx, libopus, and libavcodec-with-a-
  five-entry-allowlist. That is a large addition by this project's standards and the largest single
  one so far.
- **The build gains a variant that is not built by default.** A decoder process links libraries the
  engine must not, so the sandbox target is its own executable with its own dependency set — and the
  architecture lint should learn that no `src/` module may name any of the four.
- **`MediaTrack::codec` carries two spellings of the same question today**: `V_VP9`/`A_OPUS` from
  WebM and four-character codes from MP4. The broker is where they are reconciled into the five-entry
  enumeration, and that mapping is a table with a test rather than a `if (codec.find("vp9"))`.
- **A crash becomes an ordinary path.** The media state machine's `error` transition exists; what does
  not exist yet is the engine noticing a dead child. That is a wait-descriptor question, and it is the
  same shape ADR 0011 already solved for sockets.
- **This is the first process boundary that is not the UI/engine split.** ADR 0003's seam was designed
  for one; a second one that is *not* symmetric — the decoder never initiates anything — is a useful
  proof that the seam generalises, and if it does not, that is worth knowing before the renderer split
  (session 42) rather than during it.
- **Studio-licensed streaming still does not play**, and that is ADR 0028 §5's refusal rather than
  this one's: EME is a closed binary with device identity, and no codec decision changes it.

## Alternatives considered

**ffmpeg for all five codecs.** The obvious answer, and rejected on ADR 0001's criteria rather than on
security: inside the sandbox of §4 the attack surface argument is roughly neutral, but "small,
readable, replaceable" is not, and for three of the five codecs a library with all three properties
exists and is the one the encoders were tested against. Choosing ffmpeg for AV1 when dav1d exists
would be choosing the larger dependency for uniformity, which is the trade ADR 0001 explicitly does
not make.

**Individual libraries for all five** — openh264 and fdk-aac. Rejected on licensing, which is not a
technical objection and is decisive anyway: openh264's distribution model is a Cisco-hosted binary,
and fdk-aac's licence is not one this project can ship. Both would also be *worse* code to audit than
libavcodec's decoders, which is the opposite of the reason to prefer a small library.

**Write the decoders.** Rejected in ADR 0013 and still rejected. VP9 and AV1 are years of work each,
and — the part worth repeating — a hand-written video decoder is a memory-safety catastrophe with a
decade of published CVEs to copy from.

**In-process decode behind a "careful" API.** Rejected. This is the position every browser has
retreated from, and ADR 0004 already treats it as settled: decoders are where remote code execution
comes from, and an in-process decoder makes every CVE in four libraries a CVE in the browser.

**One decoder process for the whole browser.** Cheaper, and rejected: a crash in one video would take
out every video, and a hostile file in one tab would be a denial of service against another. Per
instance is the isolation the sandbox is *for*.
