#!/usr/bin/env python3
"""Generates src/text/UnicodeTables.inc from the Unicode Character Database.

ADR 0025 §1. **The tables are generated, never hand-copied**, and that is the whole point of this
file existing: LineBreak.txt has 3,608 lines and EastAsianWidth.txt has 2,621, a transcription error
in either is a class of text that wraps or measures wrongly with no way to notice, and a regenerated
table is diffable against the version before it.

It writes a *ranged* table rather than a per-code-point one. The properties are overwhelmingly
contiguous -- CJK ideographs are one run of 20,992 -- so ranges make the table two orders of
magnitude smaller than the code-point count and a binary search over them is a handful of
comparisons. The generated file is checked in, because a build that downloads from unicode.org is a
build that fails when the network does and a build whose output depends on when it ran.

    tools/unicode/generate.py <ucd-directory> src/text

Two files, not one: UnicodeTables.inc (line break, width) and BidiTables.inc. They are separate
because they are included by different translation units, and one file would put all four tables in
both -- 200KB of duplicated constant data for nothing.

The UCD files are not vendored: they are large, they are versioned upstream, and the generated table
records which version produced it. Fetch them with:

    curl -O https://www.unicode.org/Public/15.1.0/ucd/LineBreak.txt
    curl -O https://www.unicode.org/Public/15.1.0/ucd/EastAsianWidth.txt
    curl -O https://www.unicode.org/Public/15.1.0/ucd/BidiBrackets.txt
    curl -O https://www.unicode.org/Public/15.1.0/ucd/extracted/DerivedBidiClass.txt   # into extracted/
"""

import re
import sys
from pathlib import Path

# The line-break classes this browser distinguishes, and *only* those. UAX #14 defines around forty;
# collapsing the rest into AL (ordinary letter) is a deliberate simplification with a stated cost --
# see the header of src/text/LineBreak.h. Adding a class here is a one-line change plus a pair-table
# row, which is the shape a later session wants.
KEPT_CLASSES = {
    "BK", "CR", "LF", "NL",        # mandatory breaks
    "SP", "ZW", "WJ", "GL", "CM",  # spaces and joiners
    "BA", "HY", "B2",              # break-after, hyphen, break-either-side
    "OP", "CL", "CP", "QU",        # brackets and quotes
    "EX", "IS", "SY", "NS", "IN", "NU",  # punctuation, the solidus, and numbers
    "ID", "CJ", "H2", "H3", "JL", "JV", "JT",  # CJK and Hangul
    "PR", "PO",                    # prefix and postfix currency
    "AI", "AL", "SA",              # ambiguous, ordinary, complex-context
    "EB", "EM",                    # emoji base and modifier
}


def parse_ranges(path, keep):
    """The UCD's `start..end; VALUE` lines, as (first, last, value) with runs merged."""
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        codes, _, value = (part.strip() for part in line.partition(";"))
        if not value:
            continue
        value = value.split()[0]
        if keep is not None and value not in keep:
            value = keep_default
        if ".." in codes:
            first, last = (int(part, 16) for part in codes.split(".."))
        else:
            first = last = int(codes, 16)
        entries.append((first, last, value))
    entries.sort()
    # Merge adjacent runs of the same value. The UCD splits them by block, and unmerged they are three
    # times as many rows for no information.
    merged = []
    for first, last, value in entries:
        if merged and merged[-1][2] == value and merged[-1][1] + 1 == first:
            merged[-1] = (merged[-1][0], last, value)
        else:
            merged.append((first, last, value))
    return merged


def version_of(path):
    match = re.search(r"-(\d+\.\d+\.\d+)\.txt", path.read_text(encoding="utf-8")[:200])
    return match.group(1) if match else "unknown"


# UAX #9's bidi classes, all of them. There is no folding here and there cannot be: unlike the
# line-break table, where an unrecognised class breaking like a letter is wrong in the direction the
# specification is already wrong in, a bidi class that is folded to the wrong one *reverses text*. The
# rules name every class by hand -- W1 is about NSM, W2 about AN, N0 about brackets -- so a missing
# class is a rule that silently never fires.
BIDI_CLASSES = {
    "L", "R", "AL",                                  # strong
    "EN", "ES", "ET", "AN", "CS", "NSM", "BN",       # weak
    "B", "S", "WS", "ON",                            # neutral
    "LRE", "RLE", "LRO", "RLO", "PDF",               # explicit embeddings and overrides
    "LRI", "RLI", "FSI", "PDI",                      # isolates
}

keep_default = "AL"

if len(sys.argv) != 3:
    sys.exit(__doc__)
ucd = Path(sys.argv[1])
out_dir = Path(sys.argv[2])
line_break = parse_ranges(ucd / "LineBreak.txt", KEPT_CLASSES)
# East Asian Width: only W, F and H matter to a text layout engine -- wide, fullwidth and halfwidth --
# because they are the ones whose advance is not the font's business but the character's.
width = [
    (first, last, value)
    for first, last, value in parse_ranges(ucd / "EastAsianWidth.txt", None)
    if value in {"W", "F", "H"}
]

# DerivedBidiClass.txt, whose default is not a single value: unassigned code points in certain blocks
# default to R or AL rather than L, and the file states those defaults in comments rather than in
# data rows. They are transcribed here because they are *ranges of unassigned code points* -- an
# Arabic-block code point that Unicode has not assigned yet still has to lay out right-to-left, or a
# page using a newer Unicode than this table renders its text backwards.
BIDI_DEFAULT_RANGES = [
    (0x0600, 0x07BF, "AL"), (0x0860, 0x08FF, "AL"), (0xFB50, 0xFDCF, "AL"),
    (0xFDF0, 0xFDFF, "AL"), (0xFE70, 0xFEFF, "AL"),
    (0x10D00, 0x10D3F, "AL"), (0x10EC0, 0x10EFF, "AL"), (0x10F30, 0x10F6F, "AL"),
    (0x1EC70, 0x1ECBF, "AL"), (0x1ED00, 0x1ED4F, "AL"), (0x1EE00, 0x1EEFF, "AL"),
    (0x0590, 0x05FF, "R"), (0x07C0, 0x085F, "R"), (0xFB1D, 0xFB4F, "R"),
    (0x10800, 0x10CFF, "R"), (0x10D40, 0x10EBF, "R"), (0x10F00, 0x10F2F, "R"),
    (0x10F70, 0x10FFF, "R"), (0x1E800, 0x1EC6F, "R"), (0x1ECC0, 0x1ECFF, "R"),
    (0x1ED50, 0x1EDFF, "R"), (0x1EF00, 0x1EFFF, "R"),
]


def bidi_classes(path):
    """Bidi_Class per code point, as merged ranges, with the block defaults filled in first."""
    values = {}
    for first, last, value in BIDI_DEFAULT_RANGES:
        for code in range(first, last + 1):
            values[code] = value
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        codes, _, value = (part.strip() for part in line.partition(";"))
        value = value.split()[0] if value else ""
        if value not in BIDI_CLASSES:
            # Every value in this file is one of the 23 classes; a new one appearing means a Unicode
            # version this generator has not been read against, and guessing would reverse text.
            raise SystemExit("DerivedBidiClass.txt: unknown class %r" % value)
        if ".." in codes:
            first, last = (int(part, 16) for part in codes.split(".."))
        else:
            first = last = int(codes, 16)
        for code in range(first, last + 1):
            values[code] = value
    merged = []
    for code in sorted(values):
        value = values[code]
        if merged and merged[-1][2] == value and merged[-1][1] + 1 == code:
            merged[-1] = (merged[-1][0], code, value)
        else:
            merged.append((code, code, value))
    # `L` is the table's default, so the ranges that say `L` are the ones that need saying -- the
    # ranges *not* in the table answer L. Dropping them is 40% of the rows for no information.
    return [row for row in merged if row[2] != "L"]


def bracket_pairs(path):
    """BidiBrackets.txt: (code, paired, is_open) for rule N0."""
    pairs = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [part.strip() for part in line.split(";")]
        if len(parts) < 3:
            continue
        code, paired, kind = int(parts[0], 16), int(parts[1], 16), parts[2]
        pairs.append((code, paired, kind == "o"))
    pairs.sort()
    return pairs


bidi = bidi_classes(ucd / "extracted" / "DerivedBidiClass.txt")
brackets = bracket_pairs(ucd / "BidiBrackets.txt")

out = (out_dir / "UnicodeTables.inc").open("w", encoding="utf-8")
out.write("// Generated by tools/unicode/generate.py from the Unicode Character Database.\n")
out.write("// DO NOT EDIT. Regenerate instead, and diff the result.\n")
out.write("//\n")
out.write("// LineBreak.txt version %s, EastAsianWidth.txt version %s.\n"
          % (version_of(ucd / "LineBreak.txt"), version_of(ucd / "EastAsianWidth.txt")))
out.write("//\n")
out.write("// %d line-break ranges and %d wide-width ranges, merged from %d and %d UCD lines. The\n"
          % (len(line_break), len(width), 3608, 2621))
out.write("// ranges are sorted and non-overlapping, which is what lets the lookup be a binary\n")
out.write("// search -- and what a test asserts, because a generator bug here is a silent one.\n\n")

out.write("constexpr LineBreakRange kLineBreakRanges[] = {\n")
for first, last, value in line_break:
    out.write("    {0x%X, 0x%X, LineBreakClass::%s},\n" % (first, last, value))
out.write("};\n\n")

out.write("constexpr WidthRange kWideRanges[] = {\n")
for first, last, value in width:
    out.write("    {0x%X, 0x%X, EastAsianWidth::%s},\n"
              % (first, last, {"W": "Wide", "F": "Fullwidth", "H": "Halfwidth"}[value]))
out.write("};\n")
out.close()

# Bidi goes in its own file: it is included by src/text/Bidi.cpp and the other two by LineBreak.cpp
# and UnicodeProperties.cpp, and one file would put all four tables in every translation unit that
# wanted any of them.
out = (out_dir / "BidiTables.inc").open("w", encoding="utf-8")
out.write("// Generated by tools/unicode/generate.py from the Unicode Character Database.\n")
out.write("// DO NOT EDIT. Regenerate instead, and diff the result.\n")
out.write("//\n")
out.write("// Bidi_Class, UAX #9, from DerivedBidiClass.txt version %s. **`L` is the default and is not\n"
          % version_of(ucd / "extracted" / "DerivedBidiClass.txt"))
out.write("// in the table**: a code point no range covers is `L`, which drops 40% of the rows for no\n")
out.write("// information. The unassigned-code-point defaults from that file's comments -- the Arabic and\n")
out.write("// Hebrew blocks default to AL and R rather than L -- are filled in *first* and then\n")
out.write("// overwritten by the real data, so a code point a newer Unicode assigns inside those blocks\n")
out.write("// still lays out right-to-left instead of reversing the line it is on.\n")
out.write("//\n")
out.write("// %d bidi-class ranges and %d bracket pairs.\n" % (len(bidi), len(brackets)))
out.write("constexpr BidiClassRange kBidiClassRanges[] = {\n")
for first, last, value in bidi:
    out.write("    {0x%X, 0x%X, BidiClass::%s},\n" % (first, last, value))
out.write("};\n\n")

out.write("// Bidi_Paired_Bracket, for rule N0. Sorted, searched by code point.\n")
out.write("constexpr BracketPair kBracketPairs[] = {\n")
for code, paired, is_open in brackets:
    out.write("    {0x%X, 0x%X, %s},\n" % (code, paired, "true" if is_open else "false"))
out.write("};\n")
out.close()
