#pragma once

#include "gfx/DirtyRegion.h"
#include "gfx/DisplayList.h"

namespace microbrowser::gfx {

// What changed between two frames.
//
// This is the reason the display list exists as a comparable value rather than
// as a sequence of calls: damage can be *computed* from two frames instead of
// trusted from every call site that mutated something. A browser that asks its
// layout code to report what it invalidated is a browser with a permanent
// supply of stale-pixel bugs, because the one place that forgot is invisible.
//
// The result is a conservative superset: every pixel whose final colour differs
// is covered, and some that do not may be too. Over-reporting costs redundant
// fills; under-reporting leaves stale content on screen, so the rounding goes
// one way only.
//
// The argument for correctness is per pixel: the colour of a pixel depends only
// on the ordered subsequence of commands that touch it. If every command that
// touches p is identical in both lists and in the same relative order, p cannot
// have changed. So marking the commands that differ is sufficient -- with one
// exception, which is why clips are handled separately below.
//
// Returns false when the lists differ in a way this cannot bound, in which case
// `out` is left holding `full` and the caller must repaint everything. That
// happens when a clip changes: a clip is state that every later command reads,
// so an identical command after a changed clip draws somewhere else.
bool ComputeDamage(const DisplayList& previous, const DisplayList& current, const IntRect& full,
                   DirtyRegion& out);

// True when two commands would paint identically, resolving any side-table
// indices.
//
// Not operator==: a FillPathCommand holds an *index* into its own list's path
// table, and two commands with the same index in different lists can name
// entirely different geometry. Comparing the commands alone would call a circle
// and a square equal, and the diff would silently skip repainting the change.
bool CommandsPaintTheSame(const DisplayList& list_a, const DisplayCommand& a,
                          const DisplayList& list_b, const DisplayCommand& b);

// Device-pixel bounds of one command, or an empty rect for a command that
// paints nothing (a clip push or pop).
IntRect CommandBounds(const DisplayList& list, const DisplayCommand& command);

}  // namespace microbrowser::gfx
