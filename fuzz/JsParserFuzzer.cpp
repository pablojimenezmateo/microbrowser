#include <cstddef>
#include <cstdint>
#include <string_view>

#include "js/Parser.h"

namespace {

// Depth of the produced tree. Checked rather than assumed: the parser's depth
// guard bounds *recursion*, and a tree deeper than that guard would mean a path
// that recurses without going through it -- which is the same stack overflow
// arriving later, in whoever walks the tree.
std::size_t TreeDepth(const microbrowser::js::Node& node, std::size_t depth = 0) {
  if (depth > 4 * microbrowser::js::kMaxParseDepth) {
    __builtin_trap();
  }
  std::size_t deepest = depth;
  for (const auto& child : node.children) {
    if (child != nullptr) {
      const std::size_t child_depth = TreeDepth(*child, depth + 1);
      deepest = child_depth > deepest ? child_depth : deepest;
    }
  }
  return deepest;
}

std::size_t CountNodes(const microbrowser::js::Node& node) {
  std::size_t count = 1;
  for (const auto& child : node.children) {
    if (child != nullptr) {
      count += CountNodes(*child);
    }
  }
  return count;
}

}  // namespace

// The JavaScript parser, fed arbitrary bytes.
//
// A page serves script, so this is the second attacker-controlled surface in
// the engine and the one with recursion in it. The properties:
//
//   1. Parsing terminates and always yields a tree. A parser that could return
//      null on some inputs would push a null check onto every consumer, and
//      the one that forgot it would be the crash.
//   2. Recursion is bounded, so `((((((...` is a parse error rather than a
//      stack overflow -- memory safety, not tidiness.
//   3. The node count is bounded by the input length. A parser that can build
//      more nodes than there are bytes has a path that consumes nothing, which
//      is an infinite loop seen from the other side.
//   4. Errors are bounded, so a megabyte of garbage is not a megabyte of
//      diagnostics.
//   5. Every error points inside the source.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view source(reinterpret_cast<const char*>(data), size);

  const microbrowser::js::ParseResult result = microbrowser::js::Parse(source);
  if (result.program == nullptr) {
    __builtin_trap();
  }
  TreeDepth(*result.program);
  if (CountNodes(*result.program) > 4 * size + 16) {
    __builtin_trap();
  }
  if (result.errors.size() > 32) {
    __builtin_trap();
  }
  for (const microbrowser::js::ParseError& error : result.errors) {
    if (error.offset > size) {
      __builtin_trap();
    }
  }
  return 0;
}
