#pragma once

#include <cstddef>
#include <string_view>

#include "gfx/Path.h"

namespace microbrowser::gfx {

// SVG path data: the `d` attribute's grammar, turned into a Path.
//
// Its own header because it is the one piece of SVG that is a real parser
// rather than attribute plumbing, and because it is worth testing on its own.
// Every quirk of the grammar that a hand-written implementation gets wrong the
// first time is a quirk a page relies on:
//
//   * commands repeat without being repeated (`L 1 1 2 2` is two linetos), and
//     a repeated `M` continues as `L`, which is what closes most icon outlines;
//   * numbers need no separator when the sign or the decimal point supplies one
//     (`1-2`, `.5.5`), and `1e-3` is one number rather than a subtraction;
//   * `S` and `T` reflect the previous control point, and reflect *nothing*
//     when the previous command was not the matching curve type;
//   * `Z` returns the current point to the subpath's start, so a following
//     relative command is relative to there and not to where the pen was.
//
// Returns false when the data is malformed. The path still holds everything
// parsed up to that point, because a truncated `d` should draw what it managed
// -- which is what every renderer does, and what a page with one typo needs.
// `max_commands` bounds the work: path data is attacker-controlled and a
// megabyte of `l1 1` is a megabyte of segments.
bool ParseSvgPathData(std::string_view data, Path& out, std::size_t max_commands);

}  // namespace microbrowser::gfx
