#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dom/Node.h"
#include "html/TreeBuilder.h"

// The HTML fragment parsing algorithm, with the context element varied.
//
// This is the tree builder reached from *script*: `innerHTML`,
// `insertAdjacentHTML` and `<template>` all land here, so both inputs are
// attacker-chosen -- the markup and the element it is going into. The context
// is the parameter no other fuzz target varies, and it is the one that decides
// which insertion mode the parse *starts* in. Starting in "in row" or "in
// select" reaches error-recovery paths that a document parse can only get to
// after building its way there, which is why the pair is fuzzed rather than the
// markup alone.
//
// The properties asserted:
//
//   * it terminates, and produces a fragment, for every input;
//   * every node it returns is *in* the fragment. A fragment parse builds into
//     a throwaway document, and error recovery that emptied the open-element
//     stack would insert into that document instead -- the caller would get
//     fewer nodes back than the markup described, silently. The parser's
//     unpoppable root is what stops that, and this is the check on it;
//   * the fragment has no parent and no owner document, so nothing it holds can
//     reach the page it came from before the caller inserts it.
namespace {

// Every context the algorithm treats specially, plus enough ordinary ones to
// exercise the modes. The tokenizer-state contexts (title, textarea, style,
// script, plaintext) are first because they are the ones that change what a
// byte even *means*.
constexpr std::string_view kContexts[] = {
    "title", "textarea", "style",    "script", "plaintext", "noembed", "iframe",
    "html",  "head",     "body",     "div",    "p",         "table",   "tbody",
    "thead", "tfoot",    "tr",       "td",     "th",        "caption", "colgroup",
    "col",   "select",   "optgroup", "option", "template",  "ul",      "li",
    "form",  "svg",      "math",     "custom-element",
};
constexpr std::size_t kContextCount = sizeof(kContexts) / sizeof(kContexts[0]);

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  // The first byte picks the context and is not part of the markup, so a corpus
  // entry is a (context, fragment) pair the mutator can walk across.
  const std::string_view context = kContexts[data[0] % kContextCount];
  const std::string_view input(reinterpret_cast<const char*>(data) + 1, size - 1);

  // Bounded for the reason HtmlFuzzer's tree half is: a page can nest ten
  // thousand elements, and both building and destroying such a tree recurse.
  if (input.size() > 64 * 1024) {
    return 0;
  }

  const std::unique_ptr<microbrowser::dom::DocumentFragment> fragment =
      microbrowser::html::ParseFragment(input, context);
  if (fragment == nullptr) {
    __builtin_trap();  // HTML has no failure mode: every input is a fragment
  }
  if (fragment->Parent() != nullptr || fragment->OwnerDocument() != nullptr) {
    __builtin_trap();  // a parsed fragment belongs to nobody until it is inserted
  }

  // Every node reachable from the fragment is under the fragment. A node whose
  // ancestor walk does not reach the root is one the parse lost track of.
  fragment->ForEachDescendant([&fragment](const microbrowser::dom::Node& node) {
    const microbrowser::dom::Node* at = &node;
    while (at->Parent() != nullptr) {
      at = at->Parent();
    }
    if (at != fragment.get()) {
      __builtin_trap();
    }
  });

  // Walking and serializing exercise the tree the builder actually made, rather
  // than only the code that made it. A template serializes its contents, which
  // is the one place the serializer leaves the child list.
  const std::string serialized = fragment->SerializeChildren();
  (void)serialized;
  return 0;
}
