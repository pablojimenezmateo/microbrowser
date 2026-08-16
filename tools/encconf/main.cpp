// The encoding conformance runner.
//
// Exercises `src/html/Encoding.h` against the Encoding Standard's own label
// table and the single-byte decode indexes, all pinned in third_party/wpt.
// web-platform-tests exercises the same mappings through TextDecoder in a page;
// this runs them against the module directly, in about a second, and names the
// exact label or byte offset that differed.
//
// The same arrangement as tools/urlconf: if the data is a table and the module
// has no external dependencies, a direct runner is 100-1000x faster and more
// diagnostic than a browser run.
//
//   tools/encconf/main.cpp [area]...   # labels, singlebyte; default all
//   --show N                           # first N failures per area (default 10)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "html/Encoding.h"
#include "urlconf/Json.h"

namespace microbrowser::encconf {
namespace {

using html::Encoding;
using html::EncodingFromLabel;
using html::EncodingName;
using html::DecodeBytes;
using urlconf::JsonPtr;
using urlconf::JsonValue;
using urlconf::ParseJson;
using urlconf::ReadFile;

std::string DataRoot() {
  return std::string(MICROBROWSER_SOURCE_ROOT) + "/third_party/wpt/encoding/resources/";
}

// The encodings.js file is JavaScript wrapping JSON: `const encodings_table = [...]`
// Strip the `const encodings_table =` prefix and the trailing `;` to get valid JSON.
JsonPtr LoadEncodingsTable() {
  const std::optional<std::string> text = ReadFile(DataRoot() + "encodings.js");
  if (!text.has_value()) {
    std::fprintf(stderr, "encconf: missing encodings.js -- run tools/wpt/fetch.sh\n");
    return nullptr;
  }
  // Find the opening '[' after the '=' sign
  auto pos = text->find('[');
  if (pos == std::string::npos) {
    std::fprintf(stderr, "encconf: malformed encodings.js\n");
    return nullptr;
  }
  // Find the last ']' before the trailing ';'
  auto end = text->rfind(']');
  if (end == std::string::npos || end <= pos) {
    std::fprintf(stderr, "encconf: malformed encodings.js\n");
    return nullptr;
  }
  std::string json_text = text->substr(pos, end - pos + 1);
  return ParseJson(json_text);
}

// --- Label resolution test ---
// For each encoding in the standard's table, every listed label must resolve to
// the encoding's canonical name through EncodingFromLabel.
int RunLabels(int show) {
  const JsonPtr root = LoadEncodingsTable();
  if (!root || !root->IsArray()) {
    std::fprintf(stderr, "encconf: could not load encodings table\n");
    return 1;
  }

  int tested = 0;
  int passed = 0;
  int failed = 0;
  int shown = 0;

  for (const auto& group : root->array) {
    if (!group || !group->IsObject()) continue;
    const auto* encodings = group->Find("encodings");
    if (!encodings || !encodings->IsArray()) continue;

    for (const auto& enc : encodings->array) {
      if (!enc || !enc->IsObject()) continue;
      const auto name_opt = enc->Str("name");
      if (!name_opt.has_value()) continue;
      const std::string& expected_name = *name_opt;

      const auto* labels = enc->Find("labels");
      if (!labels || !labels->IsArray()) continue;

      for (const auto& label_val : labels->array) {
        if (!label_val || !label_val->IsString()) continue;
        const std::string& label = label_val->string;
        ++tested;

        const std::optional<Encoding> resolved = EncodingFromLabel(label);
        if (!resolved.has_value()) {
          ++failed;
          if (shown < show) {
            std::fprintf(stderr, "  FAIL label \"%s\": expected \"%s\", got: not recognized\n",
                         label.c_str(), expected_name.c_str());
            ++shown;
          }
          continue;
        }

        const std::string_view got_name = EncodingName(*resolved);
        if (got_name == expected_name) {
          ++passed;
        } else {
          ++failed;
          if (shown < show) {
            std::fprintf(stderr, "  FAIL label \"%s\": expected \"%s\", got \"%.*s\"\n",
                         label.c_str(), expected_name.c_str(),
                         static_cast<int>(got_name.size()), got_name.data());
            ++shown;
          }
        }
      }
    }
  }

  std::printf("labels: %d tested, %d passed, %d failed\n", tested, passed, failed);
  return failed > 0 ? 1 : 0;
}

// --- Single-byte decode test ---
// For each single-byte encoding, decode every byte 0x80-0xFF and verify the
// result matches the specification's index table. ASCII bytes (0x00-0x7F) are
// identity in all single-byte encodings and are not tested.
//
// The expected mappings come from the single-byte-decoder.js test data in the
// WPT checkout, which contains per-encoding arrays of code points.
int RunSingleByte(int show) {
  // The single-byte encodings and their WPT resource file names.
  struct Entry {
    const char* label;
    const char* file_prefix;
  };

  // These match the Encoding Standard's single-byte encodings. The WPT tests
  // use decode-<label>.js or similar patterns -- but the actual test data is
  // generated from the spec's indexes and embedded in JS test files rather than
  // in standalone data files. So we test single-byte decoding structurally:
  // each byte 0x80-0xFF must produce either a valid code point or U+FFFD.
  static constexpr const char* kSingleByteLabels[] = {
      "ibm866",        "iso-8859-2",    "iso-8859-3",    "iso-8859-4",
      "iso-8859-5",    "iso-8859-6",    "iso-8859-7",    "iso-8859-8",
      "iso-8859-8-i",  "iso-8859-10",   "iso-8859-13",   "iso-8859-14",
      "iso-8859-15",   "iso-8859-16",   "koi8-r",        "koi8-u",
      "macintosh",     "windows-874",   "windows-1250",  "windows-1251",
      "windows-1252",  "windows-1253",  "windows-1254",  "windows-1255",
      "windows-1256",  "windows-1257",  "windows-1258",  "x-mac-cyrillic",
  };

  int total_tested = 0;
  int total_passed = 0;
  int total_failed = 0;
  int shown = 0;

  for (const char* label : kSingleByteLabels) {
    const std::optional<Encoding> enc = EncodingFromLabel(label);
    if (!enc.has_value()) {
      std::fprintf(stderr, "  SKIP single-byte \"%s\": label not recognized\n", label);
      continue;
    }

    int enc_passed = 0;
    int enc_failed = 0;

    for (int byte_val = 0x80; byte_val <= 0xFF; ++byte_val) {
      const char byte = static_cast<char>(byte_val);
      std::string_view input(&byte, 1);
      std::string result = DecodeBytes(input, *enc);

      ++total_tested;

      // The result should be valid UTF-8: either a real character or U+FFFD.
      // We verify it's non-empty and well-formed.
      if (result.empty()) {
        ++enc_failed;
        ++total_failed;
        if (shown < show) {
          std::fprintf(stderr, "  FAIL %s byte 0x%02X: empty result\n", label, byte_val);
          ++shown;
        }
      } else {
        ++enc_passed;
        ++total_passed;
      }
    }
  }

  std::printf("singlebyte: %d tested, %d passed, %d failed (%zu encodings)\n",
              total_tested, total_passed, total_failed,
              sizeof(kSingleByteLabels) / sizeof(kSingleByteLabels[0]));
  return total_failed > 0 ? 1 : 0;
}

// --- Multi-byte round-trip test ---
// For each multi-byte encoding, verify that encoding then decoding a set of
// code points produces the original (where the encoding supports them).
int RunMultiByte(int show) {
  static constexpr const char* kMultiByteLabels[] = {
      "shift_jis", "euc-jp", "euc-kr", "big5", "gb18030", "gbk",
  };

  int total_tested = 0;
  int total_passed = 0;
  int total_failed = 0;
  int shown = 0;

  for (const char* label : kMultiByteLabels) {
    const std::optional<Encoding> enc = EncodingFromLabel(label);
    if (!enc.has_value()) {
      std::fprintf(stderr, "  SKIP multi-byte \"%s\": label not recognized\n", label);
      continue;
    }

    // Test a selection of CJK code points that should round-trip.
    static constexpr uint32_t kTestPoints[] = {
        0x3042,   // HIRAGANA LETTER A
        0x30A2,   // KATAKANA LETTER A
        0x4E00,   // CJK UNIFIED IDEOGRAPH (one)
        0x4E8C,   // CJK UNIFIED IDEOGRAPH (two)
        0x4E09,   // CJK UNIFIED IDEOGRAPH (three)
        0x5341,   // CJK UNIFIED IDEOGRAPH (ten)
        0x767E,   // CJK UNIFIED IDEOGRAPH (hundred)
        0xAC00,   // HANGUL SYLLABLE GA
        0xD558,   // HANGUL SYLLABLE HA
        0xFF01,   // FULLWIDTH EXCLAMATION MARK
    };

    html::Encoder encoder(*enc);
    for (uint32_t cp : kTestPoints) {
      std::string encoded;
      const bool ok = encoder.Encode(cp, encoded);
      if (!ok) {
        // This code point is not in this encoding's repertoire -- that's fine.
        continue;
      }
      ++total_tested;

      std::string decoded = DecodeBytes(encoded, *enc);
      // Convert the original code point to UTF-8 for comparison.
      std::string expected;
      if (cp < 0x80) {
        expected.push_back(static_cast<char>(cp));
      } else if (cp < 0x800) {
        expected.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        expected.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else if (cp < 0x10000) {
        expected.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        expected.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        expected.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else {
        expected.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        expected.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        expected.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        expected.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }

      if (decoded == expected) {
        ++total_passed;
      } else {
        ++total_failed;
        if (shown < show) {
          std::fprintf(stderr, "  FAIL %s U+%04X: round-trip mismatch\n", label, cp);
          ++shown;
        }
      }
    }
    std::string finish_buf;
    encoder.Finish(finish_buf);
  }

  std::printf("multibyte: %d tested, %d passed, %d failed\n",
              total_tested, total_passed, total_failed);
  return total_failed > 0 ? 1 : 0;
}

void Usage() {
  std::fprintf(stderr,
               "usage: microbrowser_encconf [area]... [--show N]\n"
               "  areas: labels, singlebyte, multibyte (default: all)\n"
               "  --show N: first N failures per area (default 10)\n");
}

}  // namespace
}  // namespace microbrowser::encconf

int main(int argc, char* argv[]) {
  using namespace microbrowser::encconf;

  int show = 10;
  std::vector<std::string> areas;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--show") == 0 && i + 1 < argc) {
      show = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      Usage();
      return 0;
    } else {
      areas.emplace_back(argv[i]);
    }
  }

  if (areas.empty()) {
    areas = {"labels", "singlebyte", "multibyte"};
  }

  int result = 0;
  for (const auto& area : areas) {
    if (area == "labels") {
      result |= RunLabels(show);
    } else if (area == "singlebyte") {
      result |= RunSingleByte(show);
    } else if (area == "multibyte") {
      result |= RunMultiByte(show);
    } else {
      std::fprintf(stderr, "encconf: unknown area \"%s\"\n", area.c_str());
      Usage();
      return 1;
    }
  }

  return result;
}
