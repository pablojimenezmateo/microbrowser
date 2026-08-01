# ADR 0005: Named compatibility targets, and what they cost

## Status

Accepted.

## Context

The roadmap in `README.md` describes what to *build*. It says nothing about what
should *work*, and those are different questions: a browser can complete every
milestone on that list and still fail to render any page a person wants to look
at, because real sites depend on the parts nobody schedules.

Five sites are named as the target set:

- `news.ycombinator.com`
- `reddit.com`
- `google.com`
- Plex (the web client)
- `youtube.com`

They are listed here because "is the browser getting closer?" is otherwise a
feeling. With named targets it is a checklist, and a checklist can be wrong in
public rather than quietly.

## Decision

**These five are the compatibility targets, in this order.** The order is by
what each demands, not by preference:

### 1. news.ycombinator.com — reachable on the current roadmap

Server-rendered HTML, table-based layout, a few hundred bytes of CSS, and
almost no script. What is missing is small and already on the list:

- HTML tables (an M3 gap: the tree builder has no table insertion modes)
- Links and navigation from a click
- Form controls, for the comment and search boxes

This is the first target and should be treated as one — it is close enough that
aiming at it deliberately will find real gaps.

### 2. old.reddit.com — mid-project

Server-rendered, moderate CSS, some script. Needs most of the layout work
(floats are in; `position` and overflow scrolling are not) and a working event
loop. New reddit is a React application and belongs in tier 4.

### 3. google.com — search results before the homepage

The results page is lighter markup than it looks. Both pages run real script,
Google varies its markup by user agent, and in practice it requires HTTP/2.
"Type a query and read the results" is a plausible target; the full experience
is not.

### 4. Plex — a single-page application *and* media playback

Two hard problems at once. Nothing here is reachable before the JavaScript
engine is real and the media stack exists.

### 5. youtube.com — the hardest, and worth stating why

The page is a web-components single-page application of several megabytes of
minified script. The video is fragmented MP4/WebM over DASH, fed to the decoder
by script through Media Source Extensions, in VP9/AV1/H.264 with Opus or AAC
audio. Regular videos are not Widevine-protected, which removes one otherwise
fatal blocker.

Note the split: **playing a YouTube video** given its stream URL is a bounded
media problem, and a far smaller one than rendering youtube.com. If the goal is
ever "watch the video" rather than "render the site", that path exists and is
much shorter.

## Consequences

### The JavaScript engine becomes the dominant cost of the project

Not a milestone within it. Four of the five targets are unreachable without an
engine that is complete enough and fast enough to run a modern framework. That
means, beyond what M8 has so far:

- A **bytecode VM**. The tree-walker is roughly two to three orders of magnitude
  slower than a production engine, which is the difference between a page that
  loads and one that never finishes. It also fixes a real defect rather than
  only being faster: the collector cannot run during evaluation today, because a
  tree-walker keeps live values in C++ frames it cannot scan. A VM's value stack
  is explicit and scannable, so precise collection and the speed arrive
  together. See the note in `src/js/Heap.h`.
- **Classes**, including fields, private names, `super`, and accessors.
- **Promises and a microtask queue**, then **async/await**. This is not a
  language feature that can be bolted on: it changes the host's event loop, and
  the loop is currently a blocking wait on window events.
- **Generators and iterators**, which `for...of` is defined in terms of.
- **A regular expression engine.** Currently a regex literal evaluates to its
  own source text, which is a placeholder and not a feature.
- **`Symbol`, `Proxy`, `Reflect`, getters and setters, `WeakMap`**, and the rest
  of the surface a framework's own runtime uses.
- **Modules**, with the loading they imply.

### It changes the timeline, not the shape

The roadmap's ordering stays right. The 12–18 month estimate in `README.md` does
not survive this decision and should not be quietly kept: the engine alone is a
multi-year effort at one person's pace, and the DOM binding layer, the layout
work (flexbox, grid, `position`, overflow), HTTP/2 and brotli, and the media
stack are each substantial on their own.

Stating that is the point of writing it down. A target set that cannot be met on
the stated schedule is worth knowing about now rather than discovering at the
end.

### It adds work that is currently unscheduled

- **Layout**: flexbox and grid are not optional for any of these sites, and
  neither is `position: absolute/fixed/sticky` or a real overflow/scrolling
  model.
- **Networking**: HTTP/2 is effectively required by Google and YouTube, and
  brotli is expected everywhere. Connection pooling and a partitioned DNS cache
  are already known M2 gaps.
- **Media**: audio output does not exist, and a video path that goes through the
  display-list diff at sixty frames a second is the wrong design — a video
  surface has to bypass it.
- **Third-party dependencies**: a video codec is not something to write from
  scratch. dav1d, libvpx or ffmpeg would each need an ADR of their own against
  the rules in ADR 0001.

## Alternatives considered

**Leave the targets unnamed.** Rejected: without them, every feature looks
equally important, and the ones that actually block real pages get scheduled by
accident.

**Name only Hacker News.** Rejected as dishonest in the other direction — the
ambition is the five, and a roadmap that hides the four hard ones does not help
anyone plan.
