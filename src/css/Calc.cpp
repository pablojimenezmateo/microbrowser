#include "css/Calc.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "css/CssText.h"
#include "css/MediaQuery.h"
#include "util/Parse.h"

// `calc()`, `min()`, `max()` and `clamp()`, per CSS Values and Units 4 §8.
//
// Its own translation unit rather than another branch in Declarations.cpp,
// because it is a recursive-descent parser over attacker-controlled text and
// that is a different kind of thing from the table of properties which reads
// its answer. The module's line cap would have said the same eventually; this
// is the same reason stated earlier.
//
// `min`/`max`/`clamp` share the expression grammar with `calc`. Wikipedia's
// remaining failed length declarations after session 4 were almost all
// `calc(max(...))` / bare `max(...)` with rem and px — comparable absolutes
// once rem folds. Incomparable pairs (`min(50px, 70%)`, anything with `vw`)
// stay nullopt: a Length cannot hold two competing relative answers, and
// guessing would be worse than dropping the declaration.

namespace microbrowser::css {

namespace {

// How deep a parenthesised sub-expression may nest. A stylesheet is hostile
// input and this parser recurses over it, so the bound exists for the reason
// ADR 0009 gives for script and kMaxSelectorNestingDepth gives for selectors.
// Sixteen is far past anything an author writes — the deepest nesting in the
// twenty-three stylesheets measured for ADR 0016 was two — and far below the
// stack a recursive descent needs.
constexpr int kMaxCalcDepth = 16;

// A partially-evaluated calc expression, as the sum of the four kinds of term
// this engine can carry. Addition is componentwise, which is what makes the
// unit rules fall out rather than needing to be checked: `1px + 2%` is a legal
// sum with two components, and the question of whether it can be *stored* is
// asked once, at the end, by ToLength.
//
// Doubles rather than floats: an intermediate `100%/3` should not lose more
// than the final cast to float loses anyway.
struct Sum {
  double number = 0.0;   // unitless
  double px = 0.0;       // absolute: px, pt, and rem, which folds at the root font size
  double em = 0.0;       // coefficient of the element's font size
  double percent = 0.0;  // coefficient of the containing block

  bool IsPureNumber() const { return px == 0.0 && em == 0.0 && percent == 0.0; }
};

bool Finite(const Sum& sum) {
  return std::isfinite(sum.number) && std::isfinite(sum.px) && std::isfinite(sum.em) &&
         std::isfinite(sum.percent);
}

// A bare `0` is a length in CSS math functions. Anywhere else a non-zero
// unitless number is not comparable to a length, so leave those alone.
Sum CanonicalLengthZero(Sum sum) {
  if (sum.IsPureNumber() && sum.number == 0.0) {
    sum.px = 0.0;
  }
  return sum;
}

// -1 / 0 / 1 when both sides are resolved absolutes (px / rem / pt, and a bare
// `0`); nullopt when the comparison needs a used value. Relative-vs-relative
// (`min(10%, 20%)`) is deliberately refused too: a Length can store one answer,
// not a deferred comparison, and accepting `max(0, 10%)` as `10%` by treating
// zero as `0%` would be wrong. Wikipedia's failing forms are all rem/px.
std::optional<int> CompareSums(Sum left, Sum right) {
  left = CanonicalLengthZero(left);
  right = CanonicalLengthZero(right);
  if (left.number != 0.0 || right.number != 0.0 || left.em != 0.0 || right.em != 0.0 ||
      left.percent != 0.0 || right.percent != 0.0) {
    return std::nullopt;
  }
  if (left.px < right.px) {
    return -1;
  }
  if (left.px > right.px) {
    return 1;
  }
  return 0;
}

class CalcParser {
 public:
  CalcParser(std::string_view text, const MediaContext& context, float root_font_size)
      : text_(text), context_(context), root_font_size_(root_font_size) {}

  // One top-level math function — `calc(...)`, `min(...)`, `max(...)` or
  // `clamp(...)` — consuming the whole text.
  std::optional<Sum> ParseMathFunction() {
    SkipWhitespace();
    std::optional<Sum> sum;
    if (TakeFunction("calc")) {
      sum = ParseSum(0);
      SkipWhitespace();
      if (!sum.has_value() || !Peek(')')) {
        return std::nullopt;
      }
      ++at_;
    } else if (TakeFunction("min")) {
      sum = ParseMinMaxArgs(0, false);
    } else if (TakeFunction("max")) {
      sum = ParseMinMaxArgs(0, true);
    } else if (TakeFunction("clamp")) {
      sum = ParseClampArgs(0);
    } else {
      return std::nullopt;
    }
    SkipWhitespace();
    if (!sum.has_value() || at_ != text_.size()) {
      return std::nullopt;
    }
    return sum;
  }

 private:
  void SkipWhitespace() {
    while (at_ < text_.size() && IsCssWhitespace(text_[at_])) {
      ++at_;
    }
  }

  bool Peek(char c) const { return at_ < text_.size() && text_[at_] == c; }

  // True when `name(` begins at `at_`, case-insensitive. Advances past the
  // opening parenthesis on a match.
  bool TakeFunction(std::string_view name) {
    if (at_ + name.size() >= text_.size()) {
      return false;
    }
    if (Lowered(text_.substr(at_, name.size())) != name || text_[at_ + name.size()] != '(') {
      return false;
    }
    at_ += name.size() + 1;
    return true;
  }

  // `+` and `-` are only operators when surrounded by whitespace. The
  // requirement is in the specification because it is what separates
  // `calc(1px -2px)` — two values, a syntax error — from `calc(1px - 2px)`.
  // Dropping it would make `calc(100%-20px)` parse here and nowhere else.
  std::optional<char> TakeAdditiveOperator() {
    if (at_ >= text_.size() || !IsCssWhitespace(text_[at_])) {
      return std::nullopt;
    }
    std::size_t ahead = at_;
    while (ahead < text_.size() && IsCssWhitespace(text_[ahead])) {
      ++ahead;
    }
    if (ahead >= text_.size() || (text_[ahead] != '+' && text_[ahead] != '-')) {
      return std::nullopt;
    }
    if (ahead + 1 >= text_.size() || !IsCssWhitespace(text_[ahead + 1])) {
      return std::nullopt;  // `1px +2px`: the sign belongs to the number
    }
    const char op = text_[ahead];
    at_ = ahead + 1;
    return op;
  }

  std::optional<Sum> ParseMinMaxArgs(int depth, bool want_max) {
    std::optional<Sum> best;
    while (true) {
      const std::optional<Sum> arg = ParseSum(depth);
      if (!arg.has_value()) {
        return std::nullopt;
      }
      if (!best.has_value()) {
        best = arg;
      } else {
        const std::optional<int> cmp = CompareSums(*best, *arg);
        if (!cmp.has_value()) {
          return std::nullopt;
        }
        if (want_max ? *cmp < 0 : *cmp > 0) {
          best = arg;
        }
      }
      SkipWhitespace();
      if (Peek(')')) {
        ++at_;
        return Finite(*best) ? best : std::nullopt;
      }
      if (!Peek(',')) {
        return std::nullopt;
      }
      ++at_;
    }
  }

  std::optional<Sum> ParseClampArgs(int depth) {
    const std::optional<Sum> lower = ParseSum(depth);
    SkipWhitespace();
    if (!lower.has_value() || !Peek(',')) {
      return std::nullopt;
    }
    ++at_;
    const std::optional<Sum> value = ParseSum(depth);
    SkipWhitespace();
    if (!value.has_value() || !Peek(',')) {
      return std::nullopt;
    }
    ++at_;
    const std::optional<Sum> upper = ParseSum(depth);
    SkipWhitespace();
    if (!upper.has_value() || !Peek(')')) {
      return std::nullopt;
    }
    ++at_;
    // CSS Values 4: clamp(MIN, VAL, MAX) ≡ max(MIN, min(VAL, MAX)).
    const std::optional<int> inner = CompareSums(*value, *upper);
    if (!inner.has_value()) {
      return std::nullopt;
    }
    const Sum capped = *inner > 0 ? *upper : *value;
    const std::optional<int> outer = CompareSums(*lower, capped);
    if (!outer.has_value()) {
      return std::nullopt;
    }
    const Sum result = *outer > 0 ? *lower : capped;
    return Finite(result) ? std::optional<Sum>(result) : std::nullopt;
  }

  std::optional<Sum> ParseSum(int depth) {
    if (depth > kMaxCalcDepth) {
      return std::nullopt;
    }
    std::optional<Sum> left = ParseProduct(depth);
    if (!left.has_value()) {
      return std::nullopt;
    }
    while (const std::optional<char> op = TakeAdditiveOperator()) {
      const std::optional<Sum> right = ParseProduct(depth);
      if (!right.has_value()) {
        return std::nullopt;
      }
      const double sign = *op == '-' ? -1.0 : 1.0;
      left->number += sign * right->number;
      left->px += sign * right->px;
      left->em += sign * right->em;
      left->percent += sign * right->percent;
    }
    return Finite(*left) ? left : std::nullopt;
  }

  std::optional<Sum> ParseProduct(int depth) {
    std::optional<Sum> left = ParseValue(depth);
    if (!left.has_value()) {
      return std::nullopt;
    }
    while (true) {
      const std::size_t mark = at_;
      SkipWhitespace();
      if (!Peek('*') && !Peek('/')) {
        at_ = mark;
        return Finite(*left) ? left : std::nullopt;
      }
      const char op = text_[at_++];
      const std::optional<Sum> right = ParseValue(depth);
      if (!right.has_value()) {
        return std::nullopt;
      }
      if (op == '/') {
        // Only a number may divide, and only a non-zero one. Both are the
        // specification's rules and both produce an *invalid* value rather than
        // an approximation — a `calc(100%/0)` that guessed would put a box
        // somewhere nobody asked for.
        if (!right->IsPureNumber() || right->number == 0.0) {
          return std::nullopt;
        }
        left->number /= right->number;
        left->px /= right->number;
        left->em /= right->number;
        left->percent /= right->number;
        continue;
      }
      // One side of a multiplication must be a plain number: a length times a
      // length is an area, which is not a value CSS has.
      const Sum* scalar = right->IsPureNumber() ? &*right : (left->IsPureNumber() ? &*left : nullptr);
      if (scalar == nullptr) {
        return std::nullopt;
      }
      const Sum* other = scalar == &*right ? &*left : &*right;
      const double factor = scalar->number;
      Sum product;
      product.number = other->number * factor;
      product.px = other->px * factor;
      product.em = other->em * factor;
      product.percent = other->percent * factor;
      left = product;
    }
  }

  std::optional<Sum> ParseValue(int depth) {
    SkipWhitespace();
    if (at_ >= text_.size()) {
      return std::nullopt;
    }
    // Parentheses, nested `calc()`, and the other math functions that share
    // its grammar — `calc(max(1rem, 10px))` is what wikipedia writes.
    if (text_[at_] == '(') {
      ++at_;
      const std::optional<Sum> inner = ParseSum(depth + 1);
      SkipWhitespace();
      if (!inner.has_value() || !Peek(')')) {
        return std::nullopt;
      }
      ++at_;
      return inner;
    }
    if (TakeFunction("calc")) {
      const std::optional<Sum> inner = ParseSum(depth + 1);
      SkipWhitespace();
      if (!inner.has_value() || !Peek(')')) {
        return std::nullopt;
      }
      ++at_;
      return inner;
    }
    if (TakeFunction("min")) {
      return ParseMinMaxArgs(depth + 1, false);
    }
    if (TakeFunction("max")) {
      return ParseMinMaxArgs(depth + 1, true);
    }
    if (TakeFunction("clamp")) {
      return ParseClampArgs(depth + 1);
    }
    return ParseNumeric();
  }

  // A CSS number — sign, digits, fraction, exponent — followed by a unit, a
  // `%`, or nothing.
  std::optional<Sum> ParseNumeric() {
    bool negative = false;
    if (Peek('+') || Peek('-')) {
      negative = text_[at_] == '-';
      ++at_;
    }
    // The sign is taken above rather than handed to the number parser, which
    // rejects a leading `+` on purpose — see util::ParseRealExact.
    const std::size_t start = at_;
    bool any_digit = false;
    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
      ++at_;
      any_digit = true;
    }
    if (Peek('.')) {
      ++at_;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
        any_digit = true;
      }
    }
    if (!any_digit) {
      return std::nullopt;
    }
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      const std::size_t mark = at_;
      ++at_;
      if (Peek('+') || Peek('-')) {
        ++at_;
      }
      bool exponent_digit = false;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
        exponent_digit = true;
      }
      if (!exponent_digit) {
        at_ = mark;  // `1em` is not `1` with a broken exponent
      }
    }
    // The scan above has already decided which characters the number is, so
    // this only turns them into a double — and rejects the ones that are out
    // of range, which is not a value anybody can lay out.
    const std::optional<double> parsed =
        util::ParseDouble(text_.substr(start, at_ - start));
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    const double magnitude = negative ? -*parsed : *parsed;

    const std::size_t unit_start = at_;
    if (Peek('%')) {
      ++at_;
    } else {
      while (at_ < text_.size() && ((text_[at_] >= 'a' && text_[at_] <= 'z') ||
                                    (text_[at_] >= 'A' && text_[at_] <= 'Z'))) {
        ++at_;
      }
    }
    const std::string unit = Lowered(text_.substr(unit_start, at_ - unit_start));

    Sum sum;
    if (unit.empty()) {
      sum.number = magnitude;
    } else if (unit == "%") {
      sum.percent = magnitude;
    } else if (unit == "px") {
      sum.px = magnitude;
    } else if (unit == "rem") {
      sum.px = magnitude * static_cast<double>(root_font_size_);
    } else if (unit == "em") {
      sum.em = magnitude;
    } else if (const std::optional<float> absolute =
                   AbsoluteLengthFromUnit(magnitude, unit, context_)) {
      sum.px = static_cast<double>(*absolute);
    } else {
      // `ch`, `ex` and the rest, or a viewport unit before the viewport is known.
      return std::nullopt;
    }
    return Finite(sum) ? std::optional<Sum>(sum) : std::nullopt;
  }

  std::string_view text_;
  const MediaContext& context_;
  float root_font_size_ = kRootFontSize;
  std::size_t at_ = 0;
};

// The sum, as something a Length can hold. See Calc.h for why the three
// failures below are one answer.
std::optional<Length> ToLength(const Sum& sum) {
  if (sum.number != 0.0) {
    return std::nullopt;  // `calc(1px + 2)` mixes a length with a bare number
  }
  const bool has_percent = sum.percent != 0.0;
  const bool has_em = sum.em != 0.0;
  if (has_percent && has_em) {
    return std::nullopt;
  }
  // Checked against a float's range *before* the cast: casting first and asking
  // afterwards is undefined for a double that does not fit, and the comparison
  // also rejects a NaN that arrived as 0/0.
  const auto narrow = [](double value) -> std::optional<float> {
    constexpr double kFloatLimit = 3.0e38;
    if (!(value >= -kFloatLimit && value <= kFloatLimit)) {
      return std::nullopt;
    }
    return static_cast<float>(value);
  };
  const std::optional<float> absolute = narrow(sum.px);
  if (!absolute.has_value()) {
    return std::nullopt;
  }
  if (has_percent || has_em) {
    const std::optional<float> relative = narrow(has_percent ? sum.percent : sum.em);
    if (!relative.has_value()) {
      return std::nullopt;
    }
    return Length{*relative, has_percent ? Length::Unit::Percent : Length::Unit::Em, *absolute};
  }
  return Length::Pixels(*absolute);
}

}  // namespace

std::optional<Length> ParseCalc(std::string_view text, const MediaContext& context,
                               float root_font_size) {
  const std::string_view trimmed = Trim(text);
  if (trimmed.size() < 4) {
    return std::nullopt;
  }
  CalcParser parser(trimmed, context, root_font_size);
  const std::optional<Sum> sum = parser.ParseMathFunction();
  if (!sum.has_value()) {
    return std::nullopt;
  }
  return ToLength(*sum);
}

}  // namespace microbrowser::css
