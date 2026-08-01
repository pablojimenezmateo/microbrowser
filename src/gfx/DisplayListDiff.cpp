#include "gfx/DisplayListDiff.h"

#include <algorithm>
#include <variant>

#include "util/PerformanceCounters.h"

namespace microbrowser::gfx {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsClip(const DisplayCommand& command) {
  return std::holds_alternative<PushClipCommand>(command) ||
         std::holds_alternative<PopClipCommand>(command);
}

// Two paths paint the same when their geometry is the same. Compared through
// the list rather than by index, for the reason in the header.
bool SamePath(const DisplayList& list_a, std::uint32_t a, const DisplayList& list_b,
              std::uint32_t b) {
  const Path* first = list_a.PathAt(a);
  const Path* second = list_b.PathAt(b);
  if (first == nullptr || second == nullptr) {
    // A command naming a path that is not there paints nothing, but two of them
    // are not therefore "the same": treat it as a difference so the region is
    // repainted rather than assumed clean.
    return false;
  }
  return *first == *second;
}

}  // namespace

IntRect CommandBounds(const DisplayList& list, const DisplayCommand& command) {
  // One list of one command, so that the bounds logic lives in exactly one
  // place -- DisplayList::Bounds -- and cannot drift from what damage assumes.
  DisplayList single;
  if (const auto* fill = std::get_if<FillRectCommand>(&command)) {
    single.FillRect(fill->rect, fill->color);
  } else if (const auto* fill_path = std::get_if<FillPathCommand>(&command)) {
    if (const Path* geometry = list.PathAt(fill_path->path)) {
      single.FillPath(*geometry, fill_path->color, fill_path->rule);
    }
  } else if (const auto* stroke = std::get_if<StrokePathCommand>(&command)) {
    if (const Path* geometry = list.PathAt(stroke->path)) {
      single.StrokePath(*geometry, stroke->style, stroke->color);
    }
  } else if (const auto* text = std::get_if<DrawTextCommand>(&command)) {
    const DisplayList::TextRun* run = list.TextAt(text->text);
    const FontRequest* font = list.FontAt(text->font);
    if (run != nullptr && font != nullptr) {
      single.DrawText(run->text, run->advance, *font, text->origin, text->color);
    }
  }
  return single.Bounds();
}

bool CommandsPaintTheSame(const DisplayList& list_a, const DisplayCommand& a,
                          const DisplayList& list_b, const DisplayCommand& b) {
  if (a.index() != b.index()) {
    return false;
  }
  if (const auto* fill_path = std::get_if<FillPathCommand>(&a)) {
    const auto& other = std::get<FillPathCommand>(b);
    return fill_path->color == other.color && fill_path->rule == other.rule &&
           SamePath(list_a, fill_path->path, list_b, other.path);
  }
  if (const auto* stroke = std::get_if<StrokePathCommand>(&a)) {
    const auto& other = std::get<StrokePathCommand>(b);
    return stroke->color == other.color && stroke->style == other.style &&
           SamePath(list_a, stroke->path, list_b, other.path);
  }
  if (const auto* text = std::get_if<DrawTextCommand>(&a)) {
    const auto& other = std::get<DrawTextCommand>(b);
    const DisplayList::TextRun* run_a = list_a.TextAt(text->text);
    const DisplayList::TextRun* run_b = list_b.TextAt(other.text);
    const FontRequest* font_a = list_a.FontAt(text->font);
    const FontRequest* font_b = list_b.FontAt(other.font);
    if (run_a == nullptr || run_b == nullptr || font_a == nullptr || font_b == nullptr) {
      return false;
    }
    return text->color == other.color && text->origin == other.origin && *run_a == *run_b &&
           *font_a == *font_b;
  }
  // FillRect, PushClip and PopClip carry no indices, so their own equality is
  // the whole answer.
  return a == b;
}

bool ComputeDamage(const DisplayList& previous, const DisplayList& current, const IntRect& full,
                   DirtyRegion& out) {
  out.Clear();
  AddPerformanceCounter(PerfCounterId::DamageDiffs);

  const std::vector<DisplayCommand>& before = previous.Commands();
  const std::vector<DisplayCommand>& after = current.Commands();

  // Longest common prefix and suffix. Everything between them is the change.
  // An insertion or a deletion shifts the tail, and matching from both ends is
  // what keeps that from marking the whole list dirty.
  std::size_t prefix = 0;
  while (prefix < before.size() && prefix < after.size() &&
         CommandsPaintTheSame(previous, before[prefix], current, after[prefix])) {
    ++prefix;
  }
  std::size_t suffix = 0;
  while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
         CommandsPaintTheSame(previous, before[before.size() - 1 - suffix], current,
                              after[after.size() - 1 - suffix])) {
    ++suffix;
  }

  if (prefix == before.size() && prefix == after.size()) {
    AddPerformanceCounter(PerfCounterId::DamageDiffsIdentical);
    return true;  // nothing changed, and nothing needs repainting
  }

  // A clip is state that every later command reads, so an identical command
  // after a changed clip draws somewhere else entirely. Rather than model the
  // clip stack -- which is the point at which this stops being a diff and
  // starts being a second renderer -- give up and repaint everything.
  const auto clip_in_range = [](const std::vector<DisplayCommand>& commands, std::size_t from,
                                std::size_t to) {
    return std::any_of(commands.begin() + static_cast<std::ptrdiff_t>(from),
                       commands.begin() + static_cast<std::ptrdiff_t>(to), IsClip);
  };
  if (clip_in_range(before, prefix, before.size() - suffix) ||
      clip_in_range(after, prefix, after.size() - suffix)) {
    AddPerformanceCounter(PerfCounterId::DamageDiffsFullRepaint);
    out.Add(full);
    return false;
  }

  for (std::size_t i = prefix; i < before.size() - suffix; ++i) {
    out.Add(CommandBounds(previous, before[i]).Intersected(full));
  }
  for (std::size_t i = prefix; i < after.size() - suffix; ++i) {
    out.Add(CommandBounds(current, after[i]).Intersected(full));
  }
  AddPerformanceCounter(PerfCounterId::DamageRectsProduced,
                        static_cast<std::uint64_t>(out.Count()));
  return true;
}

}  // namespace microbrowser::gfx
