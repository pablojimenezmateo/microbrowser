#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "js/Lexer.h"

// The JavaScript lexer, fed arbitrary bytes.
//
// A page serves script, so every byte here is attacker-controlled and the lexer
// is the first thing that touches it. Four properties, checked rather than
// merely survived:
//
//   1. Lexing terminates. Every path must consume at least one byte, or a
//      malformed literal is an infinite loop -- a denial of service reachable
//      by anyone who can serve a script tag.
//   2. The token count is bounded by the input length. A lexer that can emit
//      more tokens than bytes has a path that consumes nothing, which is the
//      same bug seen from the other side.
//   3. Every token's range lies inside the source and its lexeme is exactly
//      that range. Positions are what every error message points at, and a
//      token whose range escapes the buffer is an out-of-bounds read waiting
//      for whoever formats the message.
//   4. The stream always ends at EndOfFile, so a parser's error paths can stop
//      asking without a separate check.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view source(reinterpret_cast<const char*>(data), size);

  const std::vector<microbrowser::js::Token> tokens = microbrowser::js::TokenizeAll(source);
  if (tokens.empty() ||
      tokens.back().type != microbrowser::js::TokenType::EndOfFile) {
    __builtin_trap();
  }
  if (tokens.size() > size + 1) {
    __builtin_trap();
  }
  for (const microbrowser::js::Token& token : tokens) {
    if (token.start > token.end || token.end > size) {
      __builtin_trap();
    }
    if (token.type != microbrowser::js::TokenType::EndOfFile &&
        source.substr(token.start, token.end - token.start) != token.lexeme) {
      __builtin_trap();
    }
  }

  // The regex rescan is a second entry point into the same buffer, reached
  // whenever the parser wanted a literal where a slash was lexed. It has its
  // own termination condition and its own bounds.
  microbrowser::js::Lexer lexer(source);
  for (std::size_t guard = 0; guard <= size; ++guard) {
    const microbrowser::js::Token token = lexer.Next();
    if (token.type == microbrowser::js::TokenType::EndOfFile) {
      break;
    }
    if (token.IsPunctuator("/") || token.IsPunctuator("/=")) {
      const microbrowser::js::Token regex = lexer.RescanAsRegExp(token);
      if (regex.start > regex.end || regex.end > size) {
        __builtin_trap();
      }
    }
  }
  return 0;
}
