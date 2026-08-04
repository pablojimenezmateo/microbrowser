#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::js {

// An arbitrary-precision integer.
//
// The language's second numeric type, and it is a *type* rather than a library:
// `typeof 1n` is "bigint", `1n + 1` is a TypeError, and every operator has a
// case for it. That is why this is a value here rather than an object with
// methods -- half a type is worse than none, because a page cannot tell which
// half it has.
//
// Sign and magnitude, in base 2^32. Not two's complement: the language's
// bigints are unbounded, so there is no width for a sign bit to live in, and
// the bitwise operators are defined over the *infinite* two's-complement
// expansion -- which is a thing you compute from sign and magnitude rather than
// store. Base 2^32 because a 64-bit product of two limbs is exact and needs no
// carry gymnastics.
//
// Every operation here is bounded by the operands' size and by kMaxLimbs, which
// is what stops `2n ** (2n ** 64n)` from being an allocation the size of the
// machine. A page can write that.
class BigInt {
 public:
  BigInt() = default;

  static BigInt FromInt64(std::int64_t value);
  // From a double that is an exact integer. False when it is not one, which is
  // the TypeError `BigInt(1.5)` throws.
  static bool FromDouble(double value, BigInt& out);
  // From source text: decimal, or `0x`/`0o`/`0b` with the matching digits, with
  // optional leading whitespace and sign. False when the text is not an
  // integer literal, which is the SyntaxError `BigInt('x')` throws.
  static bool Parse(std::string_view text, BigInt& out);

  bool IsZero() const { return limbs_.empty(); }
  bool Negative() const { return negative_; }
  // Whether it fits a double exactly, and the value if so. Used by the
  // comparisons, which are the one place a bigint and a number may meet.
  double ToDouble() const;
  std::string ToString(int radix = 10) const;

  // The comparisons. Three-way, because `<`, `>`, `<=`, `>=` and `==` all want
  // a different half of the same answer.
  static int Compare(const BigInt& a, const BigInt& b);
  // Against a double, which is what `1n < 1.5` needs. NaN answers 2, which is
  // "unordered" -- every comparison against it is false, and a caller that
  // treats it as a number would get one of them wrong.
  static int CompareDouble(const BigInt& a, double b);

  // The arithmetic. False when the result would exceed kMaxLimbs, or on a
  // division by zero -- both of which the caller turns into a thrown error
  // rather than a silent answer.
  static bool Add(const BigInt& a, const BigInt& b, BigInt& out);
  static bool Subtract(const BigInt& a, const BigInt& b, BigInt& out);
  static bool Multiply(const BigInt& a, const BigInt& b, BigInt& out);
  // Truncating, like the language's: `-7n / 2n` is `-3n`, and the remainder
  // takes the sign of the dividend.
  static bool Divide(const BigInt& a, const BigInt& b, BigInt& quotient,
                     BigInt& remainder);
  static bool Power(const BigInt& base, const BigInt& exponent, BigInt& out);
  static bool ShiftLeft(const BigInt& a, std::int64_t by, BigInt& out);

  // The bitwise operators, over the infinite two's-complement expansion. `0`
  // is AND, `1` is OR, `2` is XOR -- three cases of one loop, because the only
  // difference is which of the eight combinations of two bits is set.
  static bool Bitwise(const BigInt& a, const BigInt& b, int op, BigInt& out);
  static BigInt Negate(const BigInt& value);
  // `~x`, which is `-x - 1` and is written as that rather than as a loop over
  // bits -- the two's-complement identity is exact for an unbounded integer.
  static bool Not(const BigInt& value, BigInt& out);

  // `BigInt.asIntN` and `asUintN`: the value modulo 2^bits, read as signed or
  // unsigned. What a page uses to make a bigint behave like a fixed-width
  // integer, which is most of what they are used for.
  static bool Truncate(const BigInt& value, std::uint64_t bits, bool is_signed, BigInt& out);

  bool operator==(const BigInt& other) const {
    return negative_ == other.negative_ && limbs_ == other.limbs_;
  }

 private:
  // Least significant first, with no trailing zero limbs -- which is what makes
  // `limbs_ == other.limbs_` a correct equality and `limbs_.empty()` mean zero.
  std::vector<std::uint32_t> limbs_;
  // Never true when `limbs_` is empty: there is one zero, and it is positive.
  bool negative_ = false;

  void Trim();
  static int CompareMagnitude(const BigInt& a, const BigInt& b);
  static bool AddMagnitude(const BigInt& a, const BigInt& b, BigInt& out);
  // `a - b`, requiring |a| >= |b|.
  static void SubtractMagnitude(const BigInt& a, const BigInt& b, BigInt& out);
};

// Sixteen thousand limbs is half a megabyte of digits, which is far past any
// real use and short of anything that would hurt. A page can write
// `2n ** 100000000n`, and the answer has to be a RangeError rather than an
// allocation the size of the machine.
inline constexpr std::size_t kMaxBigIntLimbs = 16384;

}  // namespace microbrowser::js
