#!/usr/bin/env python3
"""Generates src/html/SingleByteIndexes.inc from the WHATWG Encoding Standard's
single-byte index files, plus the label table.

ADR 0025 §2 said windows-1252 and ISO-8859-x. The Encoding Standard's remaining
single-byte encodings are the same shape -- 128 pointers, byte minus 0x80 -- and
a handwritten copy of any of them is a character that decodes wrong with no way
to notice. Same argument as generate_encodings.py.

    tools/unicode/generate_single_byte.py <index-directory> > src/html/SingleByteIndexes.inc

Fetch the indexes with:

    for f in ibm866 iso-8859-2 iso-8859-3 iso-8859-4 iso-8859-5 iso-8859-6 \\
             iso-8859-7 iso-8859-8 iso-8859-10 iso-8859-13 iso-8859-14 \\
             iso-8859-15 iso-8859-16 koi8-r koi8-u macintosh windows-874 \\
             windows-1250 windows-1251 windows-1252 windows-1253 windows-1254 \\
             windows-1255 windows-1256 windows-1257 windows-1258 x-mac-cyrillic; do
      curl -fsS -o index-$f.txt https://encoding.spec.whatwg.org/index-$f.txt
    done
"""

import re
import sys
from pathlib import Path

HOLE = 0xFFFF

# Encoding Standard encodings.json, label -> Encoding enumerator. `replacement`
# and `x-user-defined` have no index file and are handled in Encoding.cpp.
LABELS = [
    # UTF-8
    ("unicode-1-1-utf-8", "Utf8"),
    ("unicode11utf8", "Utf8"),
    ("unicode20utf8", "Utf8"),
    ("utf-8", "Utf8"),
    ("utf8", "Utf8"),
    ("x-unicode20utf8", "Utf8"),
    # IBM866
    ("866", "Ibm866"),
    ("cp866", "Ibm866"),
    ("csibm866", "Ibm866"),
    ("ibm866", "Ibm866"),
    # ISO-8859-2
    ("csisolatin2", "Iso8859_2"),
    ("iso-8859-2", "Iso8859_2"),
    ("iso-ir-101", "Iso8859_2"),
    ("iso8859-2", "Iso8859_2"),
    ("iso88592", "Iso8859_2"),
    ("iso_8859-2", "Iso8859_2"),
    ("iso_8859-2:1987", "Iso8859_2"),
    ("l2", "Iso8859_2"),
    ("latin2", "Iso8859_2"),
    # ISO-8859-3
    ("csisolatin3", "Iso8859_3"),
    ("iso-8859-3", "Iso8859_3"),
    ("iso-ir-109", "Iso8859_3"),
    ("iso8859-3", "Iso8859_3"),
    ("iso88593", "Iso8859_3"),
    ("iso_8859-3", "Iso8859_3"),
    ("iso_8859-3:1988", "Iso8859_3"),
    ("l3", "Iso8859_3"),
    ("latin3", "Iso8859_3"),
    # ISO-8859-4
    ("csisolatin4", "Iso8859_4"),
    ("iso-8859-4", "Iso8859_4"),
    ("iso-ir-110", "Iso8859_4"),
    ("iso8859-4", "Iso8859_4"),
    ("iso88594", "Iso8859_4"),
    ("iso_8859-4", "Iso8859_4"),
    ("iso_8859-4:1988", "Iso8859_4"),
    ("l4", "Iso8859_4"),
    ("latin4", "Iso8859_4"),
    # ISO-8859-5
    ("csisolatincyrillic", "Iso8859_5"),
    ("cyrillic", "Iso8859_5"),
    ("iso-8859-5", "Iso8859_5"),
    ("iso-ir-144", "Iso8859_5"),
    ("iso8859-5", "Iso8859_5"),
    ("iso88595", "Iso8859_5"),
    ("iso_8859-5", "Iso8859_5"),
    ("iso_8859-5:1988", "Iso8859_5"),
    # ISO-8859-6
    ("arabic", "Iso8859_6"),
    ("asmo-708", "Iso8859_6"),
    ("csiso88596e", "Iso8859_6"),
    ("csiso88596i", "Iso8859_6"),
    ("csisolatinarabic", "Iso8859_6"),
    ("ecma-114", "Iso8859_6"),
    ("iso-8859-6", "Iso8859_6"),
    ("iso-8859-6-e", "Iso8859_6"),
    ("iso-8859-6-i", "Iso8859_6"),
    ("iso-ir-127", "Iso8859_6"),
    ("iso8859-6", "Iso8859_6"),
    ("iso88596", "Iso8859_6"),
    ("iso_8859-6", "Iso8859_6"),
    ("iso_8859-6:1987", "Iso8859_6"),
    # ISO-8859-7
    ("csisolatingreek", "Iso8859_7"),
    ("ecma-118", "Iso8859_7"),
    ("elot_928", "Iso8859_7"),
    ("greek", "Iso8859_7"),
    ("greek8", "Iso8859_7"),
    ("iso-8859-7", "Iso8859_7"),
    ("iso-ir-126", "Iso8859_7"),
    ("iso8859-7", "Iso8859_7"),
    ("iso88597", "Iso8859_7"),
    ("iso_8859-7", "Iso8859_7"),
    ("iso_8859-7:1987", "Iso8859_7"),
    ("sun_eu_greek", "Iso8859_7"),
    # ISO-8859-8
    ("csiso88598e", "Iso8859_8"),
    ("csisolatinhebrew", "Iso8859_8"),
    ("hebrew", "Iso8859_8"),
    ("iso-8859-8", "Iso8859_8"),
    ("iso-8859-8-e", "Iso8859_8"),
    ("iso-ir-138", "Iso8859_8"),
    ("iso8859-8", "Iso8859_8"),
    ("iso88598", "Iso8859_8"),
    ("iso_8859-8", "Iso8859_8"),
    ("iso_8859-8:1988", "Iso8859_8"),
    ("visual", "Iso8859_8"),
    # ISO-8859-8-I
    ("csiso88598i", "Iso8859_8I"),
    ("iso-8859-8-i", "Iso8859_8I"),
    ("logical", "Iso8859_8I"),
    # ISO-8859-10
    ("csisolatin6", "Iso8859_10"),
    ("iso-8859-10", "Iso8859_10"),
    ("iso-ir-157", "Iso8859_10"),
    ("iso8859-10", "Iso8859_10"),
    ("iso885910", "Iso8859_10"),
    ("l6", "Iso8859_10"),
    ("latin6", "Iso8859_10"),
    # ISO-8859-13
    ("iso-8859-13", "Iso8859_13"),
    ("iso8859-13", "Iso8859_13"),
    ("iso885913", "Iso8859_13"),
    # ISO-8859-14
    ("iso-8859-14", "Iso8859_14"),
    ("iso8859-14", "Iso8859_14"),
    ("iso885914", "Iso8859_14"),
    # ISO-8859-15
    ("csisolatin9", "Iso8859_15"),
    ("iso-8859-15", "Iso8859_15"),
    ("iso8859-15", "Iso8859_15"),
    ("iso885915", "Iso8859_15"),
    ("iso_8859-15", "Iso8859_15"),
    ("l9", "Iso8859_15"),
    # ISO-8859-16
    ("iso-8859-16", "Iso8859_16"),
    # KOI8-R
    ("cskoi8r", "Koi8R"),
    ("koi", "Koi8R"),
    ("koi8", "Koi8R"),
    ("koi8-r", "Koi8R"),
    ("koi8_r", "Koi8R"),
    # KOI8-U
    ("koi8-ru", "Koi8U"),
    ("koi8-u", "Koi8U"),
    # macintosh
    ("csmacintosh", "Macintosh"),
    ("mac", "Macintosh"),
    ("macintosh", "Macintosh"),
    ("x-mac-roman", "Macintosh"),
    # windows-874
    ("dos-874", "Windows874"),
    ("iso-8859-11", "Windows874"),
    ("iso8859-11", "Windows874"),
    ("iso885911", "Windows874"),
    ("tis-620", "Windows874"),
    ("windows-874", "Windows874"),
    # windows-1250 .. 1258
    ("cp1250", "Windows1250"),
    ("windows-1250", "Windows1250"),
    ("x-cp1250", "Windows1250"),
    ("cp1251", "Windows1251"),
    ("windows-1251", "Windows1251"),
    ("x-cp1251", "Windows1251"),
    ("ansi_x3.4-1968", "Windows1252"),
    ("ascii", "Windows1252"),
    ("cp1252", "Windows1252"),
    ("cp819", "Windows1252"),
    ("csisolatin1", "Windows1252"),
    ("ibm819", "Windows1252"),
    ("iso-8859-1", "Windows1252"),
    ("iso-ir-100", "Windows1252"),
    ("iso8859-1", "Windows1252"),
    ("iso88591", "Windows1252"),
    ("iso_8859-1", "Windows1252"),
    ("iso_8859-1:1987", "Windows1252"),
    ("l1", "Windows1252"),
    ("latin1", "Windows1252"),
    ("us-ascii", "Windows1252"),
    ("windows-1252", "Windows1252"),
    ("x-cp1252", "Windows1252"),
    ("cp1253", "Windows1253"),
    ("windows-1253", "Windows1253"),
    ("x-cp1253", "Windows1253"),
    ("cp1254", "Iso8859_9"),
    ("csisolatin5", "Iso8859_9"),
    ("iso-8859-9", "Iso8859_9"),
    ("iso-ir-148", "Iso8859_9"),
    ("iso8859-9", "Iso8859_9"),
    ("iso88599", "Iso8859_9"),
    ("iso_8859-9", "Iso8859_9"),
    ("iso_8859-9:1989", "Iso8859_9"),
    ("l5", "Iso8859_9"),
    ("latin5", "Iso8859_9"),
    ("windows-1254", "Iso8859_9"),
    ("x-cp1254", "Iso8859_9"),
    ("cp1255", "Windows1255"),
    ("windows-1255", "Windows1255"),
    ("x-cp1255", "Windows1255"),
    ("cp1256", "Windows1256"),
    ("windows-1256", "Windows1256"),
    ("x-cp1256", "Windows1256"),
    ("cp1257", "Windows1257"),
    ("windows-1257", "Windows1257"),
    ("x-cp1257", "Windows1257"),
    ("cp1258", "Windows1258"),
    ("windows-1258", "Windows1258"),
    ("x-cp1258", "Windows1258"),
    ("x-mac-cyrillic", "XMacCyrillic"),
    ("x-mac-ukrainian", "XMacCyrillic"),
    # multi-byte (kept here so one lookup owns every label)
    ("chinese", "Gbk"),
    ("csgb2312", "Gbk"),
    ("csiso58gb231280", "Gbk"),
    ("gb2312", "Gbk"),
    ("gb_2312", "Gbk"),
    ("gb_2312-80", "Gbk"),
    ("gbk", "Gbk"),
    ("iso-ir-58", "Gbk"),
    ("x-gbk", "Gbk"),
    ("gb18030", "Gb18030"),
    ("big5", "Big5"),
    ("big5-hkscs", "Big5"),
    ("cn-big5", "Big5"),
    ("csbig5", "Big5"),
    ("x-x-big5", "Big5"),
    ("cseucpkdfmtjapanese", "EucJp"),
    ("euc-jp", "EucJp"),
    ("x-euc-jp", "EucJp"),
    ("csiso2022jp", "Iso2022Jp"),
    ("iso-2022-jp", "Iso2022Jp"),
    ("csshiftjis", "ShiftJis"),
    ("ms932", "ShiftJis"),
    ("ms_kanji", "ShiftJis"),
    ("shift-jis", "ShiftJis"),
    ("shift_jis", "ShiftJis"),
    ("sjis", "ShiftJis"),
    ("windows-31j", "ShiftJis"),
    ("x-sjis", "ShiftJis"),
    ("cseuckr", "EucKr"),
    ("csksc56011987", "EucKr"),
    ("euc-kr", "EucKr"),
    ("iso-ir-149", "EucKr"),
    ("korean", "EucKr"),
    ("ks_c_5601-1987", "EucKr"),
    ("ks_c_5601-1989", "EucKr"),
    ("ksc5601", "EucKr"),
    ("ksc_5601", "EucKr"),
    ("windows-949", "EucKr"),
    ("csiso2022kr", "Replacement"),
    ("hz-gb-2312", "Replacement"),
    ("iso-2022-cn", "Replacement"),
    ("iso-2022-cn-ext", "Replacement"),
    ("iso-2022-kr", "Replacement"),
    ("replacement", "Replacement"),
    ("unicodefffe", "Utf16Be"),
    ("utf-16be", "Utf16Be"),
    ("csunicode", "Utf16Le"),
    ("iso-10646-ucs-2", "Utf16Le"),
    ("ucs-2", "Utf16Le"),
    ("unicode", "Utf16Le"),
    ("unicodefeff", "Utf16Le"),
    ("utf-16", "Utf16Le"),
    ("utf-16le", "Utf16Le"),
    ("utf16", "Utf16Le"),
    ("utf16le", "Utf16Le"),
    ("utf16be", "Utf16Be"),
    ("x-user-defined", "XUserDefined"),
]

# Index file stem -> (C++ symbol, Encoding enumerator). Iso8859_8I shares iso-8859-8.
INDEXES = [
    ("ibm866", "kIbm866", "Ibm866"),
    ("iso-8859-2", "kIso8859_2", "Iso8859_2"),
    ("iso-8859-3", "kIso8859_3", "Iso8859_3"),
    ("iso-8859-4", "kIso8859_4", "Iso8859_4"),
    ("iso-8859-5", "kIso8859_5", "Iso8859_5"),
    ("iso-8859-6", "kIso8859_6", "Iso8859_6"),
    ("iso-8859-7", "kIso8859_7", "Iso8859_7"),
    ("iso-8859-8", "kIso8859_8", "Iso8859_8"),
    ("iso-8859-10", "kIso8859_10", "Iso8859_10"),
    ("iso-8859-13", "kIso8859_13", "Iso8859_13"),
    ("iso-8859-14", "kIso8859_14", "Iso8859_14"),
    ("iso-8859-15", "kIso8859_15", "Iso8859_15"),
    ("iso-8859-16", "kIso8859_16", "Iso8859_16"),
    ("koi8-r", "kKoi8R", "Koi8R"),
    ("koi8-u", "kKoi8U", "Koi8U"),
    ("macintosh", "kMacintosh", "Macintosh"),
    ("windows-874", "kWindows874", "Windows874"),
    ("windows-1250", "kWindows1250", "Windows1250"),
    ("windows-1251", "kWindows1251", "Windows1251"),
    ("windows-1252", "kWindows1252", "Windows1252"),
    ("windows-1253", "kWindows1253", "Windows1253"),
    ("windows-1254", "kWindows1254", "Iso8859_9"),
    ("windows-1255", "kWindows1255", "Windows1255"),
    ("windows-1256", "kWindows1256", "Windows1256"),
    ("windows-1257", "kWindows1257", "Windows1257"),
    ("windows-1258", "kWindows1258", "Windows1258"),
    ("x-mac-cyrillic", "kXMacCyrillic", "XMacCyrillic"),
]


def read_index(path):
    entries = {}
    identifier = "unknown"
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            match = re.search(r"Identifier: ([0-9a-f]{16,})", line)
            if match:
                identifier = match.group(1)
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        entries[int(parts[0])] = int(parts[1], 16)
    values = [entries.get(i, HOLE) for i in range(128)]
    return values, identifier


def write_array(out, symbol, values):
    out.write("constexpr std::uint16_t %s[128] = {\n" % symbol)
    line = "   "
    for value in values:
        line += " 0x%04X," % value
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
out.write("// Generated by tools/unicode/generate_single_byte.py from the WHATWG Encoding\n")
out.write("// Standard's single-byte index files. DO NOT EDIT. Regenerate instead, and diff.\n")
out.write("// 0xFFFF is a null pointer: that byte has no mapping and is U+FFFD, or an error\n")
out.write("// when TextDecoder's fatal flag is set.\n\n")

tables = []
for stem, symbol, enumerator in INDEXES:
    values, identifier = read_index(directory / ("index-%s.txt" % stem))
    out.write("// index-%s: identifier %s\n" % (stem, identifier))
    write_array(out, symbol, values)
    tables.append((symbol, enumerator))

# Iso8859_8I shares iso-8859-8's table.
tables.append(("kIso8859_8", "Iso8859_8I"))
tables.append(("kWindows1252", "Latin1"))

out.write("const std::uint16_t* SingleByteIndex(Encoding encoding) {\n")
out.write("  switch (encoding) {\n")
for symbol, enumerator in tables:
    out.write("    case Encoding::%s: return %s;\n" % (enumerator, symbol))
out.write("    default: return nullptr;\n")
out.write("  }\n")
out.write("}\n\n")

sorted_labels = sorted(LABELS, key=lambda item: item[0])
out.write("struct EncodingLabel {\n")
out.write("  const char* label;\n")
out.write("  Encoding encoding;\n")
out.write("};\n\n")
out.write("constexpr EncodingLabel kEncodingLabels[] = {\n")
for label, enumerator in sorted_labels:
    out.write('    {"%s", Encoding::%s},\n' % (label, enumerator))
out.write("};\n")
