#include "js/BigInt.h"
#include "js/Heap.h"
#include "js/Value.h"

// The seam between the arithmetic in BigInt.cpp and the value system.
//
// A bigint's digits live beside the heap, keyed by the cell that is its
// identity -- the arrangement a symbol has, and for the same reason: the cell
// is what the collector knows how to free, and the digits have to go when it
// does. Value cannot see the heap, so these are free functions rather than
// methods, and the cell carries a pointer to its own digits so a lookup here
// costs a load rather than a hash.

namespace microbrowser::js {

const BigInt* BigIntOf(const Value& value) {
  return value.IsBigInt() ? value.object->BigIntDigits() : nullptr;
}

bool IsBigIntZero(const Value& value) {
  const BigInt* digits = BigIntOf(value);
  return digits == nullptr || digits->IsZero();
}

double BigIntValueOf(const Value& value) {
  const BigInt* digits = BigIntOf(value);
  return digits == nullptr ? 0.0 : digits->ToDouble();
}

std::string BigIntText(const Value& value) {
  const BigInt* digits = BigIntOf(value);
  return digits == nullptr ? std::string("0") : digits->ToString();
}

bool BigIntEquals(const Value& a, const Value& b) {
  const BigInt* left = BigIntOf(a);
  const BigInt* right = BigIntOf(b);
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  return *left == *right;
}

bool BigIntEqualsNumber(const Value& value, double number) {
  const BigInt* digits = BigIntOf(value);
  return digits != nullptr && BigInt::CompareDouble(*digits, number) == 0;
}

bool BigIntEqualsText(const Value& value, const std::string& text) {
  // `1n == '1'` is true: the string is read as a bigint literal, and a string
  // that is not one makes the comparison false rather than an error.
  BigInt parsed;
  if (!BigInt::Parse(text, parsed)) {
    return false;
  }
  const BigInt* digits = BigIntOf(value);
  return digits != nullptr && *digits == parsed;
}

}  // namespace microbrowser::js
