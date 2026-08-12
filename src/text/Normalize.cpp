#include "text/Normalize.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microbrowser::text {

namespace {

struct DecompositionRow {
  std::uint32_t code;
  std::uint32_t offset;
  std::uint32_t length;
};

struct CompositionRow {
  std::uint32_t starter;
  std::uint32_t second;
  std::uint32_t composed;
};

struct CombiningClassRange {
  std::uint32_t first;
  std::uint32_t last;
  std::uint8_t combining_class;
};

struct CodePointRange {
  std::uint32_t first;
  std::uint32_t last;
};

#include "text/NormalizationTables.inc"

// Hangul is not in any table above: its decomposition is arithmetic, and 11,172 rows of it would
// be a quarter of this file for information a formula already carries.
constexpr std::uint32_t kHangulSBase = 0xAC00;
constexpr std::uint32_t kHangulLBase = 0x1100;
constexpr std::uint32_t kHangulVBase = 0x1161;
constexpr std::uint32_t kHangulTBase = 0x11A7;
constexpr std::uint32_t kHangulVCount = 21;
constexpr std::uint32_t kHangulTCount = 28;
constexpr std::uint32_t kHangulNCount = kHangulVCount * kHangulTCount;
constexpr std::uint32_t kHangulSCount = 19 * kHangulNCount;

bool IsHangulSyllable(std::uint32_t code_point) {
  return code_point >= kHangulSBase && code_point < kHangulSBase + kHangulSCount;
}

void DecomposeHangul(std::uint32_t code_point, CodePoints& out) {
  const std::uint32_t index = code_point - kHangulSBase;
  out.push_back(kHangulLBase + index / kHangulNCount);
  out.push_back(kHangulVBase + (index % kHangulNCount) / kHangulTCount);
  if (const std::uint32_t trailing = index % kHangulTCount; trailing != 0) {
    out.push_back(kHangulTBase + trailing);
  }
}

const DecompositionRow* FindDecomposition(std::uint32_t code_point) {
  const auto* found = std::lower_bound(
      std::begin(kDecompositions), std::end(kDecompositions), code_point,
      [](const DecompositionRow& row, std::uint32_t value) { return row.code < value; });
  return found != std::end(kDecompositions) && found->code == code_point ? found : nullptr;
}

std::uint32_t ComposePair(std::uint32_t starter, std::uint32_t second) {
  // Hangul first, for the reason it is absent from the table.
  if (starter >= kHangulLBase && starter < kHangulLBase + 19 && second >= kHangulVBase &&
      second < kHangulVBase + kHangulVCount) {
    return kHangulSBase + ((starter - kHangulLBase) * kHangulVCount + (second - kHangulVBase)) *
                              kHangulTCount;
  }
  if (IsHangulSyllable(starter) && (starter - kHangulSBase) % kHangulTCount == 0 &&
      second > kHangulTBase && second < kHangulTBase + kHangulTCount) {
    return starter + (second - kHangulTBase);
  }
  const auto* found = std::lower_bound(
      std::begin(kCompositions), std::end(kCompositions), std::pair(starter, second),
      [](const CompositionRow& row, const std::pair<std::uint32_t, std::uint32_t>& key) {
        return std::pair(row.starter, row.second) < key;
      });
  if (found != std::end(kCompositions) && found->starter == starter && found->second == second) {
    return found->composed;
  }
  return 0;
}

CodePoints Decompose(const CodePoints& input) {
  CodePoints out;
  out.reserve(input.size());
  for (const std::uint32_t code_point : input) {
    if (IsHangulSyllable(code_point)) {
      DecomposeHangul(code_point, out);
      continue;
    }
    if (const DecompositionRow* row = FindDecomposition(code_point); row != nullptr) {
      out.insert(out.end(), kDecompositionPool + row->offset,
                 kDecompositionPool + row->offset + row->length);
      continue;
    }
    out.push_back(code_point);
  }
  return out;
}

// Canonical ordering: a stable sort of each run of non-starters by combining class. Stable is the
// whole of it — two marks with the same class must keep their order, or normalization stops being
// idempotent and two spellings of one string stay two.
void CanonicalOrder(CodePoints& text) {
  const std::size_t size = text.size();
  for (std::size_t i = 1; i < size; ++i) {
    const std::uint8_t current = CombiningClassOf(text[i]);
    if (current == 0) {
      continue;
    }
    std::size_t j = i;
    while (j > 0 && CombiningClassOf(text[j - 1]) > current) {
      std::swap(text[j], text[j - 1]);
      --j;
    }
  }
}

}  // namespace

std::uint8_t CombiningClassOf(std::uint32_t code_point) {
  const auto* found = std::upper_bound(
      std::begin(kCombiningClasses), std::end(kCombiningClasses), code_point,
      [](std::uint32_t value, const CombiningClassRange& range) { return value < range.first; });
  if (found == std::begin(kCombiningClasses)) {
    return 0;
  }
  --found;
  return code_point <= found->last ? found->combining_class : std::uint8_t{0};
}

bool IsCombiningMark(std::uint32_t code_point) {
  const auto* found =
      std::upper_bound(std::begin(kMarks), std::end(kMarks), code_point,
                       [](std::uint32_t value, const CodePointRange& range) {
                         return value < range.first;
                       });
  if (found == std::begin(kMarks)) {
    return false;
  }
  --found;
  return code_point <= found->last;
}

void AppendUtf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

CodePoints DecodeUtf8(std::string_view text) {
  CodePoints out;
  out.reserve(text.size());
  std::size_t at = 0;
  std::uint32_t code = 0;
  while (at < text.size()) {
    const std::size_t start = at;
    if (util::DecodeUtf8(text, at, code)) {
      out.push_back(code);
    } else {
      out.push_back(0xFFFD);
      at = start + 1;
    }
  }
  return out;
}

std::string EncodeUtf8(const CodePoints& code_points) {
  std::string out;
  out.reserve(code_points.size());
  for (const std::uint32_t code_point : code_points) {
    AppendUtf8(out, code_point);
  }
  return out;
}

CodePoints NormalizeNfc(const CodePoints& input) {
  CodePoints text = Decompose(input);
  CanonicalOrder(text);

  // Canonical composition, UAX #15's own loop: walk forward carrying the last starter, and try to
  // compose each following character onto it. `last_class` is what makes a blocked combining mark
  // stay put — a mark cannot compose past another mark of the same or higher class.
  CodePoints out;
  out.reserve(text.size());
  std::size_t starter_index = std::string::npos;
  int last_class = -1;
  for (const std::uint32_t code_point : text) {
    const std::uint8_t combining_class = CombiningClassOf(code_point);
    if (starter_index != std::string::npos && last_class < static_cast<int>(combining_class)) {
      if (const std::uint32_t composed = ComposePair(out[starter_index], code_point);
          composed != 0) {
        out[starter_index] = composed;
        continue;
      }
    }
    if (combining_class == 0) {
      starter_index = out.size();
      last_class = -1;
    } else {
      last_class = static_cast<int>(combining_class);
    }
    out.push_back(code_point);
  }
  return out;
}

}  // namespace microbrowser::text
