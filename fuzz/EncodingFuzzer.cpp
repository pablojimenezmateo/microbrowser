#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "html/Encoding.h"

// Bytes from a server, decoded.
//
// ADR 0025 §2. This is fuzzed for a *security* property rather than for memory safety, and the
// property is stated as the invariant every assertion below checks: **decoding never invents a
// syntactically significant character, and never deletes one.**
//
// Encoding confusion leading to XSS is the bug family: a decoder that emits a `<` where the
// specification says U+FFFD turns a sanitised document into a script-executing one, and one that
// *swallows* a byte hides the character a sanitiser was looking for. So:
//
//   * **Every ASCII byte in the input appears in the output**, in order, and no ASCII character
//     appears that was not in the input. For a single-byte encoding that is exact; for UTF-8 it holds
//     because every non-ASCII sequence decodes to a non-ASCII code point or to U+FFFD, and U+FFFD's
//     UTF-8 form contains no ASCII bytes at all.
//   * **The output is well-formed UTF-8.** Everything downstream -- the tokenizer, the DOM, the
//     JavaScript engine's string layer -- assumes it, and a decoder is the only place that can break
//     the assumption.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace microbrowser;
  if (size < 1) {
    return 0;
  }
  // The encoding out of the first byte, so every decoder is exercised rather than one.
  static constexpr html::Encoding kEncodings[] = {
      html::Encoding::Utf8,       html::Encoding::Windows1252, html::Encoding::Iso8859_2,
      html::Encoding::Iso8859_5,  html::Encoding::Iso8859_7,   html::Encoding::Iso8859_9,
      html::Encoding::Iso8859_15, html::Encoding::Utf16Le,     html::Encoding::Utf16Be,
  };
  const html::Encoding encoding = kEncodings[data[0] % (sizeof(kEncodings) / sizeof(kEncodings[0]))];
  const std::string_view bytes(reinterpret_cast<const char*>(data + 1), size - 1);

  // Sniffing must not crash on anything and must always answer.
  html::SniffEncoding(bytes, "text/html; charset=utf-8");
  html::SniffEncoding(bytes);

  const std::string decoded = html::DecodeToUtf8(bytes, encoding);

  // The output is well-formed UTF-8. Checked by decoding it *again* as UTF-8 and requiring the result
  // to be identical: a decoder is idempotent on its own output exactly when that output is
  // well-formed, which makes this one comparison stand in for a validator.
  //
  // **With an ASCII byte in front**, and that is not a workaround -- it is the format. A U+FEFF that
  // appeared mid-document decodes to the same three bytes as a leading BOM, so re-decoding a string
  // that *starts* with one strips it and the comparison fails on a perfectly well-formed output. The
  // fuzzer's first version trapped on exactly that (`fe ff fe ff e8 e8` as UTF-16BE), which was a bug
  // in this invariant rather than in the decoder.
  const std::string guarded = "x" + decoded;
  if (html::DecodeToUtf8(guarded, html::Encoding::Utf8) != guarded) {
    __builtin_trap();
  }

  // No ASCII character was invented. Every ASCII byte in the output must be traceable to an ASCII byte
  // in the input, in order -- and for the single-byte encodings the two sequences must be equal.
  const bool single_byte = encoding != html::Encoding::Utf8 &&
                           encoding != html::Encoding::Utf16Le &&
                           encoding != html::Encoding::Utf16Be;
  if (single_byte) {
    std::string input_ascii;
    for (const char c : bytes) {
      if (static_cast<unsigned char>(c) < 0x80) {
        input_ascii.push_back(c);
      }
    }
    std::string output_ascii;
    for (const char c : decoded) {
      if (static_cast<unsigned char>(c) < 0x80) {
        output_ascii.push_back(c);
      }
    }
    if (input_ascii != output_ascii) {
      __builtin_trap();
    }
  } else if (encoding == html::Encoding::Utf8) {
    // For UTF-8 the same holds, and the BOM is the one exception: it is consumed rather than decoded,
    // and it is not ASCII, so it cannot affect this comparison.
    std::string input_ascii;
    for (const char c : bytes.substr(html::BomLength(bytes))) {
      if (static_cast<unsigned char>(c) < 0x80) {
        input_ascii.push_back(c);
      }
    }
    std::string output_ascii;
    for (const char c : decoded) {
      if (static_cast<unsigned char>(c) < 0x80) {
        output_ascii.push_back(c);
      }
    }
    if (input_ascii != output_ascii) {
      __builtin_trap();
    }
  }
  return 0;
}
