#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace microbrowser::gfx {

class Canvas;

// An immutable, comparable, serializable recording of what to paint.
//
// Everything upstream of pixels produces one of these; nothing upstream of
// pixels calls a drawing function directly. That single rule buys four things
// that are otherwise very hard to retrofit:
//
//   1. Paint is testable without a window — build a list, assert on it.
//   2. Damage is computable — diff this frame's list against the last one
//      instead of trusting every call site to invalidate correctly.
//   3. Paint can cross a process boundary — the list serializes, so the future
//      sandboxed WebContent process needs no new abstraction.
//   4. Layout/style code cannot smuggle in device state, because the command
//      vocabulary is closed and lives here.
//
// The vocabulary grows one command at a time, with a reference test per
// command. It is intentionally small at M0.
struct FillRectCommand {
  IntRect rect;
  Color color;

  friend bool operator==(const FillRectCommand&, const FillRectCommand&) = default;
};

// Clips are a matched push/pop pair rather than a per-command clip rect: a
// per-command rect would be re-intersected for every command in a subtree, and
// a stack is what nested stacking contexts actually are.
struct PushClipCommand {
  IntRect rect;

  friend bool operator==(const PushClipCommand&, const PushClipCommand&) = default;
};

struct PopClipCommand {
  friend bool operator==(const PopClipCommand&, const PopClipCommand&) = default;
};

using DisplayCommand = std::variant<FillRectCommand, PushClipCommand, PopClipCommand>;

class DisplayList {
 public:
  void Clear();
  bool IsEmpty() const { return commands_.empty(); }
  std::size_t Size() const { return commands_.size(); }
  const std::vector<DisplayCommand>& Commands() const { return commands_; }

  void FillRect(const IntRect& rect, Color color);
  void PushClip(const IntRect& rect);
  void PopClip();

  // Union of the device pixels this list can touch. Used to seed damage when
  // there is no previous list to diff against.
  IntRect Bounds() const;

  // Equality is structural, which is what makes "did this frame change?" a
  // cheap question rather than a heuristic.
  friend bool operator==(const DisplayList&, const DisplayList&) = default;

 private:
  std::vector<DisplayCommand> commands_;
};

// Execute `list` into `canvas`, restricted to `damage`. Commands whose bounds
// fall entirely outside `damage` are skipped, which is what makes a partial
// repaint cheaper than a full one rather than merely narrower.
//
// The canvas clip stack is left as it was found even if the list is unbalanced
// (a malformed list must not corrupt the next frame).
void Execute(const DisplayList& list, Canvas& canvas, const IntRect& damage);

// A command is copied into a vector per paint; keeping it small keeps a
// complex page's display list in cache. See docs/adr/0002-growth-budgets.md.
static_assert(sizeof(DisplayCommand) <= 24, "DisplayCommand must stay small");

}  // namespace microbrowser::gfx
