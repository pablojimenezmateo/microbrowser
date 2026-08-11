// The XML parser and the XML serializer.
//
// The property under test is not "does it build a tree for good input" -- it is
// **that it refuses bad input, and that the refusal is total**. XML's whole
// difference from HTML is that a well-formedness error is fatal, so every case
// below that ends in an error document is stating a rule the HTML tree builder
// would have recovered from silently.

#include <memory>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "dom/Node.h"
#include "xml/XmlParser.h"
#include "xml/XmlSerializer.h"

namespace microbrowser::tests {

namespace {

// Parses and serializes in one step, which is how most of these read: the
// round trip states the tree without a page of `ExpectEq` on node pointers.
std::string RoundTrip(const std::string& input) {
  const xml::XmlParseResult parsed = xml::ParseXml(input);
  return xml::SerializeXml(*parsed.document);
}

bool IsErrorDocument(const std::string& input) {
  const xml::XmlParseResult parsed = xml::ParseXml(input);
  if (parsed.ok || parsed.document == nullptr) {
    return false;
  }
  const dom::Element* root = parsed.document->DocumentElement();
  return root != nullptr && root->TagName() == "parsererror" &&
         root->Namespace().Uri() == xml::kParserErrorNamespace;
}

}  // namespace

void RegisterXmlTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Xml/ParsesAndRoundTrips", [] {
    ExpectEqString(RoundTrip("<root><child1>value1</child1></root>"),
                   "<root><child1>value1</child1></root>", "elements and text");
    ExpectEqString(RoundTrip("<?xml version=\"1.0\"?><foo/>"), "<foo/>",
                   "the declaration is not a node");
    ExpectEqString(RoundTrip("<a><b/><!--c--><?pi d?></a>"), "<a><b/><!--c--><?pi d?></a>",
                   "comments and processing instructions");
    ExpectEqString(RoundTrip("<a>&lt;&amp;&gt;</a>"), "<a>&lt;&amp;&gt;</a>",
                   "the five predefined entities, escaped back out");
    // Whitespace is ignorable only *outside* the root element. Inside one it is
    // character data, and a parser that skipped it would silently drop the
    // leading space of every mixed-content element.
    ExpectEqString(RoundTrip("  <a> x <b/> </a>  "), "<a> x <b/> </a>",
                   "whitespace is content inside the root and nothing outside it");
    ExpectEqString(RoundTrip("<a><![CDATA[<b>]]></a>"), "<a>&lt;b&gt;</a>",
                   "a CDATA section is text; there is no CDATASection node to write back");
    // Case is preserved and there is no HTML namespace anywhere: both are what
    // running the HTML tokenizer over this string would have got wrong.
    ExpectEqString(RoundTrip("<Foo Bar=\"1\"/>"), "<Foo Bar=\"1\"/>", "names keep their case");
  });

  AddTest(tests, "Xml/NamespacesAndPrefixes", [] {
    ExpectEqString(RoundTrip("<r xmlns=\"u1\"><c/></r>"), "<r xmlns=\"u1\"><c/></r>",
                   "an inherited default namespace is not re-declared");
    ExpectEqString(RoundTrip("<p:r xmlns:p=\"u1\"><p:c/></p:r>"),
                   "<p:r xmlns:p=\"u1\"><p:c/></p:r>", "a prefix stays the prefix");
    ExpectEqString(RoundTrip("<r xmlns=\"u1\"><c xmlns=\"\"/></r>"),
                   "<r xmlns=\"u1\"><c xmlns=\"\"/></r>", "undeclaring the default namespace");
    ExpectEqString(RoundTrip("<r xml:lang=\"en\"/>"), "<r xml:lang=\"en\"/>",
                   "the xml prefix is bound without being declared");
  });

  AddTest(tests, "Xml/WellFormednessErrorsAreFatal", [] {
    // Each of these is a document the HTML tree builder would have produced
    // *something* for. That is the point of the module.
    Expect(IsErrorDocument("<foo>"), "an unclosed element");
    Expect(IsErrorDocument("<a><b></a></b>"), "staggered tags");
    Expect(IsErrorDocument("</a>"), "an end tag with nothing open");
    Expect(IsErrorDocument("< a/>"), "'<' followed by a space");
    Expect(IsErrorDocument("<a novalue/>"), "an attribute with no value");
    Expect(IsErrorDocument("<a b=unquoted/>"), "an unquoted attribute value");
    Expect(IsErrorDocument("<a b=\"1\" b=\"2\"/>"), "a duplicate attribute");
    Expect(IsErrorDocument("<8a/>"), "a name starting with a digit");
    Expect(IsErrorDocument("<a/><b/>"), "two root elements");
    Expect(IsErrorDocument("text<a/>"), "character data before the root");
    Expect(IsErrorDocument(""), "no root element at all");
    Expect(IsErrorDocument("<a>&undeclared;</a>"), "a reference to an undeclared entity");
    // Namespace well-formedness, which is a second specification on top of the
    // first and is where most real feeds break.
    Expect(IsErrorDocument("<a x:b=\"1\"/>"), "an undeclared prefix on an attribute");
    Expect(IsErrorDocument("<x:a/>"), "an undeclared prefix on an element");
    Expect(IsErrorDocument("<a :b=\"1\"/>"), "an empty prefix");
    Expect(IsErrorDocument("<a xmlns:=\"u\"/>"), "'xmlns:' with no prefix");
    Expect(IsErrorDocument("<a xmlns:xmlns=\"u\"/>"), "declaring the xmlns prefix");
    Expect(IsErrorDocument("<a xmlns:p=\"\"/>"), "binding a prefix to no namespace");
    // The doctype rule that is observable from script: a public identifier
    // requires a system identifier after it.
    Expect(IsErrorDocument("<!DOCTYPE a PUBLIC \"p\"><a/>"), "PUBLIC with no system identifier");
    Expect(!IsErrorDocument("<!DOCTYPE a PUBLIC \"p\" \"\"><a/>"),
           "PUBLIC with an empty system identifier is fine");
  });

  AddTest(tests, "Xml/HostileInputIsBoundedRatherThanRecovered", [] {
    // Entity expansion is substitution *once*, so the classic amplification
    // attack is a grammar this parser does not have rather than a bound
    // somebody has to have got right.
    const xml::XmlParseResult bomb = xml::ParseXml(
        "<!DOCTYPE a [<!ENTITY x \"yy\"><!ENTITY y \"&x;&x;\">]><a>&y;</a>");
    Expect(bomb.ok, "the document parses");
    ExpectEqString(xml::SerializeXml(*bomb.document->DocumentElement()), "<a>&amp;x;&amp;x;</a>",
                   "an entity's own references are not expanded again");

    // Depth is refused rather than truncated: the tree the caller gets says
    // "this was not well-formed", not "here is the first 512 levels of it".
    std::string deep;
    for (int i = 0; i < 600; ++i) {
      deep += "<a>";
    }
    for (int i = 0; i < 600; ++i) {
      deep += "</a>";
    }
    Expect(!xml::ParseXml(deep).ok, "past the depth bound the parse fails");

    // Ill-formed UTF-8 is one replacement character per *sequence*, not per
    // byte. A lone surrogate arrives as three bytes and every engine turns it
    // into a single U+FFFD -- three would be a string a page can tell apart.
    const xml::XmlParseResult surrogate = xml::ParseXml("<a>x\xED\xA0\xBC</a>");
    Expect(surrogate.ok, "an ill-formed sequence is not a well-formedness error");
    ExpectEqString(xml::SerializeXml(*surrogate.document->DocumentElement()),
                   "<a>x\xEF\xBF\xBD</a>", "one U+FFFD for the whole sequence");
  });

  AddTest(tests, "Xml/SerializerInventsPrefixesTheSpecifiedWay", [] {
    auto document = std::make_unique<dom::Document>();
    auto& root = static_cast<dom::Element&>(
        document->Append(std::make_unique<dom::Element>(dom::NamespaceRef(), "root", 0)));
    root.SetAttributeNS(dom::NamespaceRef("uri1"), "attr1", 0, "v1");
    root.SetAttributeNS(dom::NamespaceRef("uri2"), "attr2", 0, "v2");
    // "ns1", "ns2", … and the declaration goes in beside the attribute that
    // needed it. A serializer that emitted the attribute without the
    // declaration would produce markup that no longer parses.
    ExpectEqString(xml::SerializeXml(root),
                   "<root xmlns:ns1=\"uri1\" ns1:attr1=\"v1\" xmlns:ns2=\"uri2\" "
                   "ns2:attr2=\"v2\"/>",
                   "generated prefixes");

    // An empty HTML-namespace element is written with an end tag unless it is
    // a void element, which is the one place HTML's shape reaches in here.
    auto& div = static_cast<dom::Element&>(
        root.Append(std::make_unique<dom::Element>("div")));
    ExpectEqString(xml::SerializeXml(div),
                   "<div xmlns=\"http://www.w3.org/1999/xhtml\"></div>", "a non-void HTML element");
    auto& br = static_cast<dom::Element&>(root.Append(std::make_unique<dom::Element>("br")));
    ExpectEqString(xml::SerializeXml(br), "<br xmlns=\"http://www.w3.org/1999/xhtml\" />",
                   "a void one");
  });
}

}  // namespace microbrowser::tests
