#!/usr/bin/env python3
"""Generates src/text/IdnaTables.inc and src/text/NormalizationTables.inc.

ADR 0025 §1's rule, applied to a second pair of tables: **generated, never hand-copied**, and
checked in, because a build that downloads from unicode.org is a build that fails when the network
does and whose output depends on when it ran.

Separate from generate.py, for the reason generate_encodings.py is separate: it reads a different
set of UCD files, and a generator that needs seven files to regenerate two of them is one nobody
can run.

    tools/unicode/generate_idna.py <ucd-directory> src/text <unicode-version>

The <ucd-directory> must hold, at the same Unicode version:

    curl -O https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt
    curl -O https://www.unicode.org/Public/17.0.0/ucd/CompositionExclusions.txt
    curl -O https://www.unicode.org/Public/17.0.0/ucd/extracted/DerivedJoiningType.txt  # into extracted/
    curl -O https://www.unicode.org/Public/17.0.0/idna/IdnaMappingTable.txt

The version is an argument because **none of these four files states one in a parseable header** --
UnicodeData.txt has no header at all -- and a generated table that does not record which Unicode
produced it is one nobody can re-derive. It is 17.0.0 here while the tables generate.py writes are
15.1.0, and that is deliberate rather than drift: 17.0.0 is what the web-platform-tests vectors are
generated against, and the difference between versions is the difference between reaching a host
and refusing it. Four code points found by running those vectors: U+04C0 CYRILLIC LETTER PALOCHKA
and U+2183 ROMAN NUMERAL REVERSED ONE HUNDRED became *mapped* rather than disallowed in 16.0,
U+180E MONGOLIAN VOWEL SEPARATOR became ignored, and CJK Extension J (U+323B0..U+3347F) went from
reserved to valid in 17.0. The two version sets do not meet: nothing here reads a line-break class
and nothing there reads an IDNA status.

Why these four:

  IdnaMappingTable.txt   UTS #46's per-code-point disposition. This is the whole of "what does
                         `Ｇｏ.com` mean" -- fullwidth letters are *mapped*, a soft hyphen is
                         *ignored*, and getting either wrong means connecting to a different host
                         than the one the user was shown.
  UnicodeData.txt        Canonical decompositions and combining classes (NFC), and General_Category
                         (a label may not begin with a combining mark).
  CompositionExclusions  The composition pairs that must not recompose. Without it NFC is not NFC.
  DerivedJoiningType     ContextJ: whether a ZWNJ in a label is legitimate Persian orthography or
                         an invisible character smuggled into a domain name.
"""

import sys
from pathlib import Path

# --- UCD readers -------------------------------------------------------------


def unicode_data(path):
    """UnicodeData.txt as {code: (category, ccc, decomposition_or_None)}.

    The file's `First>`/`Last>` range convention is honoured: a CJK block is two lines, and a
    reader that took them literally would give 20,992 ideographs no category at all.
    """
    records = {}
    pending_first = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split(";")
        if len(fields) < 6:
            continue
        code = int(fields[0], 16)
        name, category, ccc, decomposition = fields[1], fields[2], int(fields[3]), fields[5]
        canonical = None
        if decomposition and not decomposition.startswith("<"):
            canonical = [int(part, 16) for part in decomposition.split()]
        if name.endswith(", First>"):
            pending_first = (code, category, ccc)
            continue
        if name.endswith(", Last>") and pending_first is not None:
            first, first_category, first_ccc = pending_first
            for filled in range(first, code + 1):
                records[filled] = (first_category, first_ccc, None)
            pending_first = None
            continue
        records[code] = (category, ccc, canonical)
    return records


def composition_exclusions(path):
    excluded = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            excluded.add(int(line.split()[0], 16))
    return excluded


def joining_types(path):
    """DerivedJoiningType.txt as merged (first, last, type) ranges."""
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        codes, _, value = (part.strip() for part in line.partition(";"))
        if not value:
            continue
        if ".." in codes:
            first, last = (int(part, 16) for part in codes.split(".."))
        else:
            first = last = int(codes, 16)
        entries.append((first, last, value))
    entries.sort()
    return merge(entries)


def idna_mapping(path):
    """IdnaMappingTable.txt as merged (first, last, status, mapping) rows.

    A mapped range maps as a whole -- `0132..0133 ; mapped ; 0069 006A` is two code points with one
    replacement -- so a range and a sequence is the shape of the data rather than a compression of
    it.
    """
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [part.strip() for part in line.split(";")]
        codes, status = fields[0], fields[1]
        mapping = ()
        if len(fields) > 2 and fields[2]:
            mapping = tuple(int(part, 16) for part in fields[2].split())
        if ".." in codes:
            first, last = (int(part, 16) for part in codes.split(".."))
        else:
            first = last = int(codes, 16)
        entries.append((first, last, (status, mapping)))
    entries.sort()
    return merge(entries)


def merge(entries):
    """Merges adjacent runs carrying the same value. The UCD splits them by block."""
    merged = []
    for first, last, value in entries:
        if merged and merged[-1][2] == value and merged[-1][1] + 1 == first:
            merged[-1] = (merged[-1][0], last, value)
        else:
            merged.append((first, last, value))
    return merged





# --- Derivations -------------------------------------------------------------

HANGUL_S_BASE, HANGUL_L_COUNT, HANGUL_V_COUNT, HANGUL_T_COUNT = 0xAC00, 19, 21, 28


def full_decompositions(records):
    """Canonical decomposition, applied to a fixed point.

    Stored fully expanded rather than pairwise: a runtime that expanded recursively would have to
    guard against a cycle in data it did not generate, and this loop is the only place that
    question has to be answered.
    """
    resolved = {}

    def expand(code):
        if code in resolved:
            return resolved[code]
        record = records.get(code)
        if record is None or record[2] is None:
            return [code]
        out = []
        for part in record[2]:
            out.extend(expand(part))
        resolved[code] = out
        return out

    return {code: expand(code) for code, record in records.items() if record[2] is not None}


def composition_pairs(records, excluded):
    """(starter, second) -> composed, for canonical composition.

    Three exclusions, all of them load-bearing: the script-specific exclusion list, singletons (a
    one-code-point decomposition never recomposes), and any code point whose own combining class is
    non-zero -- composing onto a non-starter is how NFC stops being idempotent.
    """
    pairs = {}
    for code, (_, ccc, decomposition) in records.items():
        if decomposition is None or len(decomposition) != 2:
            continue
        if code in excluded or ccc != 0:
            continue
        if records.get(decomposition[0], ("", 0, None))[1] != 0:
            continue
        pairs[(decomposition[0], decomposition[1])] = code
    return pairs


# --- Emit --------------------------------------------------------------------

IDNA_STATUS = {
    "valid": "Valid",
    "ignored": "Ignored",
    "mapped": "Mapped",
    "deviation": "Deviation",
    "disallowed": "Disallowed",
    "disallowed_STD3_valid": "DisallowedStd3Valid",
    "disallowed_STD3_mapped": "DisallowedStd3Mapped",
}

JOINING_TYPE = {"C": "C", "D": "D", "L": "L", "R": "R", "T": "T", "U": "U"}

if len(sys.argv) != 4:
    sys.exit(__doc__)
ucd = Path(sys.argv[1])
out_dir = Path(sys.argv[2])
version = sys.argv[3]

records = unicode_data(ucd / "UnicodeData.txt")
excluded = composition_exclusions(ucd / "CompositionExclusions.txt")
joining = joining_types(ucd / "extracted" / "DerivedJoiningType.txt")
idna = idna_mapping(ucd / "IdnaMappingTable.txt")

# Combining classes and marks, as merged ranges. Both are overwhelmingly contiguous.
ccc_rows = merge(sorted((code, code, ccc) for code, (_, ccc, _) in records.items() if ccc != 0))
mark_rows = merge(
    sorted((code, code, True) for code, (category, _, _) in records.items() if category[0] == "M")
)
decompositions = full_decompositions(records)
compositions = composition_pairs(records, excluded)

# One flat pool of code points behind both the IDNA mappings and the decompositions, so a row is an
# offset and a length rather than a pointer into a per-row allocation.
mapping_pool = []
mapping_index = {}


def intern(sequence):
    key = tuple(sequence)
    if key not in mapping_index:
        mapping_index[key] = len(mapping_pool)
        mapping_pool.extend(key)
    return mapping_index[key], len(key)


idna_rows = []
for first, last, (status, mapping) in idna:
    offset, length = intern(mapping) if mapping else (0, 0)
    idna_rows.append((first, last, IDNA_STATUS[status], offset, length))

decomposition_pool = []
decomposition_rows = []
for code in sorted(decompositions):
    sequence = decompositions[code]
    decomposition_rows.append((code, len(decomposition_pool), len(sequence)))
    decomposition_pool.extend(sequence)

out = (out_dir / "IdnaTables.inc").open("w", encoding="utf-8")
out.write("// Generated by tools/unicode/generate_idna.py from the Unicode Character Database.\n")
out.write("// DO NOT EDIT. Regenerate instead, and diff the result.\n")
out.write("//\n")
out.write("// IdnaMappingTable.txt and DerivedJoiningType.txt, Unicode %s.\n" % version)
out.write("//\n")
out.write("// %d disposition ranges over %d mapped code points, and %d joining-type ranges.\n"
          % (len(idna_rows), len(mapping_pool), len(joining)))
out.write("// The ranges are sorted and non-overlapping, which is what lets the lookup be a binary\n")
out.write("// search -- and what a test asserts, because a generator bug here is a silent one.\n\n")

out.write("constexpr IdnaRange kIdnaRanges[] = {\n")
for first, last, status, offset, length in idna_rows:
    out.write("    {0x%X, 0x%X, IdnaStatus::%s, %d, %d},\n" % (first, last, status, offset, length))
out.write("};\n\n")

out.write("// Every replacement sequence, end to end. A row above names a span of this.\n")
out.write("constexpr std::uint32_t kIdnaMappingPool[] = {\n")
for index in range(0, len(mapping_pool), 12):
    out.write("    " + " ".join("0x%X," % code for code in mapping_pool[index:index + 12]) + "\n")
out.write("};\n\n")

out.write("// Joining_Type, for UTS #46's ContextJ rule: whether a ZERO WIDTH NON-JOINER in a label\n")
out.write("// is Persian orthography or an invisible character smuggled into a host name.\n")
out.write("constexpr JoiningTypeRange kJoiningTypeRanges[] = {\n")
for first, last, value in joining:
    out.write("    {0x%X, 0x%X, JoiningType::%s},\n" % (first, last, JOINING_TYPE[value]))
out.write("};\n")
out.close()

out = (out_dir / "NormalizationTables.inc").open("w", encoding="utf-8")
out.write("// Generated by tools/unicode/generate_idna.py from the Unicode Character Database.\n")
out.write("// DO NOT EDIT. Regenerate instead, and diff the result.\n")
out.write("//\n")
out.write("// Canonical decomposition and composition (UAX #15, NFC), from UnicodeData.txt and\n")
out.write("// CompositionExclusions.txt, Unicode %s.\n" % version)
out.write("//\n")
out.write("// The decompositions are stored **fully expanded**: a runtime that recursed would have\n")
out.write("// to guard against a cycle in data it did not generate, and the generator is the one\n")
out.write("// place that question can be answered once.\n")
out.write("//\n")
out.write("// %d decompositions over %d code points, %d composition pairs, %d combining-class\n"
          % (len(decomposition_rows), len(decomposition_pool), len(compositions), len(ccc_rows)))
out.write("// ranges and %d mark ranges. Hangul is not in any of them -- it is arithmetic.\n"
          % len(mark_rows))
out.write("\nconstexpr DecompositionRow kDecompositions[] = {\n")
for code, offset, length in decomposition_rows:
    out.write("    {0x%X, %d, %d},\n" % (code, offset, length))
out.write("};\n\n")

out.write("constexpr std::uint32_t kDecompositionPool[] = {\n")
for index in range(0, len(decomposition_pool), 12):
    out.write("    " + " ".join("0x%X," % code for code in decomposition_pool[index:index + 12])
              + "\n")
out.write("};\n\n")

out.write("// Sorted by (starter, second) so a composition is a binary search rather than a map.\n")
out.write("constexpr CompositionRow kCompositions[] = {\n")
for (starter, second), composed in sorted(compositions.items()):
    out.write("    {0x%X, 0x%X, 0x%X},\n" % (starter, second, composed))
out.write("};\n\n")

out.write("constexpr CombiningClassRange kCombiningClasses[] = {\n")
for first, last, ccc in ccc_rows:
    out.write("    {0x%X, 0x%X, %d},\n" % (first, last, ccc))
out.write("};\n\n")

out.write("// General_Category=M. A label may not begin with a combining mark, which is the rule\n")
out.write("// that stops a host name from starting with something that paints onto what precedes it.\n")
out.write("constexpr CodePointRange kMarks[] = {\n")
for first, last, _ in mark_rows:
    out.write("    {0x%X, 0x%X},\n" % (first, last))
out.write("};\n")
out.close()
