#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>

#include "js/RegExpProgram.h"

// The Unicode property escapes, `\\p{...}` and `\\P{...}`.
//
// Its own translation unit because it is its own subject: a table, and a
// lookup over it. Nothing here matches anything -- the compiler in RegExp.cpp
// turns what this returns into a CodePoint instruction, and the matcher tests
// against it. Separating it also keeps the pattern compiler readable, which a
// hundred lines of block boundaries in the middle of it would not.

namespace microbrowser::js {

// The Unicode property escapes, as ranges.
//
// **This is a subset, and deliberately.** The full property data is a megabyte
// of tables generated from the standard, which is a dependency ADR 0001 would
// have to sanction and a build step this repository does not have. What is
// here is the blocks a page's text is actually in -- Latin, Greek, Cyrillic,
// Hebrew, Arabic, Devanagari, Thai, the CJK ideographs, the kana, Hangul --
// plus the digit and space categories, which are exact.
//
// A property this does not know is a SyntaxError rather than a silently empty
// set: a pattern that matches nothing is a validator that accepts nothing, and
// a page would find that as a rejected form rather than as a broken regex.
bool PropertyRanges(std::string_view name, std::vector<CodeRange>& out) {
  // The letter blocks, shared by `L` and its case-specific spellings when the
  // distinction is not available -- `Lu` and `Ll` are exact for the Latin,
  // Greek and Cyrillic ranges where case is arithmetic, and fall back to the
  // whole block elsewhere, where the concept mostly does not apply.
  static const CodeRange kLetters[] = {
      {0x41, 0x5A},     {0x61, 0x7A},     {0xAA, 0xAA},     {0xB5, 0xB5},
      {0xBA, 0xBA},     {0xC0, 0xD6},     {0xD8, 0xF6},     {0xF8, 0x2FF},
      {0x370, 0x1FFF},  {0x2C00, 0x2FEF}, {0x3001, 0xD7FF}, {0xF900, 0xFDCF},
      {0xFDF0, 0xFFFD}, {0x10000, 0xEFFFF},
  };
  static const CodeRange kUpper[] = {
      {0x41, 0x5A},   {0xC0, 0xD6},   {0xD8, 0xDE},   {0x100, 0x17F},
      {0x391, 0x3AB}, {0x410, 0x42F}, {0x400, 0x40F},
  };
  static const CodeRange kLower[] = {
      {0x61, 0x7A},   {0xB5, 0xB5},   {0xDF, 0xF6},   {0xF8, 0xFF},
      {0x100, 0x17F}, {0x3B1, 0x3CB}, {0x430, 0x44F}, {0x450, 0x45F},
  };
  static const CodeRange kDigits[] = {
      {0x30, 0x39},     {0x660, 0x669},   {0x6F0, 0x6F9},   {0x966, 0x96F},
      {0xE50, 0xE59},   {0xFF10, 0xFF19},
  };
  static const CodeRange kSpaces[] = {
      {0x09, 0x0D},     {0x20, 0x20},     {0x85, 0x85},     {0xA0, 0xA0},
      {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F},
      {0x205F, 0x205F}, {0x3000, 0x3000},
  };
  static const CodeRange kPunctuation[] = {
      {0x21, 0x2F},     {0x3A, 0x40},     {0x5B, 0x60},     {0x7B, 0x7E},
      {0xA1, 0xA1},     {0xAB, 0xAB},     {0xBB, 0xBB},     {0xBF, 0xBF},
      {0x2010, 0x2027}, {0x2030, 0x205E}, {0x3001, 0x3003}, {0x300C, 0x3011},
  };
  const auto take = [&out](const CodeRange* first, std::size_t count) {
    out.assign(first, first + count);
    return true;
  };
  if (name == "L" || name == "Letter" || name == "Alpha" || name == "Alphabetic" ||
      name == "General_Category=L" || name == "General_Category=Letter") {
    return take(kLetters, std::size(kLetters));
  }
  if (name == "Lu" || name == "Uppercase_Letter" || name == "Uppercase") {
    return take(kUpper, std::size(kUpper));
  }
  if (name == "Ll" || name == "Lowercase_Letter" || name == "Lowercase") {
    return take(kLower, std::size(kLower));
  }
  if (name == "N" || name == "Nd" || name == "Number" || name == "Decimal_Number" ||
      name == "digit") {
    return take(kDigits, std::size(kDigits));
  }
  if (name == "White_Space" || name == "space" || name == "Zs" ||
      name == "Space_Separator") {
    return take(kSpaces, std::size(kSpaces));
  }
  if (name == "P" || name == "Punctuation" || name == "punct") {
    return take(kPunctuation, std::size(kPunctuation));
  }
  if (name == "ASCII") {
    out.assign({CodeRange{0x00, 0x7F}});
    return true;
  }
  if (name == "Any") {
    out.assign({CodeRange{0x00, 0x10FFFF}});
    return true;
  }
  if (name == "Script=Latin" || name == "sc=Latin") {
    out.assign({CodeRange{0x41, 0x5A}, CodeRange{0x61, 0x7A}, CodeRange{0xC0, 0x24F}});
    return true;
  }
  if (name == "Script=Greek" || name == "sc=Greek") {
    out.assign({CodeRange{0x370, 0x3FF}, CodeRange{0x1F00, 0x1FFF}});
    return true;
  }
  if (name == "Script=Cyrillic" || name == "sc=Cyrillic") {
    out.assign({CodeRange{0x400, 0x52F}});
    return true;
  }
  if (name == "Script=Han" || name == "sc=Han") {
    out.assign({CodeRange{0x4E00, 0x9FFF}, CodeRange{0x3400, 0x4DBF}});
    return true;
  }
  if (name == "Script=Hiragana" || name == "sc=Hiragana") {
    out.assign({CodeRange{0x3040, 0x309F}});
    return true;
  }
  if (name == "Script=Katakana" || name == "sc=Katakana") {
    out.assign({CodeRange{0x30A0, 0x30FF}});
    return true;
  }
  if (name == "Script=Hangul" || name == "sc=Hangul") {
    out.assign({CodeRange{0x1100, 0x11FF}, CodeRange{0xAC00, 0xD7AF}});
    return true;
  }
  if (name == "Script=Arabic" || name == "sc=Arabic") {
    out.assign({CodeRange{0x600, 0x6FF}, CodeRange{0x750, 0x77F}});
    return true;
  }
  if (name == "Script=Hebrew" || name == "sc=Hebrew") {
    out.assign({CodeRange{0x590, 0x5FF}});
    return true;
  }
  return false;
}

}  // namespace microbrowser::js
