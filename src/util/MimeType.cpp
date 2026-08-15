#include "util/MimeType.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/StringUtil.h"

namespace microbrowser::util {
namespace {

bool IsHttpWhitespace(std::uint32_t c) {
  return c == 0x09 || c == 0x0A || c == 0x0D || c == 0x20;
}

bool IsHttpTokenCode(std::uint32_t c) {
  // MIME Sniffing §3: "!", "#", "$", "%", "&", "'", "*", "+", "-", ".", "^",
  // "_", "`", "|", "~", or an ASCII alphanumeric.
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsHttpQuotedStringCode(std::uint32_t c) {
  return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
}

std::uint32_t AsciiLower(std::uint32_t c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

std::vector<std::uint32_t> ToCodes(std::string_view text) {
  std::vector<std::uint32_t> codes;
  codes.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    std::uint32_t code = 0;
    if (DecodeUtf8(text, at, code)) {
      codes.push_back(code);
      continue;
    }
    // A malformed byte is that byte as a code point, which is how Fetch's
    // isomorphic decode treats a header field. Skipping it would hide a
    // character a MIME type is defined to reject.
    codes.push_back(static_cast<unsigned char>(text[at]));
    ++at;
  }
  return codes;
}

std::string FromCodes(const std::vector<std::uint32_t>& codes, std::size_t begin, std::size_t end) {
  std::string out;
  for (std::size_t i = begin; i < end; ++i) {
    AppendUtf8(out, codes[i]);
  }
  return out;
}

std::string FromCodes(const std::vector<std::uint32_t>& codes) {
  return FromCodes(codes, 0, codes.size());
}

bool AllToken(const std::vector<std::uint32_t>& codes) {
  if (codes.empty()) {
    return false;
  }
  for (const std::uint32_t c : codes) {
    if (!IsHttpTokenCode(c)) {
      return false;
    }
  }
  return true;
}

bool AllQuotedString(const std::vector<std::uint32_t>& codes) {
  for (const std::uint32_t c : codes) {
    if (!IsHttpQuotedStringCode(c)) {
      return false;
    }
  }
  return true;
}

void LowerInPlace(std::vector<std::uint32_t>& codes) {
  for (std::uint32_t& c : codes) {
    c = AsciiLower(c);
  }
}

void TrimTrailingHttpWhitespace(std::vector<std::uint32_t>& codes) {
  while (!codes.empty() && IsHttpWhitespace(codes.back())) {
    codes.pop_back();
  }
}

void SkipHttpWhitespace(const std::vector<std::uint32_t>& input, std::size_t& position) {
  while (position < input.size() && IsHttpWhitespace(input[position])) {
    ++position;
  }
}

std::vector<std::uint32_t> CollectUntil(const std::vector<std::uint32_t>& input, std::size_t& position,
                                        std::uint32_t stop_a, std::uint32_t stop_b) {
  const std::size_t begin = position;
  while (position < input.size() && input[position] != stop_a && input[position] != stop_b) {
    ++position;
  }
  return std::vector<std::uint32_t>(input.begin() + static_cast<std::ptrdiff_t>(begin),
                                    input.begin() + static_cast<std::ptrdiff_t>(position));
}

std::vector<std::uint32_t> CollectUntil(const std::vector<std::uint32_t>& input, std::size_t& position,
                                        std::uint32_t stop) {
  return CollectUntil(input, position, stop, stop);
}

// Fetch "collect an HTTP quoted string" with extract-value true. `position`
// starts on the opening quote and ends after the closing one (or at EOF).
std::vector<std::uint32_t> CollectQuoted(const std::vector<std::uint32_t>& input, std::size_t& position) {
  std::vector<std::uint32_t> value;
  ++position;  // skip opening "
  while (true) {
    while (position < input.size() && input[position] != '"' && input[position] != '\\') {
      value.push_back(input[position]);
      ++position;
    }
    if (position >= input.size()) {
      break;
    }
    const std::uint32_t quote_or_backslash = input[position];
    ++position;
    if (quote_or_backslash == '\\') {
      if (position >= input.size()) {
        value.push_back('\\');
        break;
      }
      value.push_back(input[position]);
      ++position;
      continue;
    }
    break;  // closing "
  }
  return value;
}

bool ParameterNameExists(const MimeType& mime, std::string_view name) {
  for (const auto& parameter : mime.parameters) {
    if (parameter.first == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<MimeType> ParseMimeType(std::string_view input) {
  // MIME Sniffing Standard §4.4.
  std::vector<std::uint32_t> codes = ToCodes(input);
  while (!codes.empty() && IsHttpWhitespace(codes.back())) {
    codes.pop_back();
  }
  std::size_t position = 0;
  SkipHttpWhitespace(codes, position);

  std::vector<std::uint32_t> type = CollectUntil(codes, position, '/');
  if (!AllToken(type) || position >= codes.size()) {
    return std::nullopt;
  }
  ++position;  // skip /
  std::vector<std::uint32_t> subtype = CollectUntil(codes, position, ';');
  TrimTrailingHttpWhitespace(subtype);
  if (!AllToken(subtype)) {
    return std::nullopt;
  }
  LowerInPlace(type);
  LowerInPlace(subtype);

  MimeType mime;
  mime.type = FromCodes(type);
  mime.subtype = FromCodes(subtype);

  while (position < codes.size()) {
    ++position;  // skip ;
    SkipHttpWhitespace(codes, position);
    std::vector<std::uint32_t> parameter_name = CollectUntil(codes, position, ';', '=');
    LowerInPlace(parameter_name);
    std::vector<std::uint32_t> parameter_value;
    bool have_value = false;
    if (position < codes.size()) {
      if (codes[position] == ';') {
        continue;
      }
      ++position;  // skip =
      if (position >= codes.size()) {
        break;
      }
      if (codes[position] == '"') {
        parameter_value = CollectQuoted(codes, position);
        CollectUntil(codes, position, ';');  // discard remainder of the parameter
        have_value = true;
      } else {
        parameter_value = CollectUntil(codes, position, ';');
        TrimTrailingHttpWhitespace(parameter_value);
        if (parameter_value.empty()) {
          continue;
        }
        have_value = true;
      }
    }
    if (!have_value) {
      continue;
    }
    if (parameter_name.empty() || !AllToken(parameter_name) || !AllQuotedString(parameter_value)) {
      continue;
    }
    const std::string name = FromCodes(parameter_name);
    if (ParameterNameExists(mime, name)) {
      continue;
    }
    mime.parameters.emplace_back(name, FromCodes(parameter_value));
  }
  return mime;
}

std::string SerializeMimeType(const MimeType& mime) {
  // MIME Sniffing Standard §4.5.
  std::string serialization = mime.type + "/" + mime.subtype;
  for (const auto& parameter : mime.parameters) {
    serialization.push_back(';');
    serialization += parameter.first;
    serialization.push_back('=');
    const std::vector<std::uint32_t> value_codes = ToCodes(parameter.second);
    if (value_codes.empty() || !AllToken(value_codes)) {
      std::string quoted = "\"";
      for (const std::uint32_t c : value_codes) {
        if (c == '"' || c == '\\') {
          quoted.push_back('\\');
        }
        AppendUtf8(quoted, c);
      }
      quoted.push_back('"');
      serialization += quoted;
    } else {
      serialization += parameter.second;
    }
  }
  return serialization;
}

std::string BlobMimeType(std::string_view input) {
  const std::optional<MimeType> parsed = ParseMimeType(input);
  return parsed.has_value() ? SerializeMimeType(*parsed) : std::string();
}

bool IsHttpToken(std::string_view text) { return AllToken(ToCodes(text)); }

bool IsHttpHeaderValue(std::string_view text) {
  // Fetch: a header value is a byte sequence with no 0x00 / 0x0A / 0x0D.
  // A JS string with a code point above 0xFF cannot be isomorphic-encoded.
  const std::vector<std::uint32_t> codes = ToCodes(text);
  for (const std::uint32_t c : codes) {
    if (c > 0xFF || c == 0x00 || c == 0x0A || c == 0x0D) {
      return false;
    }
  }
  return true;
}

}  // namespace microbrowser::util
