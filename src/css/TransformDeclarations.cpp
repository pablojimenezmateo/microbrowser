// `transform` and `transform-origin`.
//
// ADR 0014 §4, and its own translation unit because it is its own small grammar:
// eleven functions with argument counts and units that differ per function, and an
// error rule that is unlike the rest of CSS. **A `transform` with one unparsable
// function is dropped entirely**, not partially applied -- `translate(10px)
// rotate(oops)` moves nothing. That is the specification, and it is also the safe
// direction: half a transform positions a box somewhere the author never wrote.

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/Tokenizer.h"
#include "css/Transform.h"
#include "util/StringUtil.h"

namespace microbrowser::css {

namespace {

using util::AsciiLowerCase;

constexpr float kPi = 3.14159265358979323846f;

// An angle, in radians. The four units are all in real stylesheets: `deg` by a wide
// margin, `turn` in animation-heavy CSS, `rad` and `grad` rarely -- and a unit this
// parser did not know would otherwise read as a bare number, which for `rotate()`
// means radians and would silently rotate by a fifty-seventh of what was written.
std::optional<float> ParseAngle(const Token& token) {
  if (token.kind == Token::Kind::Number && token.number == 0.0) {
    return 0.0f;  // a bare zero is a legal angle, and the only legal bare number
  }
  if (token.kind != Token::Kind::Dimension) {
    return std::nullopt;
  }
  const std::string unit = AsciiLowerCase(token.value);
  const float value = static_cast<float>(token.number);
  if (unit == "deg") {
    return value * kPi / 180.0f;
  }
  if (unit == "rad") {
    return value;
  }
  if (unit == "turn") {
    return value * 2.0f * kPi;
  }
  if (unit == "grad") {
    return value * kPi / 200.0f;
  }
  return std::nullopt;
}

std::optional<float> ParseNumber(const Token& token) {
  if (token.kind != Token::Kind::Number) {
    return std::nullopt;
  }
  return static_cast<float>(token.number);
}

// A length or a percentage, for a translation. `em` is kept as `em` rather than
// resolved, because the font size a transform's `em` means is the transformed
// element's own -- which this layer has, but the box that resolves the matrix has
// too, and one resolution point is fewer than two.
std::optional<Length> ParseTranslation(const Token& token) {
  if (token.kind == Token::Kind::Number && token.number == 0.0) {
    return Length::Pixels(0.0f);
  }
  if (token.kind == Token::Kind::Percentage) {
    return Length{static_cast<float>(token.number), Length::Unit::Percent};
  }
  if (token.kind != Token::Kind::Dimension) {
    return std::nullopt;
  }
  const std::string unit = AsciiLowerCase(token.value);
  const float value = static_cast<float>(token.number);
  if (unit == "px") {
    return Length::Pixels(value);
  }
  if (unit == "em") {
    return Length{value, Length::Unit::Em};
  }
  if (unit == "rem") {
    return Length{value, Length::Unit::Rem};
  }
  // Every other absolute unit, through the same conversions the rest of the cascade
  // uses. A `pt` in a transform is rare and is not a reason for a second table.
  if (unit == "pt") {
    return Length::Pixels(value * 96.0f / 72.0f);
  }
  if (unit == "pc") {
    return Length::Pixels(value * 16.0f);
  }
  if (unit == "in") {
    return Length::Pixels(value * 96.0f);
  }
  if (unit == "cm") {
    return Length::Pixels(value * 96.0f / 2.54f);
  }
  if (unit == "mm") {
    return Length::Pixels(value * 96.0f / 25.4f);
  }
  return std::nullopt;
}

// The tokens of one function's arguments, with commas dropped. Commas are not
// checked, and that is deliberate rather than lazy: `translate(10px 20px)` is
// invalid CSS, and the cost of accepting it is nothing -- while the cost of a comma
// state machine here is a second place for the argument count to be decided.
std::vector<Token> Arguments(const std::vector<Token>& tokens, std::size_t& at) {
  std::vector<Token> arguments;
  int depth = 1;
  ++at;  // past the Function token
  while (at < tokens.size() && depth > 0) {
    const Token& token = tokens[at];
    if (token.kind == Token::Kind::LeftParen || token.kind == Token::Kind::Function) {
      ++depth;
    } else if (token.kind == Token::Kind::RightParen) {
      --depth;
      if (depth == 0) {
        ++at;
        break;
      }
    }
    if (token.kind != Token::Kind::Whitespace && token.kind != Token::Kind::Comma) {
      arguments.push_back(token);
    }
    ++at;
  }
  return arguments;
}

bool ParseFunction(std::string_view name, const std::vector<Token>& arguments,
                   TransformOperation& out) {
  const auto translation = [&arguments](std::size_t index) -> std::optional<Length> {
    return index < arguments.size() ? ParseTranslation(arguments[index]) : std::nullopt;
  };
  const auto number = [&arguments](std::size_t index) -> std::optional<float> {
    return index < arguments.size() ? ParseNumber(arguments[index]) : std::nullopt;
  };

  if (name == "translate" || name == "translatex" || name == "translatey") {
    const std::size_t wanted = name == "translate" ? 0u : 1u;
    if (arguments.empty() || arguments.size() > (wanted == 0u ? 2u : 1u)) {
      return false;
    }
    const std::optional<Length> first = translation(0);
    if (!first.has_value()) {
      return false;
    }
    out.kind = TransformOperation::Kind::Translate;
    if (name == "translatey") {
      out.length_x = Length::Pixels(0.0f);
      out.length_y = *first;
      return true;
    }
    out.length_x = *first;
    // `translate(10px)` is `translate(10px, 0)`: the missing axis is zero, not the
    // same value. Getting that wrong moves every single-argument translation
    // diagonally, which looks like a layout bug rather than a parse bug.
    out.length_y = Length::Pixels(0.0f);
    if (arguments.size() == 2) {
      const std::optional<Length> second = translation(1);
      if (!second.has_value()) {
        return false;
      }
      out.length_y = *second;
    }
    return true;
  }
  if (name == "scale" || name == "scalex" || name == "scaley") {
    if (arguments.empty() || arguments.size() > (name == "scale" ? 2u : 1u)) {
      return false;
    }
    const std::optional<float> first = number(0);
    if (!first.has_value()) {
      return false;
    }
    out.kind = TransformOperation::Kind::Scale;
    if (name == "scalex") {
      out.a = *first;
      out.b = 1.0f;
      return true;
    }
    if (name == "scaley") {
      out.a = 1.0f;
      out.b = *first;
      return true;
    }
    out.a = *first;
    // `scale(2)` is uniform -- unlike `translate`, where the missing argument is
    // zero. The two functions disagree, and this is the line that says so.
    out.b = *first;
    if (arguments.size() == 2) {
      const std::optional<float> second = number(1);
      if (!second.has_value()) {
        return false;
      }
      out.b = *second;
    }
    return true;
  }
  if (name == "rotate" || name == "rotatez") {
    if (arguments.size() != 1) {
      return false;
    }
    const std::optional<float> radians = ParseAngle(arguments[0]);
    if (!radians.has_value()) {
      return false;
    }
    out.kind = TransformOperation::Kind::Rotate;
    out.a = *radians;
    return true;
  }
  if (name == "skew" || name == "skewx" || name == "skewy") {
    if (arguments.empty() || arguments.size() > (name == "skew" ? 2u : 1u)) {
      return false;
    }
    const std::optional<float> first = ParseAngle(arguments[0]);
    if (!first.has_value()) {
      return false;
    }
    out.kind = TransformOperation::Kind::Skew;
    out.a = name == "skewy" ? 0.0f : *first;
    out.b = name == "skewy" ? *first : 0.0f;
    if (name == "skew" && arguments.size() == 2) {
      const std::optional<float> second = ParseAngle(arguments[1]);
      if (!second.has_value()) {
        return false;
      }
      out.b = *second;
    }
    return true;
  }
  if (name == "matrix") {
    if (arguments.size() != 6) {
      return false;
    }
    float values[6] = {};
    for (std::size_t i = 0; i < 6; ++i) {
      const std::optional<float> value = number(i);
      if (!value.has_value()) {
        return false;
      }
      values[i] = *value;
    }
    out.kind = TransformOperation::Kind::Matrix;
    out.a = values[0];
    out.b = values[1];
    out.c = values[2];
    out.d = values[3];
    out.e = values[4];
    out.f = values[5];
    return true;
  }
  // Everything 3D -- `translate3d`, `rotateX`, `perspective`, `matrix3d` -- is
  // *refused* rather than flattened. Flattening is what a 2D engine is tempted to
  // do, and `rotateY(90deg)` flattened to 2D is a box at full width where the page
  // meant an edge-on sliver: a wrong page rather than a missing effect. A dropped
  // `transform` leaves the box where layout put it, which is the honest answer.
  return false;
}

}  // namespace

bool ApplyTransformDeclaration(std::string_view property, std::string_view value,
                               const ComputedStyle& parent, ComputedStyle& style) {
  if (property == "transform") {
    const std::string lowered = AsciiLowerCase(value);
    if (lowered == "none") {
      style.transform = TransformList{};
      return true;
    }
    if (lowered == "inherit") {
      style.transform = parent.transform;
      return true;
    }
    TransformList parsed;
    const std::vector<Token> tokens = Tokenize(value);
    std::size_t at = 0;
    while (at < tokens.size() && tokens[at].kind != Token::Kind::EndOfFile) {
      if (tokens[at].kind == Token::Kind::Whitespace) {
        ++at;
        continue;
      }
      if (tokens[at].kind != Token::Kind::Function) {
        return false;
      }
      const std::string name = AsciiLowerCase(tokens[at].value);
      const std::vector<Token> arguments = Arguments(tokens, at);
      TransformOperation operation;
      if (!ParseFunction(name, arguments, operation)) {
        return false;  // one bad function drops the whole declaration
      }
      parsed.operations.push_back(operation);
    }
    if (parsed.operations.empty()) {
      return false;
    }
    style.transform = std::move(parsed);
    return true;
  }

  if (property == "transform-origin") {
    // One or two components. The three-value form's third component is a `z`
    // offset, which a 2D engine has nowhere to put -- so it is refused rather than
    // ignored, for the same reason `translate3d` is.
    const std::vector<Token> tokens = Tokenize(value);
    std::vector<Token> parts;
    for (const Token& token : tokens) {
      if (token.kind != Token::Kind::Whitespace && token.kind != Token::Kind::EndOfFile) {
        parts.push_back(token);
      }
    }
    if (parts.empty() || parts.size() > 2) {
      return false;
    }
    const auto component = [](const Token& token, bool vertical) -> std::optional<Length> {
      if (token.kind == Token::Kind::Ident) {
        const std::string keyword = AsciiLowerCase(token.value);
        if (keyword == "center") {
          return Length{50.0f, Length::Unit::Percent};
        }
        if (keyword == (vertical ? "top" : "left")) {
          return Length{0.0f, Length::Unit::Percent};
        }
        if (keyword == (vertical ? "bottom" : "right")) {
          return Length{100.0f, Length::Unit::Percent};
        }
        return std::nullopt;
      }
      return ParseTranslation(token);
    };
    const std::optional<Length> x = component(parts[0], false);
    if (!x.has_value()) {
      return false;
    }
    std::optional<Length> y = Length{50.0f, Length::Unit::Percent};
    if (parts.size() == 2) {
      y = component(parts[1], true);
      if (!y.has_value()) {
        return false;
      }
    }
    style.transform_origin_x = *x;
    style.transform_origin_y = *y;
    return true;
  }
  return false;
}

}  // namespace microbrowser::css
