# C++ Guide

C++20. GCC 13 and Clang 18 both build the tree warning-free under `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion
-Wformat=2`. Keep it that way; the presets turn warnings into errors.

## Ownership

- **RAII for everything.** No raw `new`/`delete`, no `malloc`/`free`. Linted
  (`NoManualHeapOwnership`). `SdlWindow` owns its window, renderer, and SDL's initialization as one
  unit, released in the destructor. Placement new and an eventual counting `operator new` are
  outside the rule, because neither takes ownership of anything.
- **Value semantics by default.** Prefer a plain struct passed by value or const reference over a
  polymorphic hierarchy. `gfx::IntRect` is 16 bytes and copies freely; making it a class with an
  interface would cost more than it could ever save.
- **Inheritance only for a durable polymorphic boundary.** `ipc::Transport` is one — there will
  genuinely be an in-process and a socket implementation. Most things are not, and a base class
  added "for testability" usually means the logic wanted extracting instead.
- **Pass dependencies in; do not reach out for them.** No singletons, no service locators, no
  global registries. `Engine` takes an `ipc::EngineEndpoint&`; it cannot acquire one another way.
- **Mutable state at namespace scope is banned** and linted. Function-local statics are fine — they
  initialize on first use and cannot be reached without calling the function that owns them.

## Types And Units

**Distinct types for distinct coordinate spaces.** `gfx::FloatRect` is CSS/layout space;
`gfx::IntRect` is device pixels. They do not implicitly convert, and `EnclosingIntRect` is the one
sanctioned crossing. This kills the "which space is this rect in?" bug class that plagues layout
code, and it is the reason a rounding decision has to be written down where it happens.

**Half-open ranges everywhere.** A rect covers `[x, x + width)`. Half-open arithmetic is total;
closed ranges need a zero-extent special case in every operation.

**Saturate rather than assume.** Layout arithmetic produces NaN and enormous floats routinely — a
percentage of an unresolved width, an overflowing zoom. `static_cast<int>` on either is undefined
behavior, so clamp first. Areas are computed in 64 bits because `width * height` in `int` is one
zoom level away from overflowing.

**Non-throwing parses.** `util/Parse.h`, never `std::sto*`. They throw *and* read the decimal
separator from `LC_NUMERIC`, which SDL's X11 and hidapi backends change behind our back by calling
`setlocale(LC_ALL, "")`. Under a comma-decimal locale every `1.5` in a stylesheet, a config file,
or an HTTP header would stop parsing at the `.`. Linted.

## Headers

- `#pragma once`. Linted.
- **Forward-declare third-party types rather than including their headers.** `SdlWindow.h`
  forward-declares `SDL_Window` and `SDL_Renderer`, so including it does not drag SDL into a
  translation unit that only wants a window size.
- Include what you use. The include order is: own header, C system, C++ standard, third-party,
  project.
- Keep headers minimal. A header is a compile-time cost paid by every one of its includers, and the
  class budgets already push in this direction.

## Hostile Input

A browser's entire input surface is hostile. Code that parses bytes from the network is held to a
different standard than code that parses a config file. The full threat model is in
`guidelines/security.md`; these are the implementation rules it produces:

- **The banned C functions are not available.** `strcpy`, `strcat`, `sprintf`, `strncpy`, `alloca`,
  `strtok`, `atoi`, `rand`, `mktemp`, `system`, and friends, linted by `NoBannedCFunctions` with the
  reason attached to each name. `strncpy` is on the list *because* it looks like the safe one: it
  does not terminate on truncation, so the bug it creates is a read past the end rather than a write.

- **Bounds-check every read; never trust a length prefix.** `ipc::ByteReader` validates a claimed
  length against the bytes that actually remain *before* allocating, so a frame claiming a 4 GiB
  string with four bytes left fails instead of attempting a 4 GiB reserve.
- **Sticky failure, total decoding.** Once a read fails, every later read fails, so a decoder runs
  straight through and checks once at the end. A malformed input yields `nullopt`, never a
  half-populated object.
- **Reject, do not ignore.** Trailing bytes after a decoded frame mean the two ends disagree about
  the payload shape; that surfaces as a decode failure rather than a silently dropped field.
- **Explicit tags, not variant indices.** A variant index shifts when someone inserts an
  alternative, silently reinterpreting every in-flight message. An explicit tag constant makes that
  a compile-time conflict.

## Performance-Sensitive Code

- **Write the obvious scalar version first and test it exhaustively.** `BlendSrcOver` is a
  four-multiply-per-pixel loop precisely so the SIMD blitters that replace it can be validated
  against something known correct, rather than against a second hand-derived formula.
- **Exact fixed-point, not approximations.** `MulDiv255` computes `round(x * a / 255)` exactly;
  a plain `>> 8` is off by up to one level and the error accumulates visibly over stacked
  translucent layers.
- **Fast-path the common case explicitly.** An opaque fill is a `std::fill` of a `uint32_t` span, not
  a blend loop that happens to be a no-op.
- **Reuse capacity in per-frame structures.** `DisplayList::Clear` calls `clear()`, not
  `= DisplayList{}` — display lists are rebuilt every frame, and reusing the capacity is what keeps
  painting off the allocator.
- **No string materialization in hot paths.** Compute text upstream, in the code that builds the
  view model or display list.

## Comments

Comment the *why*, never the *what*. The code says what it does.

Worth a comment: a non-obvious constraint (`SDL_BLENDMODE_NONE` because the default composites each
present against the previous one and darkens translucent pixels cumulatively); a decision someone
will want to reverse (why decoding replays through the builder instead of appending raw); a bug
class being defended against (why close-on-exec must be on the creating call).

Not worth a comment: `// increment the counter`.

When a comment records a trap, say what happens without the fix, not just that a fix is needed. The
next reader has to be able to tell whether the trap still applies.
