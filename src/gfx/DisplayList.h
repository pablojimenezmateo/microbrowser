#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "gfx/Color.h"
#include "gfx/Geometry.h"
#include "gfx/Path.h"
#include "gfx/Rasterizer.h"
#include "gfx/Stroker.h"

namespace microbrowser::gfx {

class Painter;

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

// Path commands name their geometry by index into the list's own path table
// rather than carrying it inline. A command is copied and compared per frame; a
// path is a pair of heap vectors, and putting one inside the variant would make
// every command in the list as large as the largest path command and turn a
// list copy into a deep copy of every shape on the page.
//
// The index is an internal representation detail and never appears on the IPC
// wire — see ipc/Message.cpp, which serializes the geometry inline and replays
// it through the builder. An index that crossed a trust boundary would be an
// out-of-bounds read waiting for a hostile renderer to name a path that is not
// there.
struct FillPathCommand {
  std::uint32_t path = 0;
  FillRule rule = FillRule::NonZero;
  Color color;

  friend bool operator==(const FillPathCommand&, const FillPathCommand&) = default;
};

struct StrokePathCommand {
  std::uint32_t path = 0;
  Color color;
  StrokeStyle style;

  friend bool operator==(const StrokePathCommand&, const StrokePathCommand&) = default;
};

using DisplayCommand = std::variant<FillRectCommand, PushClipCommand, PopClipCommand,
                                    FillPathCommand, StrokePathCommand>;

class DisplayList {
 public:
  void Clear();
  bool IsEmpty() const { return commands_.empty(); }
  std::size_t Size() const { return commands_.size(); }
  const std::vector<DisplayCommand>& Commands() const { return commands_; }
  const std::vector<Path>& Paths() const { return paths_; }

  // Null for an index no path command produced. The builder cannot emit one,
  // but a DisplayList is a value type that a caller can assemble field by
  // field, and Execute indexes a vector with whatever it finds — so the range
  // check lives with the data rather than at each of its call sites.
  const Path* PathAt(std::uint32_t index) const {
    return index < paths_.size() ? &paths_[index] : nullptr;
  }

  void FillRect(const IntRect& rect, Color color);
  void PushClip(const IntRect& rect);
  void PopClip();
  void FillPath(const Path& path, Color color, FillRule rule = FillRule::NonZero);
  void StrokePath(const Path& path, const StrokeStyle& style, Color color);

  // Union of the device pixels this list can touch. Used to seed damage when
  // there is no previous list to diff against.
  IntRect Bounds() const;

  // Equality is structural, which is what makes "did this frame change?" a
  // cheap question rather than a heuristic.
  friend bool operator==(const DisplayList&, const DisplayList&) = default;

 private:
  std::vector<DisplayCommand> commands_;
  std::vector<Path> paths_;
};

// Execute `list` through `painter`, restricted to `damage`. Commands whose
// bounds fall entirely outside `damage` are skipped, which is what makes a
// partial repaint cheaper than a full one rather than merely narrower.
//
// A Painter rather than a Canvas because the painter owns the rasterizer and
// stroker arenas: executing a list is the hot path, and a Canvas-only signature
// would force a fresh arena per frame.
//
// The canvas clip stack is left as it was found even if the list is unbalanced
// (a malformed list must not corrupt the next frame).
void Execute(const DisplayList& list, Painter& painter, const IntRect& damage);

// A command is copied into a vector per paint; keeping it small keeps a
// complex page's display list in cache. See docs/adr/0002-growth-budgets.md.
static_assert(sizeof(DisplayCommand) <= 24, "DisplayCommand must stay small");

}  // namespace microbrowser::gfx
