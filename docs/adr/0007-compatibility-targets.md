# ADR 0007: Named compatibility targets, and what they cost

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

### 1. news.ycombinator.com — reached

Server-rendered HTML, table-based layout, a few hundred bytes of CSS, and
almost no script. The front page and a comments page both render, and clicking
a story navigates to it.

Aiming at it deliberately did what this ADR predicted, and more bluntly than
expected: the things it turned out to need were not the things on the list
above. Font *stacks* were never split, so `font-family: Verdana, Geneva,
sans-serif` matched nothing and **no page with a stylesheet rendered any text
at all**. `text-align` was cascaded, inherited and stored and then never read.
A self-closing `<tr>` closed its row, which foster-parented the rest of the
page out of its table. Table columns were divided evenly rather than sized to
their content. None of these failed a test; every one of them was obvious the
moment a real page was on screen.

The lesson to carry to the next target is the method, not the fixes: load it,
snapshot it, write down what is wrong, and work from that list.

Still missing on this site, and deliberately: `<select>` is laid out and
submitted but does not open on a click; `cellspacing` is not mapped, because
the box model has no `border-spacing` and moving the gap inside the cell would
change where text wraps; and `:visited` matches nothing, which is a privacy
decision rather than an unimplemented one.

### 2. old.reddit.com — renders

Server-rendered, moderate CSS, some script. **Surveyed 2026-08-04** by this
ADR's own method, and it produced the same lesson Hacker News did for the third
time running: what it needed was not what this section predicted. The prediction
was "most of the layout work — `position` and overflow scrolling". None of the
three things that actually blocked it was on that list.

**We sent no `User-Agent` at all**, and reddit's edge blocks that outright. The
first snapshot rendered a page titled "Blocked". This is worth stating as a
category, not an incident: the target set exists to find out what stops a real
page, and the first thing that stopped one was a request header we had never
had a reason to send. Measured before it was chosen — reddit serves the honest
string `microbrowser` the same page it serves Chrome, so claiming to be Chrome
would buy nothing and cost the whole point of naming a target.

**`overflow` was applied to non-replaced inline boxes**, which CSS 2.1 §11.1.1
says it does not apply to. `.thing .title { overflow: hidden }` on an `<a>`
clipped every story title on the front page to an inline box's geometry, which
is empty. They were all in the display list, at the right coordinates, in the
right colour, behind a 0×0 clip — which is the failure mode worth remembering,
because a screenshot cannot tell that apart from a title that was never
recorded.

**`display: inline-block` was cascaded, stored, and then laid out as `inline`.**
That was the real one. Every inline-block on the page — flair, domain links, the
whole buttons row — had no geometry of its own, so no width, no height, no
padding, and a background painting nothing. It is now a box kind of its own.

Still missing, and now the visible ones: `vertical-align` does not exist, so
reddit's `vertical-align: middle` flair sits on the baseline; the subreddit
header bar overlaps itself; and the right sidebar's float is wrong.

Its scripts still fail, and the reported error is a red herring worth writing
down so the next session does not chase it. `reddit-init.js` wraps itself in
`try { … } catch (err) { r.sendError(…) }`, and `r` is defined *inside* the
try. So anything that throws early makes the catch handler throw
`ReferenceError: r is not defined`, and every later script that expects `r`
fails too. The real first error is masked; unmasking it needs a way to evaluate
a prelude before a page's own scripts, which the snapshot tool cannot yet do.

**`www.reddit.com` is a different problem and stays in tier 4.** It returns a
JavaScript challenge rather than a page, and the User-Agent above does not
change that — see `docs/roadmap-to-any-page.md`, whose Phase A is about getting
through that door.

### 3. google.com — search results before the homepage

The results page is lighter markup than it looks. Both pages run real script,
Google varies its markup by user agent, and in practice it requires HTTP/2.
"Type a query and read the results" is a plausible target; the full experience
is not.

### 4. Plex — a single-page application *and* media playback

Two hard problems at once. Nothing here is reachable before the JavaScript
engine is real and the media stack exists.

### 5. youtube.com — the hardest, and worth stating why

**Surveyed 2026-08-04**, by the method this ADR recommends: load it, look at what breaks, write it
down. The result was the same lesson Hacker News taught — what it needed was not what was on the
list. Fourteen scripts failed, and after the engine bugs behind them were fixed (`var` scope and
hoisting, top-level `this`, the `for-in` grammar, escaped identifiers, the parse depth bound) the
whole 10.7MB application bundle parses and every remaining failure is a missing *binding* rather
than a missing language feature.

The survey turned into five ADRs, which between them are the plan this section used to gesture at:
**0010** (transport — the page costs 15 TLS handshakes and 5x the bytes it should), **0011**
(asynchronous loading, which is the structural blocker), **0012** (the binding surface, and the rule
that a stub is worse than an absence), **0013** (media and the codec dependency), and **0014** (CSS,
where the measurement says custom properties matter roughly a hundred times more than grid).


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
- ~~**Classes**, including fields, private names, `super`, and accessors.~~
  Done. Private names are ordinary properties under their `#` name, which is
  not real privacy and is the right amount until something observes the
  difference.
- ~~**Promises and a microtask queue**~~, then **async/await**. The queue is
  done, and it turned out not to change the host loop at all: a microtask
  exists only because something already ran, so the drain rides a wakeup that
  was already happening and the zero-idle-CPU invariant is untouched. It is
  drained at the end of a turn and bounded, so an endless `.then` chain costs
  promptness rather than the window.

  `await` is a different problem and is *not* done. A promise only ever
  schedules a call, which a tree-walker can do; suspending a function in the
  middle needs its stack to be data rather than C++ frames. That waits on the
  bytecode VM, along with generators.
- ~~**Iterators**, which `for...of` is defined in terms of.~~ Done, with
  symbols under them: `for...of`, spread, rest and array destructuring all run
  the protocol, and `Map` and `Set` publish it. **Generators** are not, and
  wait on the VM for the same reason `await` does.
- ~~**A regular expression engine.**~~ Done: a backtracking matcher over a
  compiled program, bounded three ways because both the pattern and the
  subject are attacker-controlled. Byte-oriented like the rest of the string
  implementation, which is exact for ASCII and an approximation above it --
  making the unit a code point is the same change as making a JS string
  UTF-16.
- ~~**`Symbol`**~~ and ~~getters and setters~~ are done. **`Proxy`,
  `Reflect`, `WeakMap`** and the rest of the surface a framework's own runtime
  uses are not.
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
