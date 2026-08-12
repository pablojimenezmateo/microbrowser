#!/usr/bin/env python3
"""Generates src/html/EncodingIndexes.inc from the WHATWG Encoding Standard's index files.

ADR 0025 §2, session 32. The same argument as tools/unicode/generate.py: these are 78,000 mappings
across four files, a transcription error in any of them is a character that decodes to the wrong
character with no way to notice, and a generated table is diffable against the version before it.

Flat arrays indexed by *pointer*, not sorted ranges, because these indexes are 70-100% dense -- a
range table would be almost one row per entry and a lookup would be a binary search where an array
index does. 0xFFFF marks a hole, which is what the Encoding Standard calls a null pointer.

    tools/unicode/generate_encodings.py <index-directory> > src/html/EncodingIndexes.inc

The index files are not vendored, for the reason the UCD files are not: they are large and versioned
upstream. Fetch them with:

    for f in index-jis0208 index-jis0212 index-euc-kr index-big5 index-gb18030 \
             index-gb18030-ranges index-iso-2022-jp-katakana; do
      curl -O https://encoding.spec.whatwg.org/$f.txt
    done

**The encoder tables are generated here too, and they are not the decoder tables read backwards.**
Every one of the standard's "index pointer for code point" operations has its own exclusions and its
own tie-break, and each exists because the index maps two pointers to one code point:

  * `index Shift_JIS pointer` drops pointers 8272-8835 *before* taking the first match, so a
    duplicated code point encodes to the later one.
  * `index Big5 pointer` drops every pointer below (0xA1-0x81)x157 -- the Hong Kong supplement, which
    must not be produced literally -- and takes the **last** match for six code points rather than
    the first.
  * every other index takes the first match.

Inverting a decode table without those rules produces bytes that decode back to the right character
and are still not what any other browser sends, which is a wrong answer no round-trip test can see.
"""

import re
import sys
from pathlib import Path

# Distinct per width, because 0xFFFF is a legitimate code point in a 32-bit table (U+FFFF is a
# noncharacter, but the standard's indexes are free to be sparse in ways this must not mistake).
HOLE = 0xFFFF
WIDE_HOLE = 0xFFFFFFFF


def read_index(path):
    """`pointer  0xCODE  # comment` lines, as a flat list with holes."""
    entries = {}
    identifier = "unknown"
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            match = re.search(r"Identifier: ([0-9a-f]{16})", line)
            if match:
                identifier = match.group(1)
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        pointer = int(parts[0])
        code = int(parts[1], 16)
        entries[pointer] = code
    size = max(entries) + 1
    values = [entries.get(i, HOLE) for i in range(size)]
    # **Big5 maps above the BMP** -- pointer 947 is U+27267, a supplementary CJK ideograph -- so the
    # element type is chosen from the data rather than assumed to be 16 bits. This was written as an
    # assertion first ("none of these indexes has one") and the assertion fired, which is the whole
    # reason to write one: a uint16 table would have truncated that character to U+7267 silently, and
    # `曦` for `𧉧` is a wrong character rather than a missing one.
    wide = any(value != HOLE and value > 0xFFFF for value in values)
    return values, identifier, wide


def read_pairs(path):
    """The same file read as ordered (pointer, code point) pairs, holes and all."""
    pairs = []
    identifier = "unknown"
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            match = re.search(r"Identifier: ([0-9a-f]{16})", line)
            if match:
                identifier = match.group(1)
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        pairs.append((int(parts[0]), int(parts[1], 16)))
    return pairs, identifier


def encode_table(values, *, exclude=lambda pointer: False, last_for=()):
    """The standard's "index pointer for code point", as a sorted (code point, pointer) table.

    `exclude` removes entries before the search, which is what makes the Shift_JIS and Big5 variants
    different tables rather than the same one; `last_for` is Big5's six code points that take the
    last surviving pointer instead of the first.
    """
    first = {}
    last = {}
    for pointer, code in enumerate(values):
        if code == HOLE or exclude(pointer):
            continue
        first.setdefault(code, pointer)
        last[code] = pointer
    for code in last_for:
        if code in last:
            first[code] = last[code]
    return sorted(first.items())


def write_array(out, symbol, values, bits, formatter):
    out.write("constexpr std::uint%d_t %s[] = {\n" % (bits, symbol))
    line = "   "
    for value in values:
        line += " " + formatter(value) + ","
        if len(line) > 96:
            out.write(line + "\n")
            line = "   "
    if line.strip():
        out.write(line + "\n")
    out.write("};\n\n")


if len(sys.argv) != 2:
    sys.exit(__doc__)
directory = Path(sys.argv[1])

out = sys.stdout
out.write("// Generated by tools/unicode/generate_encodings.py from the WHATWG Encoding Standard's\n")
out.write("// index files. DO NOT EDIT. Regenerate instead, and diff the result.\n//\n")

DECODE = (
    ("index-jis0208", "kJis0208"),
    ("index-jis0212", "kJis0212"),
    ("index-euc-kr", "kEucKr"),
    ("index-big5", "kBig5"),
    ("index-gb18030", "kGb18030"),
    ("index-iso-2022-jp-katakana", "kIso2022JpKatakana"),
)

tables = []
loaded = {}
for name, symbol in DECODE:
    values, identifier, wide = read_index(directory / (name + ".txt"))
    loaded[name] = values
    tables.append((symbol, values, name, identifier, wide))
    out.write("// %s: %d entries, identifier %s%s\n"
              % (name, len(values), identifier, ", above the BMP" if wide else ""))

ranges, ranges_identifier = read_pairs(directory / "index-gb18030-ranges.txt")
out.write("// index-gb18030-ranges: %d ranges, identifier %s\n" % (len(ranges), ranges_identifier))

out.write("//\n// A hole -- 0xFFFF in a 16-bit table, 0xFFFFFFFF in a 32-bit one -- is what the standard\n")
out.write("// calls a null pointer: that pointer maps to no character, and a decoder must produce\n")
out.write("// U+FFFD rather than index past it.\n\n")

for symbol, values, name, _, wide in tables:
    out.write("// %s. %s\n" % (name, "32 bits: this index maps above the BMP."
                               if wide else "16 bits is enough for every entry."))
    if wide:
        write_array(out, symbol, values, 32,
                    lambda v: "0x%X" % (WIDE_HOLE if v == HOLE else v))
    else:
        write_array(out, symbol, values, 16, lambda v: "0x%04X" % v)

# index gb18030 ranges. Two parallel arrays rather than a struct: the pointer array is searched and
# the code point array is read, and a search over an array of pairs would stride past a field it
# never looks at. Both are ascending in both columns, which is what makes either one searchable.
out.write("// index-gb18030-ranges, as the standard's two columns. `index gb18030 ranges pointer`\n"
          "// searches the code points and `index gb18030 ranges code point` searches the pointers;\n"
          "// both are ascending, which is the property that makes one table serve both directions.\n")
write_array(out, "kGb18030RangePointers", [p for p, _ in ranges], 32, lambda v: "%d" % v)
write_array(out, "kGb18030RangeCodePoints", [c for _, c in ranges], 32, lambda v: "0x%X" % v)

# The encoder tables. Sorted by code point, so a lookup is a binary search over the first array and a
# read at the same subscript in the second -- and the *pointer* is what the standard's arithmetic
# needs, not the bytes, because each encoder divides it differently.
BIG5_LAST = (0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345)
BIG5_FLOOR = (0xA1 - 0x81) * 157

ENCODE = (
    ("kJis0208Encode", encode_table(loaded["index-jis0208"]),
     "index jis0208, first pointer. EUC-JP and ISO-2022-JP both encode through it."),
    ("kShiftJisEncode",
     encode_table(loaded["index-jis0208"], exclude=lambda p: 8272 <= p <= 8835),
     "index Shift_JIS pointer: jis0208 with pointers 8272-8835 removed *before* the first match,\n"
     "// so a code point the index lists twice encodes to the later of the two."),
    ("kEucKrEncode", encode_table(loaded["index-euc-kr"]), "index euc-kr, first pointer."),
    ("kBig5Encode",
     encode_table(loaded["index-big5"], exclude=lambda p: p < BIG5_FLOOR, last_for=BIG5_LAST),
     "index Big5 pointer: the Hong Kong supplement (every pointer below %d) is dropped so it is\n"
     "// never produced literally, and six code points take the *last* surviving pointer." % BIG5_FLOOR),
    ("kGb18030Encode", encode_table(loaded["index-gb18030"]), "index gb18030, first pointer."),
)

for symbol, pairs, note in ENCODE:
    wide = any(code > 0xFFFF for code, _ in pairs)
    out.write("// %s: %d code points.\n// %s\n" % (symbol, len(pairs), note))
    write_array(out, symbol + "Codes", [c for c, _ in pairs], 32 if wide else 16,
                (lambda v: "0x%X" % v) if wide else (lambda v: "0x%04X" % v))
    write_array(out, symbol + "Pointers", [p for _, p in pairs], 16, lambda v: "%d" % v)
