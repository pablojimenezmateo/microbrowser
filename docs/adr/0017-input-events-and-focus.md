# ADR 0017 — Input, the event model, and focus

**Status:** accepted · **Date:** 2026-08-04

## Context

The whole of what a page can learn about the user's hands, today, is three IPC messages:

```cpp
struct PointerMessage   { enum class Kind { Move, Down, Up }; Kind kind; gfx::IntPoint position; std::uint8_t button; };
struct TextInputMessage { std::string text; };
struct InputCommandMessage { enum class Command { Backspace, Delete, Enter }; Command command; };
```

No modifiers. No key identity — `TextInputMessage` carries the *text a key produced*, which is the
right thing for typing into the omnibox and carries no way to know that Escape was pressed. No
focus, beyond "which form control the engine last hit-tested into". `PointerMessage` reaches the
page as a click that dispatches `click` and bubbles, and that is genuinely all.

The survey says what the target sites want instead:

| | occurrences across 16.2MB of script |
|---|---|
| `addEventListener` | **930** |
| `CustomEvent` | 293 |
| `focus()` | 232 |
| `keydown` | 169 |
| `pointerdown` / `pointermove` / `pointerup` | 90 |
| `touchstart` | 74 |
| `visibilitychange` | 34 |
| `contentEditable` | 11 |

930 listener registrations is the shape of an application whose entire behaviour is event handlers.
And the reddit measurement is sharper than any count: **reddit.com's front door will not open
without `DOMContentLoaded` and a programmatic form submission.** The interstitial in
`docs/surveys/2026-08-04-reddit-youtube-plex.md` registers exactly one listener, and if it never
fires there is no reddit.

There is also a security shape here that is easy to get wrong and expensive to fix later. An event a
page constructs and dispatches must never be mistaken for one the user caused. `src/bindings`
already gets this right — events a page makes are untrusted by construction — and this ADR is where
that property either survives contact with a real input pipeline or quietly dies, because the
pipeline is where trusted and untrusted events meet in one queue.

## Decision

### 1. The IPC input surface becomes what an input event actually is

`PointerMessage` and `TextInputMessage` are replaced, not extended:

```
struct PointerInputMessage {
  enum class Kind : std::uint8_t { Move, Down, Up, Enter, Leave, Wheel, Cancel };
  Kind kind;
  gfx::PointF position;         // CSS pixels, viewport-relative
  std::int32_t pointer_id;      // a mouse is one; touches are many
  enum class Type : std::uint8_t { Mouse, Pen, Touch } type;
  std::uint16_t buttons;        // bitmask of what is held, not what changed
  std::uint8_t  button;         // what changed
  Modifiers modifiers;
  gfx::Vec2F  wheel_delta;
};

struct KeyInputMessage {
  enum class Kind : std::uint8_t { Down, Up };
  Kind kind;
  std::string code;             // physical key: "KeyA", "Escape" — layout-independent
  std::string key;              // what it means: "a", "A", "Escape" — layout-dependent
  std::string text;             // what it inserts, possibly empty
  Modifiers modifiers;
  bool repeat;
};
```

Splitting `code` from `key` from `text` is not spec pedantry. A game reads `code` because WASD is a
shape on the keyboard; a shortcut reads `key` because Ctrl+C is a letter; an editor reads `text`
because a dead key produces nothing until the next one. Collapsing them — which is what
`TextInputMessage` does today — makes two of the three unimplementable, and 169 `keydown` sites will
find that out.

**`Modifiers` is a struct with named bools, not an integer.** It crosses the IPC seam from a process
that will eventually be the untrusted one, and a bitmask whose meaning is a convention is exactly
the kind of field ADR 0004 says to treat as a claim rather than a fact.

Composition (`compositionstart` / `compositionupdate` / `compositionend`) is named here and deferred
with a reason: it needs the platform IME, which is `src/platform`'s to expose, and no target site
requires it. It is written down so that adding CJK or accented input later is an extension of this
message set rather than a discovery that the set was wrong.

### 2. Dispatch is the specification's algorithm, in full, once

The current dispatch is "find the target, run its listeners, walk to the parent". The real algorithm
is a **capture phase down the ancestor chain, an at-target phase, and a bubble phase up**, with
`stopPropagation`, `stopImmediatePropagation`, `preventDefault`, `passive`, `once`, and a listener
list ordered by insertion.

It is implemented once, in `src/bindings`, and every event goes through it — `click`, `keydown`, a
`CustomEvent` a page dispatched, a `MutationObserver`'s delivery is not an event and stays a
microtask. Two events with two dispatch paths is how a browser ends up with a `preventDefault` that
works on links and not on forms.

**The default action is a separate step that runs after dispatch and only if nothing cancelled it.**
That is already true for link navigation and it is the property to preserve: `preventDefault` on a
`submit` must stop the submission, on a `keydown` must stop the character being inserted, and on a
`wheel` must stop the scroll — the last of which is why `passive` listeners exist and why a passive
listener's `preventDefault` is ignored rather than honoured.

### 3. `isTrusted` is set by where the event came from, and there is no way to set it

An event created by `new Event()` or `document.createEvent` has `isTrusted === false` and cannot be
made true. An event the engine synthesises from a `PointerInputMessage` or a `KeyInputMessage` has
it true. The flag is set at construction from the constructor that was used, not assigned afterwards
and not settable through any binding.

This matters more than it looks. `element.click()` dispatches an untrusted click that still performs
the default action — that is the specification, and it is fine. What is not fine is a page being
able to forge an event that the *browser* treats as user intent, and the places that will eventually
check for user intent (opening a window, entering fullscreen, reading the clipboard, starting audio)
are all things ADR 0029 gates. Those gates read `isTrusted`, so its integrity is a security property
of this ADR and not a detail of that one.

### 4. Focus is a document property, and the engine owns it

One focused element per document; `document.activeElement` reports it; `HTMLElement.focus()` and
`.blur()` move it; Tab moves it in tree order over focusable elements, honouring `tabindex`.

Moving focus fires `blur` and `focusout` on the old, `focus` and `focusin` on the new, in that
order, and sets the `:focus` / `:focus-within` state bits that ADR 0016 defined. `:focus-visible`
follows the heuristic every browser converged on — set for keyboard-driven focus, not for a mouse
click on a button — because a focus ring that appears on every click is the reason authors write
`outline: none`, and that is worse for the user than either behaviour.

**Focus is also the input router.** A `KeyInputMessage` goes to the focused element, or to the body
if nothing is focused, and hit-testing is only consulted for pointer events. That is the split that
makes a text field work without a second mechanism.

The browser chrome's own focus (`src/ui`'s omnibox) is not this focus. `src/ui` has no dom and no
layout — the chrome is not a page — so the application decides whether a key belongs to the chrome
or the page *before* it becomes a `KeyInputMessage`. Keeping that decision in `src/app` is what
stops a page from being able to type into the address bar, which is a phishing primitive rather than
a bug.

### 5. Hit testing gets the box, and the box gets the event

Today hit testing finds links, form controls and event targets by walking for specific things. It
becomes one operation: **given a viewport point, return the deepest box whose border box contains
it, respecting overflow clipping, then map that box to its node.** `pointer-events: none` skips a
box; a box with no node (an anonymous box) resolves to its parent's node.

That is a layout query, so it goes through ADR 0015's `GeometrySource` seam by the same rule: a node
handle out, never a box pointer.

### 6. Touch, and the honest absence of it

`touchstart` appears 74 times and every one of those sites is behind a feature detect. **Touch events
are not implemented and `ontouchstart` is not defined**, which is ADR 0012's rule: a page that
detects touch and gets `undefined` takes the mouse path, which works. Pointer events carry a `type`
of `Touch` for the day a touchscreen exists, so the message set does not have to change then.

## Consequences

- **`src/ipc`'s message set changes shape, and every consumer changes with it.** `AGENTS.md` is
  explicit that compatibility is not a default constraint and that a broad refactor is preferred to
  preserving a stale boundary; this is that case. `TextInputMessage` and `InputCommandMessage` are
  deleted rather than kept alongside, because two input paths means the chrome's and the page's
  diverge.
- **`src/app` becomes the place that decides chrome-or-page for every key.** That is a real
  responsibility landing in the main loop, and it is a security boundary, so it gets tests that
  assert a page never receives a key aimed at the omnibox and vice versa.
- **The dispatch algorithm is a lot of small rules and each one is silently wrong until tested.**
  Capture-phase ordering, `stopImmediatePropagation` versus `stopPropagation`, `once` removal timing,
  a listener added during dispatch. These are cheap tests and they are the ones that decay.
- **Interaction becomes measurable as latency.** A key press that runs a listener that mutates the
  DOM that forces layout (ADR 0015) that repaints is the full pipeline, and it is the number the
  user actually feels. It deserves a scope from the day it exists.
- **This is what makes `:hover` real.** ADR 0016 defined the state bits and left who sets them here;
  `Enter`/`Leave` on `PointerInputMessage` is the answer, and the two ADRs are only useful together.

## Alternatives considered

**Extend `PointerMessage` and `TextInputMessage` in place.** Rejected. The existing messages encode a
model — one pointer, no modifiers, text without key identity — and every field added to them is a
field that has to be optional because the old senders do not fill it. A message set with optional
semantics is one that cannot be validated at the seam, which is the one thing ADR 0003 built the
seam to do.

**Send raw platform key codes and decode them in the engine.** Rejected on the process model. Key
decoding needs the keyboard layout, which is platform state; putting it in the engine means the
process that will be sandboxed and untrusted is the one holding the layout table, and it means every
platform's quirks live in the module that is supposed to know nothing about platforms.

**Implement `keypress` as well, since old code uses it.** Rejected. It is deprecated, its behaviour
differs between engines, and implementing it is how a browser inherits twenty years of
compatibility hacks. `keydown` plus `beforeinput`/`input` covers what it was for.

**Do focus later — the target sites are mostly read-only.** Rejected on the measurement: `focus()`
is called 232 times, and reddit's front door needs a form to submit. It is also the thing that makes
a browser usable by someone who does not use a mouse, and retrofitting a focus model under an
already-shipped event system means revisiting every dispatch site.
