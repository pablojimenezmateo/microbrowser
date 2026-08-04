#include "css/Calc.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "css/CssText.h"
#include "util/Parse.h"

// `calc()`, per CSS Values and Units 3 §8.
//
// Its own translation unit rather than another branch in Declarations.cpp,
// because it is a recursive-descent parser over attacker-controlled text and
// that is a different kind of thing from the table of properties which reads
// its answer. The module's line cap would have said the same eventually; this
// is the same reason stated earlier.

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
  double px = 0.0;       // absolute: px, pt, and rem, which folds at kRootFontSize
  double em = 0.0;       // coefficient of the element's font size
  double percent = 0.0;  // coefficient of the containing block

  bool IsPureNumber() const { return px == 0.0 && em == 0.0 && percent == 0.0; }
};

bool Finite(const Sum& sum) {
  return std::isfinite(sum.number) && std::isfinite(sum.px) && std::isfinite(sum.em) &&
         std::isfinite(sum.percent);
}

class CalcParser {
 public:
  explicit CalcParser(std::string_view text) : text_(text) {}

  // The whole expression, which must consume the text: trailing junk is a
  // syntax error rather than something to ignore, or `calc(1px) blue` would
  // read as a length.
  std::optional<Sum> ParseAll() {
    const std::optional<Sum> sum = ParseSum(0);
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
    // A parenthesised sum, and the nested `calc()` the specification also
    // allows — `calc(calc(1px) * 2)` is legal and means what it looks like.
    std::size_t open = std::string_view::npos;
    if (text_[at_] == '(') {
      open = at_ + 1;
    } else if (Lowered(text_.substr(at_, 5)) == "calc(") {
      open = at_ + 5;
    }
    if (open != std::string_view::npos) {
      at_ = open;
      const std::optional<Sum> inner = ParseSum(depth + 1);
      SkipWhitespace();
      if (!inner.has_value() || !Peek(')')) {
        return std::nullopt;
      }
      ++at_;
      return inner;
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
    } else if (unit == "pt") {
      sum.px = magnitude * 4.0 / 3.0;
    } else if (unit == "rem") {
      sum.px = magnitude * static_cast<double>(kRootFontSize);
    } else if (unit == "em") {
      sum.em = magnitude;
    } else {
      // `vw`, `ch`, `ex` and the rest. Not supported, so the calc is invalid
      // and the declaration is dropped — which is the same outcome the unit
      // gets outside a calc, and is why `@supports (width: 1vw)` answers no.
      return std::nullopt;
    }
    return Finite(sum) ? std::optional<Sum>(sum) : std::nullopt;
  }

  std::string_view text_;
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

std::optional<Length> ParseCalc(std::string_view text) {
  const std::string_view trimmed = Trim(text);
  if (trimmed.size() < 6 || trimmed.back() != ')' ||
      Lowered(trimmed.substr(0, 5)) != "calc(") {
    return std::nullopt;
  }
  CalcParser parser(trimmed.substr(5, trimmed.size() - 6));
  const std::optional<Sum> sum = parser.ParseAll();
  if (!sum.has_value()) {
    return std::nullopt;
  }
  return ToLength(*sum);
}

}  // namespace microbrowser::css
