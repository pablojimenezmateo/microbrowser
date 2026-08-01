# Architecture Guide

How this codebase is structured, and the mechanisms that keep it that way.

## Quick Scan

- Modules are layered. A module may only depend on modules below it, declared in `MODULE.deps`.
- A module's public surface is declared. Anything unlisted is private and unreachable from outside.
- Classes have declared budgets. Growth past them is a test failure; raising them is a visible edit.
- The UI/Engine seam is a serialized message protocol, so a process split stays a scheduling
  decision rather than a rewrite.
- Nothing upstream of pixels calls a drawing function. It produces a display list.

## The Layer Stack

```
src/app/        main loop, idle-wait policy, dirty-region policy      ── UI side of the seam
src/ui/         tabs, omnibox, chrome, settings, privacy dashboard        (M7)
src/ipc/        typed serializable messages + swappable transport     ── THE SEAM
src/engine/     per-tab engine: navigation, lifecycle, event loop     ── Engine side
  src/html/     tokenizer, tree construction (WHATWG-literal)             (M3)
  src/dom/      Node/Element/Document, ranges, events, bindings           (M3)
  src/css/      tokenizer, parser, selectors, cascade, computed style     (M4)
  src/layout/   box tree, BFC/IFC, line breaking, flex, grid              (M5)
  src/paint/    display list building, stacking contexts, compositing     (M6)
  src/js/       lexer, parser, bytecode, interpreter, GC, builtins        (M8)
src/net/        URL, DNS, sockets, TLS, HTTP/1.1, pool, cookies, cache    (M2)
src/privacy/    filter engine, URL sanitizer, HTTPS-only, partition keys  (M2)
src/gfx/        Canvas, Path, Rasterizer, Blit, GlyphAtlas, Font, Color
src/platform/   SDL window/input/present, clipboard, app dirs, fs
src/util/       parse, strings, tasks, tracing, counters
```

## The Module Contract

### The manifest

Every directory under `src/` has a `MODULE.deps`:

```
purpose: Software rasterization. Pixels in memory; nothing about windows or the web.

allow: util

public: Geometry.h Color.h Canvas.h DirtyRegion.h DisplayList.h

extern:

max_tu_lines: 800

budget: Canvas       header_lines=80 public_methods=20 members=4
budget: DirtyRegion  header_lines=60 public_methods=10 members=1
```

Comments start with `#`. Blank lines are ignored. Every field is required except `allow:` and
`extern:`, which may be empty — and an empty `extern:` is not an omission, it is the assertion that
this module touches no third-party code.

### What each field does

**`purpose:`** — one line justifying the module's existence. Required, and not ceremony: a
directory nobody had to justify creating is how a codebase grows a `common/` and then a `misc/`.
When you are unsure where a file goes, the purpose lines are the answer.

**`allow:`** — Chromium's `DEPS` `include_rules`. Transitive dependencies are **not** implied:
`engine` allowing `ipc` does not let it include `gfx` just because `ipc` does. Every edge is stated.

**`public:`** — Gecko's `moz.build` `EXPORTS`. A module's advertised surface. A header not on this
list is private to the module: other modules cannot include it, and the lint says so by name. This
is what lets a module have internal structure without that structure becoming everyone's problem.

**`extern:`** — the sanctioned third-party groups this module may include (`SDL3`, `freetype`,
`harfbuzz`, `openssl`, `zlib`, `brotli`, `stb`). This one field replaces a whole family of rules.
"gfx must not include SDL" is not written anywhere; it follows from `gfx` declaring no externs.
"SDL lives only in platform" follows from `platform` being the only module that declares `SDL3`.

**`max_tu_lines:`** — per-file cap. A file over its cap means a missing module, not a bigger file.

**`budget:`** — per-class limits on header lines, public methods, and data members.

### CMake mirrors it

`CMakeLists.txt` declares one static library per module with the same dependency edges
(Ladybird's model), so the build graph and the manifests tell the same story, and modules compile in
parallel. The manifests are the authority; the lint is the enforcement. The link edges exist so the
structure is visible in the build, not as a second enforcement mechanism — the final executable
links everything, so a bad include would still resolve at link time. Do not rely on the linker to
catch a layering violation.

## Why Budgets

A class does not become a god object in one commit. It grows three lines at a time, and every
individual step looks reasonable — a helper here, a cached field there, one more thing the shell
happens to know about. By the time it is obviously wrong, no single commit is to blame and nobody
can point at the moment it went wrong.

Chromium, Gecko, and Ladybird all attack the *coupling* half of this with declared dependencies.
None of them mechanically bounds *class size*; they rely on review culture and owner files, which
works when you have hundreds of reviewers and does not when you have one.

A declared budget converts slow drift into a discrete event. When `Application` needs a thirteenth
member, the build fails, and the fix is either "this belongs somewhere else" or an edit to
`MODULE.deps` that a reviewer sees in the diff. Both outcomes are fine. What is not fine is the
third option, which is what happens without the budget: nobody notices.

Two details make it work rather than merely exist:

- **Un-budgeted large classes fail too.** A class over 25 header lines with no budget entry is a
  violation. Otherwise the rule only constrains classes someone already thought about, and a brand
  new god object sails straight past it.
- **`tools/budget-report.sh` shows headroom**, sorted by how close each budget is to its limit. The
  lint tells you what already broke; the report tells you what is about to, which is the more useful
  moment to look.

Budgets are not sacred numbers. They are a forcing function for a conversation.

## Separation of Concerns

The layering above is the coarse version. These are the specific separations that matter most, each
of which was a deliberate choice rather than an accident of where code landed.

**Policy is separate from mechanism, and policy is pure.** `gfx::DirtyRegion` accumulates damage;
`app::DirtyRegionPolicy` decides whether that damage is worth a partial repaint. `platform` waits
for events; `app::IdleWaitStrategy` decides *how* to wait. In both cases the policy half is a pure
function over a small struct, so it is unit-tested exhaustively without a window, a GPU, or a clock.
That is why "idle CPU is zero" can be a test rather than a hope.

**Pixels are separate from windows.** `gfx` produces pixels into memory and has no idea what a
window is. `platform` gets them onto a screen and has no idea what a browser is. This is enforced
(`gfx` declares no externs) and it buys deterministic reference tests: the same input produces the
same bytes everywhere, so a golden file means something.

**Drawing is separate from deciding what to draw.** Everything upstream of pixels produces a
`gfx::DisplayList`; nothing upstream of pixels calls a drawing function. That single rule buys four
things that are very hard to retrofit: paint is testable without a window, damage is computable by
diffing lists rather than trusting every call site to invalidate correctly, paint can cross a
process boundary because the list serializes, and layout code cannot smuggle in device state
because the command vocabulary is closed.

**The engine is separate from the UI.** They share only `src/ipc`. The UI cannot name a `dom::`,
`css::`, `layout::`, or `js::` type; the engine cannot name a window, a texture, or an SDL anything.
This is what keeps sandboxing the engine a scheduling decision — and once it is sandboxed, this
boundary stops being an architectural preference and becomes *the* trust boundary, with the engine
side assumed to be running the attacker's code. `guidelines/security.md` and
`docs/adr/0004-process-model-and-site-isolation.md` cover what that implies for anything crossing it.

**Third-party code is separate from everything.** Each library is named by exactly one module and
never crosses a boundary as a type. FreeType will be an implementation detail of `gfx::GlyphAtlas`,
not a type in a layout signature.

### How this gets violated, and how it gets caught

The violation is almost never dramatic. It is: *"I just need the window size here."* Then a UI type
appears in an engine signature, and six months later the process split is impossible.

Writing the lint caught exactly this in M0. `src/app` was including `<SDL3/SDL.h>` directly to call
`SDL_WaitEvent`, which is completely reasonable-looking and entirely wrong: it put the window system
in the layer that is supposed to be testable without one. Event waiting moved into
`platform::SdlWindow`, and `app` now declares no externs, so it can never come back.

## The IPC Seam

```
UI    --> Engine :  Navigate, Reload, StopLoad, ResizeViewport, Scroll, Pointer
Engine --> UI   :  PaintFrame, TitleChanged, LoadProgress, NavigationCommitted
```

Every message serializes, and `IpcMessageTests` round-trips all of them on every build — even though
M0 delivers them through an in-process queue that never encodes anything.

That is deliberate. A message that cannot be serialized is a message that quietly holds a pointer,
and the round-trip test catches it on the commit that introduces it rather than a year later when
the process split is attempted. The wire format is also written to a hostile-input standard already:
bounds-checked reads, a sticky failure flag, length prefixes validated against the bytes that
actually remain, trailing bytes rejected rather than ignored. Doing that now costs nothing; doing it
after the seam carries renderer output is a security retrofit.

`InProcessTransport` moves messages. `SocketTransport` will write frames over a `socketpair`.
Nothing above the `Transport` interface changes.

Adding a message is therefore a security review, not just an API change: the payload arrives from a
process that is assumed compromised. Which field is a length, a path, an index, or an origin — and
what happens when each one lies? Site identity in particular is never a message field; the browser
process knows it from having created the connection.

## Adding a Module

1. Create `src/<name>/` with a `MODULE.deps`. Write the `purpose:` line first — if it is hard to
   write in one sentence, the module boundary is wrong.
2. Declare the narrowest `allow:` that works. Add edges when you actually need them, not
   pre-emptively.
3. Start `public:` with one header. Publishing is easy later; un-publishing is not.
4. Add the module to `CMakeLists.txt` with matching link dependencies.
5. Budget classes as you write them, at roughly their natural size — not at the size you hope they
   stay, and not with generous headroom that defeats the point.
