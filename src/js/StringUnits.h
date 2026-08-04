#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace microbrowser::js {

// UTF-16 indexing over UTF-8 storage.
//
// A JavaScript string is defined as a sequence of UTF-16 code units. `length`
// counts them, `s[i]` yields one, `charCodeAt` returns one, and every index a
// method takes or returns is measured in them. This engine stores strings as
// UTF-8, because that is what the network, the HTML parser, the CSS parser and
// the DOM all speak -- converting at each of those boundaries would be a copy
// per string per crossing, and the bindings seam is the wrong place to put a
// second string type.
//
// So the storage stays UTF-8 and the *indexing* is UTF-16. That is what this
// header is: the conversion between a code-unit index and a byte offset, in one
// place, so that `charCodeAt`, `slice` and a regular expression's match index
// cannot disagree about what position 3 means.
//
// **The ASCII fast path is the whole performance story.** For a string with no
// byte above 0x7F -- every HTML tag name, every CSS keyword, every property
// name, and most of what a page indexes into -- a code-unit index *is* a byte
// offset and every function below returns immediately. `IsAscii` is the test,
// and it is one word-at-a-time scan.
//
// What is left is the cost of indexing into a non-ASCII string, which is a
// walk from the start. A loop that reads `s[i]` for every `i` is quadratic in
// that case. That is a real cost and it is the price of not having a second
// representation; the note is here so the next person measures rather than
// wonders.
//
// Lone surrogates are representable, which they have to be: `charCodeAt` on an
// emoji returns a high surrogate, and `String.fromCharCode(high, low)` putting
// it back has to produce the emoji again. A surrogate with no partner is stored
// as its own three-byte sequence -- WTF-8 -- rather than being replaced, so the
// round trip is exact.

// Whether every byte is below 0x80, in which case a code-unit index is a byte
// offset and every conversion below is the identity.
bool IsAscii(std::string_view text);

// How many UTF-16 code units `text` encodes. This is `.length`.
std::size_t Utf16Length(std::string_view text);

// The byte offset where code unit `unit` begins, or `text.size()` past the end.
//
// A `unit` that lands on the low half of a surrogate pair answers the offset of
// the *pair*, because there is no byte boundary inside one -- callers that
// slice there get the whole character, which is what every engine does with an
// index into the middle of one.
std::size_t ByteOffsetOfUnit(std::string_view text, std::size_t unit);

// The inverse: how many code units precede byte offset `at`.
std::size_t UnitOffsetOfByte(std::string_view text, std::size_t at);

// The code unit at `unit`, or 0 past the end. `charCodeAt` with the range
// check already done.
std::uint16_t CodeUnitAt(std::string_view text, std::size_t unit);

// The whole code point beginning at `unit`, which is the pair when one starts
// there. `codePointAt`.
std::uint32_t CodePointAt(std::string_view text, std::size_t unit);

// `text` from code unit `begin` up to `end`, both already clamped.
std::string SubstringUnits(std::string_view text, std::size_t begin, std::size_t end);

// Appends one UTF-16 code unit.
//
// A high surrogate is held rather than written, in `pending`, so that the low
// one following it can be joined into a single four-byte sequence -- which is
// what makes `String.fromCharCode(0xD83D, 0xDE00)` one emoji rather than two
// broken halves. Call FlushCodeUnit at the end to write a high surrogate that
// never found its partner.
void AppendCodeUnit(std::string& out, std::uint16_t unit, std::uint32_t& pending);
void FlushCodeUnit(std::string& out, std::uint32_t& pending);

// Case conversion, over more than ASCII.
//
// `toUpperCase` folded only A-Z, so "héllo" became "HéLLO" -- which is right
// for the four ASCII letters and wrong for the one a page actually cared
// about. Full Unicode case mapping needs the tables from the standard, and
// those are a megabyte and a dependency; what is here is the algorithmic part
// of them, which covers Latin-1 Supplement, Latin Extended-A and B, Greek and
// Cyrillic. That is every alphabet a Western page's text is in, and the
// remainder is left alone rather than mangled.
std::string ToUpper(std::string_view text);
std::string ToLower(std::string_view text);

}  // namespace microbrowser::js
