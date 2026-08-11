// `dom::NamespaceRef`: the handle an element and an attribute carry instead of
// a namespace URI string.
//
// The interesting property is not that it round-trips a URI -- it is that the
// table behind it is **reference counted**. An append-only intern table would
// be unbounded growth driven by page script (`createElementNS('u' + i, 'a')` in
// a loop), and nothing about the round trip would say so. That is what the
// second test here is for, and it is the reason this class exists rather than
// an enum plus a `std::string`.

#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "dom/Namespaces.h"
#include "dom/Node.h"

namespace microbrowser::tests {

using dom::NamespaceRef;

void RegisterNamespaceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Namespaces/TheSixKnownUrisCostNothing", [] {
    ExpectEqString(std::string(NamespaceRef(NamespaceRef::kHtml).Uri()),
                   "http://www.w3.org/1999/xhtml", "the HTML namespace");
    ExpectEqString(std::string(NamespaceRef("http://www.w3.org/2000/svg").Uri()),
                   "http://www.w3.org/2000/svg", "SVG, interned back to its constant");
    Expect(NamespaceRef("http://www.w3.org/2000/svg") == NamespaceRef(NamespaceRef::kSvg),
           "a URI and its constant are the same handle");
    // The empty string is no namespace, which is step 1 of every DOM method
    // that takes one -- not a namespace whose URI happens to be empty.
    Expect(NamespaceRef("").IsNone(), "the empty URI is no namespace");
    Expect(NamespaceRef().IsNone(), "and so is a default-constructed one");
    ExpectEqString(std::string(NamespaceRef().Uri()), "", "which reads back as empty");
    const std::size_t interned = dom::InternedNamespaceCount();
    { const NamespaceRef known(NamespaceRef::kMathMl); }
    ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(interned),
                 "a known namespace does not touch the table");
  });

  AddTest(tests, "Namespaces/AnUnknownUriIsInternedAndThenReclaimed", [] {
    const std::size_t before = dom::InternedNamespaceCount();
    {
      const NamespaceRef first("http://example.org/one");
      ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before) + 1, "one new URI, one entry");
      const NamespaceRef again("http://example.org/one");
      Expect(first == again, "the same URI is the same handle");
      ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before) + 1, "and not a second entry");
      const NamespaceRef other("http://example.org/two");
      Expect(!(first == other), "a different URI is a different handle");
      ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before) + 2, "and does get its own entry");
      ExpectEqString(std::string(first.Uri()), "http://example.org/one",
                     "an interned handle still knows its URI");
    }
    // The point of the whole class: the entries went away with the last holder.
    // Without this, a page making ten million one-off namespaces would keep
    // all ten million for the life of the process.
    ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before),
                 "nothing holds them any more, so nothing is interned");
  });

  AddTest(tests, "Namespaces/CopiesAndMovesKeepTheCountHonest", [] {
    const std::size_t before = dom::InternedNamespaceCount();
    {
      NamespaceRef made("http://example.org/counted");
      NamespaceRef copy = made;                 // NOLINT: the copy is the test
      NamespaceRef moved = std::move(made);     // NOLINT: so is the move
      ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before) + 1, "one entry, three names");
      Expect(copy == moved, "the copy and the moved-to handle agree");
      Expect(made.IsNone(), "and the moved-from one is no namespace");
      copy = NamespaceRef();
      ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before) + 1,
                   "one holder left, so the entry stays");
    }
    ExpectEqInt(static_cast<long long>(dom::InternedNamespaceCount()), static_cast<long long>(before), "and goes when the last one does");
  });

  AddTest(tests, "Namespaces/AnElementRemembersItsNameInFourParts", [] {
    // The bug this replaced: one name field answering both `tagName` and
    // `localName`, which cannot be right about both at once.
    const dom::Element prefixed(NamespaceRef("http://example.org/"), "x:b", 1);
    ExpectEqString(prefixed.TagName(), "x:b", "the qualified name");
    ExpectEqString(std::string(prefixed.LocalName()), "b", "the local name");
    ExpectEqString(std::string(prefixed.Prefix()), "x", "the prefix");
    ExpectEqString(std::string(prefixed.Namespace().Uri()), "http://example.org/",
                   "the namespace");

    // And the case that makes the prefix a stored *length* rather than a search
    // for a colon: an HTML parser meeting `<xml:lang>` produces one element
    // whose whole local name has a colon in it.
    const dom::Element unprefixed("xml:lang");
    ExpectEqString(unprefixed.TagName(), "xml:lang", "the qualified name is the whole of it");
    ExpectEqString(std::string(unprefixed.LocalName()), "xml:lang", "and so is the local name");
    Expect(unprefixed.Prefix().empty(), "there is no prefix");
    Expect(unprefixed.Namespace().IsHtml(), "and it is an HTML element");
  });

  AddTest(tests, "Namespaces/AttributesMatchByQualifiedNameOrByNamespace", [] {
    dom::Element element("div");
    element.SetAttributeNS(NamespaceRef("http://example.org/"), "x:a", 1, "one");
    element.SetAttribute("a", "two");
    ExpectEqInt(static_cast<long long>(element.Attributes().size()), 2,
                 "the two are different attributes, not one overwritten");
    // `getAttribute` matches the qualified name whatever namespace it is in;
    // the `…NS` half matches (namespace, local name). Both attributes above
    // have the local name `a`, and only the second has the qualified name `a`.
    const std::string* by_name = element.GetAttribute("a");
    Expect(by_name != nullptr && *by_name == "two", "by qualified name");
    const dom::Attribute* by_namespace =
        element.GetAttributeNS(NamespaceRef("http://example.org/"), "a");
    Expect(by_namespace != nullptr && by_namespace->value == "one", "by namespace");

    // Setting through the namespaced path keeps the attribute's position and
    // replaces its prefix, rather than appending a second one nothing could
    // tell apart.
    element.SetAttributeNS(NamespaceRef("http://example.org/"), "y:a", 1, "three");
    ExpectEqInt(static_cast<long long>(element.Attributes().size()), 2, "still two attributes");
    ExpectEqString(element.Attributes()[0].name, "y:a", "with the new prefix, in place");
    Expect(element.RemoveAttributeNS(NamespaceRef("http://example.org/"), "a"),
           "and it is removable by namespace");
    ExpectEqInt(static_cast<long long>(element.Attributes().size()), 1, "leaving the un-namespaced one");
  });
}

}  // namespace microbrowser::tests
