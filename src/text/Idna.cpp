#include "text/Idna.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "text/Bidi.h"
#include "text/Normalize.h"

namespace microbrowser::text {

namespace {

enum class IdnaStatus : std::uint8_t {
  Valid,
  Ignored,
  Mapped,
  Deviation,
  Disallowed,
  DisallowedStd3Valid,
  DisallowedStd3Mapped,
};

enum class JoiningType : std::uint8_t { C, D, L, R, T, U };

struct IdnaRange {
  std::uint32_t first;
  std::uint32_t last;
  IdnaStatus status;
  std::uint32_t mapping_offset;
  std::uint32_t mapping_length;
};

struct JoiningTypeRange {
  std::uint32_t first;
  std::uint32_t last;
  JoiningType type;
};

#include "text/IdnaTables.inc"

const IdnaRange* FindIdnaRange(std::uint32_t code_point) {
  const auto* found =
      std::upper_bound(std::begin(kIdnaRanges), std::end(kIdnaRanges), code_point,
                       [](std::uint32_t value, const IdnaRange& range) {
                         return value < range.first;
                       });
  if (found == std::begin(kIdnaRanges)) {
    return nullptr;
  }
  --found;
  return code_point <= found->last ? found : nullptr;
}

JoiningType JoiningTypeOf(std::uint32_t code_point) {
  const auto* found = std::upper_bound(
      std::begin(kJoiningTypeRanges), std::end(kJoiningTypeRanges), code_point,
      [](std::uint32_t value, const JoiningTypeRange& range) { return value < range.first; });
  if (found == std::begin(kJoiningTypeRanges)) {
    return JoiningType::U;
  }
  --found;
  return code_point <= found->last ? found->type : JoiningType::U;
}

bool IsAsciiUpper(std::uint32_t code_point) {
  return code_point >= 'A' && code_point <= 'Z';
}

// --- Punycode (RFC 3492) -----------------------------------------------------
//
// Written out rather than pulled in because it is forty lines and because every one of its overflow
// checks is load-bearing: the deltas are attacker-controlled, and a wrap in the generalized
// variable-length integer is a label that decodes to something other than what it encodes.

constexpr std::uint32_t kBase = 36;
constexpr std::uint32_t kTMin = 1;
constexpr std::uint32_t kTMax = 26;
constexpr std::uint32_t kSkew = 38;
constexpr std::uint32_t kDamp = 700;
constexpr std::uint32_t kInitialBias = 72;
constexpr std::uint32_t kInitialN = 128;
constexpr std::uint32_t kMaxInt = 0x7FFFFFFF;

std::uint32_t Adapt(std::uint32_t delta, std::uint32_t count, bool first_time) {
  delta = first_time ? delta / kDamp : delta / 2;
  delta += delta / count;
  std::uint32_t k = 0;
  while (delta > ((kBase - kTMin) * kTMax) / 2) {
    delta /= kBase - kTMin;
    k += kBase;
  }
  return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
}

std::uint32_t Threshold(std::uint32_t k, std::uint32_t bias) {
  if (k <= bias) {
    return kTMin;
  }
  return k >= bias + kTMax ? kTMax : k - bias;
}

int DecodeDigit(std::uint32_t code_point) {
  if (code_point >= '0' && code_point <= '9') {
    return static_cast<int>(code_point - '0' + 26);
  }
  if (code_point >= 'a' && code_point <= 'z') {
    return static_cast<int>(code_point - 'a');
  }
  if (code_point >= 'A' && code_point <= 'Z') {
    return static_cast<int>(code_point - 'A');
  }
  return -1;
}

char EncodeDigit(std::uint32_t digit) {
  return static_cast<char>(digit < 26 ? digit + 'a' : digit - 26 + '0');
}

std::optional<CodePoints> PunycodeDecode(const CodePoints& input) {
  for (const std::uint32_t code_point : input) {
    if (code_point > 0x7F) {
      return std::nullopt;  // a punycode label is ASCII by definition
    }
  }
  CodePoints output;
  std::size_t consumed = 0;
  const std::size_t delimiter = [&] {
    for (std::size_t i = input.size(); i > 0; --i) {
      if (input[i - 1] == '-') {
        return i - 1;
      }
    }
    return std::string::npos;
  }();
  if (delimiter != std::string::npos) {
    output.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(delimiter));
    consumed = delimiter + 1;
  }

  std::uint32_t n = kInitialN;
  std::uint32_t i = 0;
  std::uint32_t bias = kInitialBias;
  while (consumed < input.size()) {
    const std::uint32_t old_i = i;
    std::uint32_t w = 1;
    for (std::uint32_t k = kBase;; k += kBase) {
      if (consumed >= input.size()) {
        return std::nullopt;
      }
      const int digit = DecodeDigit(input[consumed++]);
      if (digit < 0) {
        return std::nullopt;
      }
      const auto value = static_cast<std::uint32_t>(digit);
      if (value > (kMaxInt - i) / w) {
        return std::nullopt;
      }
      i += value * w;
      const std::uint32_t t = Threshold(k, bias);
      if (value < t) {
        break;
      }
      if (w > kMaxInt / (kBase - t)) {
        return std::nullopt;
      }
      w *= kBase - t;
    }
    const auto out_length = static_cast<std::uint32_t>(output.size() + 1);
    bias = Adapt(i - old_i, out_length, old_i == 0);
    if (i / out_length > kMaxInt - n) {
      return std::nullopt;
    }
    n += i / out_length;
    i %= out_length;
    if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) {
      return std::nullopt;  // not a scalar value, so not something a host can hold
    }
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(i), n);
    ++i;
  }
  return output;
}

std::optional<std::string> PunycodeEncode(const CodePoints& input) {
  std::string output;
  std::uint32_t basic_count = 0;
  for (const std::uint32_t code_point : input) {
    if (code_point < 0x80) {
      output.push_back(static_cast<char>(code_point));
      ++basic_count;
    }
  }
  std::uint32_t handled = basic_count;
  if (basic_count > 0) {
    output.push_back('-');
  }

  std::uint32_t n = kInitialN;
  std::uint32_t delta = 0;
  std::uint32_t bias = kInitialBias;
  while (handled < input.size()) {
    std::uint32_t m = kMaxInt;
    for (const std::uint32_t code_point : input) {
      if (code_point >= n && code_point < m) {
        m = code_point;
      }
    }
    if (m - n > (kMaxInt - delta) / (handled + 1)) {
      return std::nullopt;
    }
    delta += (m - n) * (handled + 1);
    n = m;
    for (const std::uint32_t code_point : input) {
      if (code_point < n) {
        if (++delta == 0) {
          return std::nullopt;
        }
      }
      if (code_point != n) {
        continue;
      }
      std::uint32_t q = delta;
      for (std::uint32_t k = kBase;; k += kBase) {
        const std::uint32_t t = Threshold(k, bias);
        if (q < t) {
          break;
        }
        output.push_back(EncodeDigit(t + (q - t) % (kBase - t)));
        q = (q - t) / (kBase - t);
      }
      output.push_back(EncodeDigit(q));
      bias = Adapt(delta, handled + 1, handled == basic_count);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  return output;
}

// --- Validity ----------------------------------------------------------------

bool IsValidStatus(std::uint32_t code_point, bool be_strict) {
  const IdnaRange* range = FindIdnaRange(code_point);
  if (range == nullptr) {
    return false;  // an unassigned code point is disallowed
  }
  switch (range->status) {
    case IdnaStatus::Valid:
      return true;
    case IdnaStatus::Deviation:
      return true;  // Transitional_Processing is false, so a deviation is valid
    case IdnaStatus::DisallowedStd3Valid:
      return !be_strict;
    case IdnaStatus::Ignored:
    case IdnaStatus::Mapped:
    case IdnaStatus::DisallowedStd3Mapped:
    case IdnaStatus::Disallowed:
      return false;
  }
  return false;
}

// UTS #46's ContextJ: a ZERO WIDTH (NON-)JOINER is legitimate only where a script actually needs
// one. Without this rule a label may carry invisible characters that make two different names
// render identically.
bool ContextJAllows(const CodePoints& label, std::size_t index) {
  const std::uint32_t code_point = label[index];
  if (code_point != 0x200C && code_point != 0x200D) {
    return true;
  }
  if (index == 0) {
    return false;
  }
  // Both joiners are allowed directly after a virama, which is the Indic case.
  if (CombiningClassOf(label[index - 1]) == 9) {
    return true;
  }
  if (code_point == 0x200D) {
    return false;
  }
  // ZWNJ additionally allowed between a left- or dual-joining character and a right- or
  // dual-joining one, ignoring transparent characters on either side. That is Persian.
  std::size_t before = index;
  while (before > 0) {
    --before;
    const JoiningType type = JoiningTypeOf(label[before]);
    if (type == JoiningType::T) {
      continue;
    }
    if (type != JoiningType::L && type != JoiningType::D) {
      return false;
    }
    break;
  }
  if (before == index) {
    return false;
  }
  for (std::size_t after = index + 1; after < label.size(); ++after) {
    const JoiningType type = JoiningTypeOf(label[after]);
    if (type == JoiningType::T) {
      continue;
    }
    return type == JoiningType::R || type == JoiningType::D;
  }
  return false;
}

bool IsBidiCharacter(std::uint32_t code_point) {
  const BidiClass value = BidiClassOf(code_point);
  return value == BidiClass::R || value == BidiClass::AL || value == BidiClass::AN;
}

// RFC 5893's Bidi Rule, applied to every label once any label anywhere in the domain is
// right-to-left. Mixing directions inside a name is how two names come to render the same.
bool SatisfiesBidiRule(const CodePoints& label) {
  if (label.empty()) {
    return true;  // the root label, which has no direction to disagree about
  }
  const BidiClass first = BidiClassOf(label[0]);
  const bool rtl = first == BidiClass::R || first == BidiClass::AL;
  if (!rtl && first != BidiClass::L) {
    return false;
  }

  bool saw_en = false;
  bool saw_an = false;
  for (const std::uint32_t code_point : label) {
    const BidiClass value = BidiClassOf(code_point);
    if (rtl) {
      switch (value) {
        case BidiClass::R:
        case BidiClass::AL:
        case BidiClass::AN:
        case BidiClass::EN:
        case BidiClass::ES:
        case BidiClass::CS:
        case BidiClass::ET:
        case BidiClass::ON:
        case BidiClass::BN:
        case BidiClass::NSM:
          break;
        default:
          return false;
      }
      saw_en = saw_en || value == BidiClass::EN;
      saw_an = saw_an || value == BidiClass::AN;
    } else {
      switch (value) {
        case BidiClass::L:
        case BidiClass::EN:
        case BidiClass::ES:
        case BidiClass::CS:
        case BidiClass::ET:
        case BidiClass::ON:
        case BidiClass::BN:
        case BidiClass::NSM:
          break;
        default:
          return false;
      }
    }
  }
  if (rtl && saw_en && saw_an) {
    return false;
  }

  std::size_t last = label.size();
  while (last > 0 && BidiClassOf(label[last - 1]) == BidiClass::NSM) {
    --last;
  }
  if (last == 0) {
    return false;
  }
  const BidiClass ending = BidiClassOf(label[last - 1]);
  if (rtl) {
    return ending == BidiClass::R || ending == BidiClass::AL || ending == BidiClass::EN ||
           ending == BidiClass::AN;
  }
  return ending == BidiClass::L || ending == BidiClass::EN;
}

bool IsLabelValid(const CodePoints& label, bool be_strict) {
  if (label.empty()) {
    return true;  // an empty label is a dot next to a dot, which the URL parser decides about
  }
  if (NormalizeNfc(label) != label) {
    return false;
  }
  if (be_strict) {
    // CheckHyphens: no hyphen in the third and fourth positions, and none at either end. Off for
    // URL parsing, because `xn--` itself would fail it.
    if (label.size() >= 4 && label[2] == '-' && label[3] == '-') {
      return false;
    }
    if (label.front() == '-' || label.back() == '-') {
      return false;
    }
  }
  if (IsCombiningMark(label[0])) {
    return false;
  }
  for (std::size_t i = 0; i < label.size(); ++i) {
    if (label[i] == '.') {
      return false;
    }
    if (!IsValidStatus(label[i], be_strict)) {
      return false;
    }
    if (!ContextJAllows(label, i)) {
      return false;
    }
  }
  return true;
}

bool StartsWithAcePrefix(const CodePoints& label) {
  if (label.size() < 4) {
    return false;
  }
  const auto lower = [](std::uint32_t code_point) {
    return IsAsciiUpper(code_point) ? code_point + 32 : code_point;
  };
  return lower(label[0]) == 'x' && lower(label[1]) == 'n' && label[2] == '-' && label[3] == '-';
}

}  // namespace

std::optional<std::string> UnicodeToAscii(std::string_view domain, bool be_strict) {
  // 1. Map. Every code point is kept, replaced, dropped or refused; there is no fifth answer, and
  // an unassigned code point is refused rather than passed through.
  const CodePoints decoded = DecodeUtf8(domain);
  CodePoints mapped;
  mapped.reserve(decoded.size());
  for (const std::uint32_t code_point : decoded) {
    const IdnaRange* range = FindIdnaRange(code_point);
    if (range == nullptr) {
      return std::nullopt;
    }
    switch (range->status) {
      case IdnaStatus::Valid:
      case IdnaStatus::Deviation:  // Transitional_Processing is false
        mapped.push_back(code_point);
        break;
      case IdnaStatus::Ignored:
        break;
      case IdnaStatus::Mapped:
        mapped.insert(mapped.end(), kIdnaMappingPool + range->mapping_offset,
                      kIdnaMappingPool + range->mapping_offset + range->mapping_length);
        break;
      case IdnaStatus::DisallowedStd3Valid:
        if (be_strict) {
          return std::nullopt;
        }
        mapped.push_back(code_point);
        break;
      case IdnaStatus::DisallowedStd3Mapped:
        if (be_strict) {
          return std::nullopt;
        }
        mapped.insert(mapped.end(), kIdnaMappingPool + range->mapping_offset,
                      kIdnaMappingPool + range->mapping_offset + range->mapping_length);
        break;
      case IdnaStatus::Disallowed:
        return std::nullopt;
    }
  }

  // 2. Normalize, then 3. break into labels. Both in that order: a label boundary is U+002E, and
  // normalization can neither create nor destroy one, but it can change what is on either side.
  const CodePoints normalized = NormalizeNfc(mapped);
  std::vector<CodePoints> labels(1);
  for (const std::uint32_t code_point : normalized) {
    if (code_point == '.') {
      labels.emplace_back();
    } else {
      labels.back().push_back(code_point);
    }
  }

  // 4. Convert and validate. A label that already carries the ACE prefix is decoded and then held
  // to exactly the same criteria as one that did not, which is the point: `xn--` is a spelling, not
  // a permission.
  std::vector<CodePoints> unicode_labels;
  unicode_labels.reserve(labels.size());
  for (const CodePoints& label : labels) {
    if (!StartsWithAcePrefix(label)) {
      if (!IsLabelValid(label, be_strict)) {
        return std::nullopt;
      }
      unicode_labels.push_back(label);
      continue;
    }
    const CodePoints encoded(label.begin() + 4, label.end());
    const std::optional<CodePoints> unicode = PunycodeDecode(encoded);
    if (!unicode.has_value() || unicode->empty()) {
      return std::nullopt;
    }
    if (!IsLabelValid(*unicode, be_strict)) {
      return std::nullopt;
    }
    unicode_labels.push_back(*unicode);
  }

  // 5. CheckBidi is a property of the *domain*, not of a label: one right-to-left label anywhere
  // puts every label under RFC 5893's rule.
  const bool is_bidi_domain = std::any_of(
      unicode_labels.begin(), unicode_labels.end(), [](const CodePoints& label) {
        return std::any_of(label.begin(), label.end(), IsBidiCharacter);
      });
  if (is_bidi_domain) {
    for (const CodePoints& label : unicode_labels) {
      if (!SatisfiesBidiRule(label)) {
        return std::nullopt;
      }
    }
  }

  // 6. Back to ASCII. A label that is already ASCII is emitted as it stands -- re-encoding one
  // would turn `xn--` back into `xn--xn--`.
  std::string out;
  for (std::size_t i = 0; i < unicode_labels.size(); ++i) {
    if (i > 0) {
      out.push_back('.');
    }
    const CodePoints& label = unicode_labels[i];
    const bool is_ascii =
        std::all_of(label.begin(), label.end(), [](std::uint32_t c) { return c < 0x80; });
    if (is_ascii) {
      for (const std::uint32_t code_point : label) {
        out.push_back(static_cast<char>(code_point));
      }
      continue;
    }
    const std::optional<std::string> encoded = PunycodeEncode(label);
    if (!encoded.has_value()) {
      return std::nullopt;
    }
    out += "xn--";
    out += *encoded;
  }

  if (be_strict) {
    // VerifyDnsLength. Off for URL parsing, because a URL that names a too-long host is a name
    // resolution that fails rather than a URL that does not exist.
    std::size_t total = out.size();
    if (!out.empty() && out.back() == '.') {
      --total;
    }
    if (total < 1 || total > 253) {
      return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= out.size()) {
      const std::size_t dot = out.find('.', start);
      const std::size_t end = dot == std::string::npos ? out.size() : dot;
      const std::size_t length = end - start;
      if ((length < 1 || length > 63) && !(dot == std::string::npos && length == 0)) {
        return std::nullopt;
      }
      if (dot == std::string::npos) {
        break;
      }
      start = dot + 1;
    }
  }
  return out;
}

}  // namespace microbrowser::text
