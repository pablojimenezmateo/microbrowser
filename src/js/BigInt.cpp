#include "js/BigInt.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Arbitrary-precision integers, in base 2^32.
//
// Nothing here knows what a JavaScript value is: this is the arithmetic, and
// the type that wraps it lives beside the heap. Keeping the two apart is what
// lets every operation below be checked against a calculator rather than
// against the engine.

namespace microbrowser::js {

namespace {

constexpr std::uint64_t kBase = 1ull << 32;

int DigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

void BigInt::Trim() {
  while (!limbs_.empty() && limbs_.back() == 0) {
    limbs_.pop_back();
  }
  if (limbs_.empty()) {
    negative_ = false;  // one zero, and it is positive
  }
}

BigInt BigInt::FromInt64(std::int64_t value) {
  BigInt out;
  // Through uint64 so that INT64_MIN negates without overflowing, which is
  // undefined behaviour and is exactly the value a page reaches for.
  auto magnitude = static_cast<std::uint64_t>(value);
  if (value < 0) {
    out.negative_ = true;
    magnitude = ~magnitude + 1;
  }
  while (magnitude != 0) {
    out.limbs_.push_back(static_cast<std::uint32_t>(magnitude & 0xFFFFFFFFull));
    magnitude >>= 32;
  }
  out.Trim();
  return out;
}

bool BigInt::FromDouble(double value, BigInt& out) {
  if (!std::isfinite(value) || value != std::trunc(value)) {
    return false;
  }
  out = BigInt();
  double magnitude = std::fabs(value);
  if (magnitude < 1.0) {
    return true;
  }
  out.negative_ = value < 0;
  // Peeled off a limb at a time from the top, which is exact: a double's
  // mantissa is 53 bits, so `fmod` against 2^32 is a lossless split.
  std::vector<std::uint32_t> reversed;
  while (magnitude >= 1.0) {
    const double limb = std::fmod(magnitude, static_cast<double>(kBase));
    reversed.push_back(static_cast<std::uint32_t>(limb));
    magnitude = std::floor(magnitude / static_cast<double>(kBase));
    if (reversed.size() > kMaxBigIntLimbs) {
      return false;
    }
  }
  out.limbs_ = std::move(reversed);
  out.Trim();
  return true;
}

bool BigInt::Parse(std::string_view text, BigInt& out) {
  std::size_t at = 0;
  while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' ||
                              text[at] == '\r' || text[at] == '\f' || text[at] == '\v')) {
    ++at;
  }
  std::size_t end = text.size();
  while (end > at && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                      text[end - 1] == '\r' || text[end - 1] == '\f' || text[end - 1] == '\v')) {
    --end;
  }
  out = BigInt();
  if (at == end) {
    return true;  // an empty string is zero, which is what Number does too
  }
  bool negative = false;
  if (text[at] == '+' || text[at] == '-') {
    negative = text[at] == '-';
    ++at;
  }
  int radix = 10;
  if (!negative && at + 1 < end && text[at] == '0') {
    const char marker = text[at + 1];
    if (marker == 'x' || marker == 'X') {
      radix = 16;
      at += 2;
    } else if (marker == 'o' || marker == 'O') {
      radix = 8;
      at += 2;
    } else if (marker == 'b' || marker == 'B') {
      radix = 2;
      at += 2;
    }
  }
  if (at >= end) {
    return false;
  }
  for (; at < end; ++at) {
    if (text[at] == '_') {
      continue;  // numeric separators, which the lexer already allows
    }
    const int digit = DigitValue(text[at]);
    if (digit < 0 || digit >= radix) {
      return false;
    }
    // out = out * radix + digit, in place.
    std::uint64_t carry = static_cast<std::uint64_t>(digit);
    for (std::uint32_t& limb : out.limbs_) {
      const std::uint64_t product =
          static_cast<std::uint64_t>(limb) * static_cast<std::uint64_t>(radix) + carry;
      limb = static_cast<std::uint32_t>(product & 0xFFFFFFFFull);
      carry = product >> 32;
    }
    while (carry != 0) {
      if (out.limbs_.size() >= kMaxBigIntLimbs) {
        return false;
      }
      out.limbs_.push_back(static_cast<std::uint32_t>(carry & 0xFFFFFFFFull));
      carry >>= 32;
    }
  }
  out.negative_ = negative;
  out.Trim();
  return true;
}

double BigInt::ToDouble() const {
  double value = 0;
  for (std::size_t i = limbs_.size(); i > 0; --i) {
    value = value * static_cast<double>(kBase) + static_cast<double>(limbs_[i - 1]);
  }
  return negative_ ? -value : value;
}

std::string BigInt::ToString(int radix) const {
  if (limbs_.empty()) {
    return "0";
  }
  // Repeated division of the magnitude by the radix. Quadratic in the number
  // of limbs, which for the sizes a page uses is nothing -- and the
  // sub-quadratic alternative is a divide-and-conquer split that is much
  // harder to check.
  std::vector<std::uint32_t> work = limbs_;
  std::string digits;
  while (!work.empty()) {
    std::uint64_t remainder = 0;
    for (std::size_t i = work.size(); i > 0; --i) {
      const std::uint64_t current = (remainder << 32) | work[i - 1];
      work[i - 1] = static_cast<std::uint32_t>(current / static_cast<std::uint64_t>(radix));
      remainder = current % static_cast<std::uint64_t>(radix);
    }
    digits.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[remainder]);
    while (!work.empty() && work.back() == 0) {
      work.pop_back();
    }
  }
  if (negative_) {
    digits.push_back('-');
  }
  return std::string(digits.rbegin(), digits.rend());
}

int BigInt::CompareMagnitude(const BigInt& a, const BigInt& b) {
  if (a.limbs_.size() != b.limbs_.size()) {
    return a.limbs_.size() < b.limbs_.size() ? -1 : 1;
  }
  for (std::size_t i = a.limbs_.size(); i > 0; --i) {
    if (a.limbs_[i - 1] != b.limbs_[i - 1]) {
      return a.limbs_[i - 1] < b.limbs_[i - 1] ? -1 : 1;
    }
  }
  return 0;
}

int BigInt::Compare(const BigInt& a, const BigInt& b) {
  if (a.negative_ != b.negative_) {
    return a.negative_ ? -1 : 1;
  }
  const int magnitude = CompareMagnitude(a, b);
  return a.negative_ ? -magnitude : magnitude;
}

int BigInt::CompareDouble(const BigInt& a, double b) {
  if (std::isnan(b)) {
    return 2;  // unordered: every comparison against NaN is false
  }
  if (std::isinf(b)) {
    return b > 0 ? -1 : 1;
  }
  // Through the double when the bigint fits one exactly, and through the
  // *floor* otherwise: comparing `2n ** 100n` against 1.5 must not round the
  // bigint to something that compares equal.
  BigInt floored;
  if (!FromDouble(std::trunc(b), floored)) {
    return 0;
  }
  const int whole = Compare(a, floored);
  if (whole != 0) {
    return whole;
  }
  // Equal to the truncated part: the fraction decides, and its sign is the
  // sign of `b`.
  const double fraction = b - std::trunc(b);
  if (fraction == 0) {
    return 0;
  }
  return fraction > 0 ? -1 : 1;
}

bool BigInt::AddMagnitude(const BigInt& a, const BigInt& b, BigInt& out) {
  const std::size_t size = std::max(a.limbs_.size(), b.limbs_.size());
  if (size + 1 > kMaxBigIntLimbs) {
    return false;
  }
  std::vector<std::uint32_t> result;
  result.reserve(size + 1);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint64_t left = i < a.limbs_.size() ? a.limbs_[i] : 0;
    const std::uint64_t right = i < b.limbs_.size() ? b.limbs_[i] : 0;
    const std::uint64_t sum = left + right + carry;
    result.push_back(static_cast<std::uint32_t>(sum & 0xFFFFFFFFull));
    carry = sum >> 32;
  }
  if (carry != 0) {
    result.push_back(static_cast<std::uint32_t>(carry));
  }
  out.limbs_ = std::move(result);
  out.Trim();
  return true;
}

void BigInt::SubtractMagnitude(const BigInt& a, const BigInt& b, BigInt& out) {
  std::vector<std::uint32_t> result;
  result.reserve(a.limbs_.size());
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < a.limbs_.size(); ++i) {
    const std::int64_t right = i < b.limbs_.size() ? static_cast<std::int64_t>(b.limbs_[i]) : 0;
    std::int64_t difference = static_cast<std::int64_t>(a.limbs_[i]) - right - borrow;
    borrow = 0;
    if (difference < 0) {
      difference += static_cast<std::int64_t>(kBase);
      borrow = 1;
    }
    result.push_back(static_cast<std::uint32_t>(difference));
  }
  out.limbs_ = std::move(result);
  out.Trim();
}

bool BigInt::Add(const BigInt& a, const BigInt& b, BigInt& out) {
  BigInt made;
  if (a.negative_ == b.negative_) {
    if (!AddMagnitude(a, b, made)) {
      return false;
    }
    made.negative_ = a.negative_;
  } else {
    const int order = CompareMagnitude(a, b);
    if (order == 0) {
      out = BigInt();
      return true;
    }
    if (order > 0) {
      SubtractMagnitude(a, b, made);
      made.negative_ = a.negative_;
    } else {
      SubtractMagnitude(b, a, made);
      made.negative_ = b.negative_;
    }
  }
  made.Trim();
  out = std::move(made);
  return true;
}

bool BigInt::Subtract(const BigInt& a, const BigInt& b, BigInt& out) {
  return Add(a, Negate(b), out);
}

BigInt BigInt::Negate(const BigInt& value) {
  BigInt out = value;
  if (!out.limbs_.empty()) {
    out.negative_ = !out.negative_;
  }
  return out;
}

bool BigInt::Multiply(const BigInt& a, const BigInt& b, BigInt& out) {
  if (a.limbs_.empty() || b.limbs_.empty()) {
    out = BigInt();
    return true;
  }
  if (a.limbs_.size() + b.limbs_.size() > kMaxBigIntLimbs) {
    return false;
  }
  // Schoolbook. Quadratic, and the sizes a page uses make that the right
  // choice: Karatsuba wins somewhere past a thousand limbs and is much harder
  // to be sure of.
  std::vector<std::uint32_t> result(a.limbs_.size() + b.limbs_.size(), 0);
  for (std::size_t i = 0; i < a.limbs_.size(); ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < b.limbs_.size(); ++j) {
      const std::uint64_t product = static_cast<std::uint64_t>(a.limbs_[i]) * b.limbs_[j] +
                                    result[i + j] + carry;
      result[i + j] = static_cast<std::uint32_t>(product & 0xFFFFFFFFull);
      carry = product >> 32;
    }
    std::size_t at = i + b.limbs_.size();
    while (carry != 0) {
      const std::uint64_t sum = result[at] + carry;
      result[at] = static_cast<std::uint32_t>(sum & 0xFFFFFFFFull);
      carry = sum >> 32;
      ++at;
    }
  }
  out.limbs_ = std::move(result);
  out.negative_ = a.negative_ != b.negative_;
  out.Trim();
  return true;
}

bool BigInt::Divide(const BigInt& a, const BigInt& b, BigInt& quotient, BigInt& remainder) {
  if (b.limbs_.empty()) {
    return false;  // the caller turns this into a RangeError
  }
  if (CompareMagnitude(a, b) < 0) {
    quotient = BigInt();
    remainder = a;
    return true;
  }
  // Bit-at-a-time long division. Slower than Knuth's algorithm D by a constant
  // factor of about thirty and short enough to check by eye, which is the
  // right trade until something measures it: a page that divides bigints is
  // doing it a handful of times, not in a loop over a document.
  BigInt work;
  BigInt result;
  result.limbs_.assign(a.limbs_.size(), 0);
  for (std::size_t bit = a.limbs_.size() * 32; bit > 0; --bit) {
    const std::size_t index = (bit - 1) / 32;
    const std::uint32_t mask = 1u << ((bit - 1) % 32);
    // work = work * 2 + this bit of a
    std::uint32_t carry = (a.limbs_[index] & mask) != 0 ? 1u : 0u;
    for (std::uint32_t& limb : work.limbs_) {
      const std::uint64_t doubled = (static_cast<std::uint64_t>(limb) << 1) | carry;
      limb = static_cast<std::uint32_t>(doubled & 0xFFFFFFFFull);
      carry = static_cast<std::uint32_t>(doubled >> 32);
    }
    if (carry != 0) {
      work.limbs_.push_back(carry);
    }
    work.Trim();
    if (CompareMagnitude(work, b) >= 0) {
      BigInt reduced;
      SubtractMagnitude(work, b, reduced);
      work = std::move(reduced);
      result.limbs_[index] |= mask;
    }
  }
  result.Trim();
  result.negative_ = !result.limbs_.empty() && (a.negative_ != b.negative_);
  work.negative_ = !work.limbs_.empty() && a.negative_;
  quotient = std::move(result);
  remainder = std::move(work);
  return true;
}

bool BigInt::Power(const BigInt& base, const BigInt& exponent, BigInt& out) {
  if (exponent.negative_) {
    return false;  // a negative exponent is a RangeError, not a fraction
  }
  // Square and multiply, over the exponent's bits. The size check inside
  // Multiply is what bounds this: `2n ** 100000000n` fails on the first
  // squaring that would exceed the cap rather than after allocating for it.
  out = FromInt64(1);
  BigInt factor = base;
  for (std::size_t bit = 0; bit < exponent.limbs_.size() * 32; ++bit) {
    const std::uint32_t limb = exponent.limbs_[bit / 32];
    if ((limb >> (bit % 32)) & 1u) {
      BigInt product;
      if (!Multiply(out, factor, product)) {
        return false;
      }
      out = std::move(product);
    }
    // The last squaring is skipped: nothing above the top bit uses it, and it
    // is the one most likely to exceed the cap.
    if (bit + 1 < exponent.limbs_.size() * 32) {
      BigInt squared;
      if (!Multiply(factor, factor, squared)) {
        // Only fatal if a higher bit is still set; otherwise the loop is done
        // with `factor` and the overflow is irrelevant.
        bool higher = false;
        for (std::size_t rest = bit + 1; rest < exponent.limbs_.size() * 32; ++rest) {
          higher = higher || ((exponent.limbs_[rest / 32] >> (rest % 32)) & 1u) != 0;
        }
        if (higher) {
          return false;
        }
        return true;
      }
      factor = std::move(squared);
    }
  }
  return true;
}

bool BigInt::ShiftLeft(const BigInt& a, std::int64_t by, BigInt& out) {
  if (a.limbs_.empty()) {
    out = BigInt();
    return true;
  }
  if (by < 0) {
    // A right shift, which for a negative value rounds *down* -- `-1n >> 1n`
    // is `-1n`, not `0n`. Written as a floor division by a power of two,
    // which is what the two's-complement definition amounts to.
    BigInt divisor;
    if (!ShiftLeft(FromInt64(1), -by, divisor)) {
      return false;
    }
    BigInt quotient;
    BigInt remainder;
    if (!Divide(a, divisor, quotient, remainder)) {
      return false;
    }
    if (a.negative_ && !remainder.IsZero()) {
      BigInt lowered;
      if (!Subtract(quotient, FromInt64(1), lowered)) {
        return false;
      }
      quotient = std::move(lowered);
    }
    out = std::move(quotient);
    return true;
  }
  const auto whole = static_cast<std::size_t>(by / 32);
  const auto bits = static_cast<std::uint32_t>(by % 32);
  if (a.limbs_.size() + whole + 1 > kMaxBigIntLimbs) {
    return false;
  }
  std::vector<std::uint32_t> result(whole, 0);
  std::uint32_t carry = 0;
  for (const std::uint32_t limb : a.limbs_) {
    // A shift by zero would be a 32-bit shift in the carry, which is undefined
    // behaviour -- so the two cases are written apart.
    if (bits == 0) {
      result.push_back(limb);
    } else {
      result.push_back((limb << bits) | carry);
      carry = limb >> (32 - bits);
    }
  }
  if (carry != 0) {
    result.push_back(carry);
  }
  out.limbs_ = std::move(result);
  out.negative_ = a.negative_;
  out.Trim();
  return true;
}

bool BigInt::Not(const BigInt& value, BigInt& out) {
  // `~x == -x - 1`, exactly, for an unbounded integer.
  BigInt negated = Negate(value);
  return Subtract(negated, FromInt64(1), out);
}

bool BigInt::Bitwise(const BigInt& a, const BigInt& b, int op, BigInt& out) {
  // Over the infinite two's-complement expansion. A negative value's limbs are
  // computed on the fly as `~magnitude + 1`, which is what "infinitely many
  // leading ones" means one limb at a time.
  const std::size_t size = std::max(a.limbs_.size(), b.limbs_.size()) + 1;
  if (size > kMaxBigIntLimbs) {
    return false;
  }
  const auto limb_of = [](const BigInt& value, std::size_t index, std::uint64_t& borrow) {
    const std::uint32_t raw = index < value.limbs_.size() ? value.limbs_[index] : 0;
    if (!value.negative_) {
      return raw;
    }
    // Two's complement, one limb at a time: invert and add the running carry.
    const std::uint64_t complemented = static_cast<std::uint64_t>(~raw) + borrow;
    borrow = complemented >> 32;
    return static_cast<std::uint32_t>(complemented & 0xFFFFFFFFull);
  };

  std::uint64_t borrow_a = 1;
  std::uint64_t borrow_b = 1;
  std::vector<std::uint32_t> result(size, 0);
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint32_t left = limb_of(a, i, borrow_a);
    const std::uint32_t right = limb_of(b, i, borrow_b);
    result[i] = op == 0 ? (left & right) : (op == 1 ? (left | right) : (left ^ right));
  }
  // The sign of the answer is what the operation does to the two sign bits,
  // which for an infinite expansion is what it does to the leading ones.
  const bool negative = op == 0   ? (a.negative_ && b.negative_)
                        : op == 1 ? (a.negative_ || b.negative_)
                                  : (a.negative_ != b.negative_);
  out = BigInt();
  if (!negative) {
    out.limbs_ = std::move(result);
    out.Trim();
    return true;
  }
  // Back from two's complement: negate the complemented result.
  std::uint64_t borrow = 1;
  for (std::uint32_t& limb : result) {
    const std::uint64_t complemented = static_cast<std::uint64_t>(~limb) + borrow;
    limb = static_cast<std::uint32_t>(complemented & 0xFFFFFFFFull);
    borrow = complemented >> 32;
  }
  out.limbs_ = std::move(result);
  out.negative_ = true;
  out.Trim();
  return true;
}

bool BigInt::Truncate(const BigInt& value, std::uint64_t bits, bool is_signed, BigInt& out) {
  if (bits == 0) {
    out = BigInt();
    return true;
  }
  if (bits > static_cast<std::uint64_t>(kMaxBigIntLimbs) * 32) {
    return false;
  }
  // The value modulo 2^bits, taken through the two's-complement expansion so
  // that a negative input wraps rather than reflecting.
  BigInt modulus;
  if (!ShiftLeft(FromInt64(1), static_cast<std::int64_t>(bits), modulus)) {
    return false;
  }
  BigInt quotient;
  BigInt remainder;
  if (!Divide(value, modulus, quotient, remainder)) {
    return false;
  }
  if (remainder.negative_) {
    BigInt wrapped;
    if (!Add(remainder, modulus, wrapped)) {
      return false;
    }
    remainder = std::move(wrapped);
  }
  if (!is_signed) {
    out = std::move(remainder);
    return true;
  }
  // Signed: anything at or above half the modulus is the negative of its
  // distance from the whole.
  BigInt half;
  if (!ShiftLeft(FromInt64(1), static_cast<std::int64_t>(bits - 1), half)) {
    return false;
  }
  if (Compare(remainder, half) >= 0) {
    BigInt lowered;
    if (!Subtract(remainder, modulus, lowered)) {
      return false;
    }
    out = std::move(lowered);
    return true;
  }
  out = std::move(remainder);
  return true;
}

}  // namespace microbrowser::js
