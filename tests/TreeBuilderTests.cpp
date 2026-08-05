#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "dom/Node.h"
#include "html/TreeBuilder.h"

namespace microbrowser::tests {

using dom::Document;
using dom::Element;
using html::ParseDocument;
using html::TreeBuilder;

namespace {

// The whole tree as HTML, which lets a test state an expected structure in one
// string instead of walking nodes.
std::string Serialized(std::string_view input) {
  return ParseDocument(input)->SerializeChildren();
}

void ExpectTree(std::string_view input, std::string_view expected) {
  ExpectEqString(Serialized(input), expected,
                 std::string("wrong tree for: ") + std::string(input));
}

// The fragment parsed into `context`, serialized. The context element is the
// whole point of the algorithm, so every fragment assertion names one.
void ExpectFragment(std::string_view context, std::string_view input,
                    std::string_view expected) {
  const std::unique_ptr<dom::DocumentFragment> fragment =
      html::ParseFragment(input, context);
  ExpectEqString(fragment->SerializeChildren(), expected,
                 std::string("wrong fragment for <") + std::string(context) + ">: " +
                     std::string(input));
}

}  // namespace

void RegisterTreeBuilderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TreeBuilder/BuildsTheImpliedElementsAroundContent", [] {
    // None of html, head or body appear in the source, and all three must
    // appear in the tree. Every document has them whether or not it says so.
    ExpectTree("<p>hi</p>", "<html><head></head><body><p>hi</p></body></html>");
    ExpectTree("hello", "<html><head></head><body>hello</body></html>");
    ExpectTree("", "<html><head></head><body></body></html>");
  });

  AddTest(tests, "TreeBuilder/PlacesHeadContentInTheHead", [] {
    const std::unique_ptr<Document> document =
        ParseDocument("<title>T</title><meta charset=utf-8><p>body</p>");
    Expect(document->Head() != nullptr, "there is a head");
    Expect(document->Body() != nullptr, "and a body");
    Expect(document->Head()->TextContent() == "T", "the title text is in the head");
    Expect(document->Body()->TextContent() == "body",
           "and the paragraph is in the body, which the parser inferred from a <p> appearing");
  });

  AddTest(tests, "TreeBuilder/RecordsQuirksModeFromTheDoctype", [] {
    Expect(!ParseDocument("<!DOCTYPE html><p>x")->InQuirksMode(), "a modern doctype is standards");
    Expect(ParseDocument("<p>x")->InQuirksMode(),
           "no doctype at all is quirks mode, which is a rendering decision made once, here");
    Expect(ParseDocument("<!DOCTYPE something><p>x")->InQuirksMode(), "an unknown one is quirks");
  });

  AddTest(tests, "TreeBuilder/KeepsAttributes", [] {
    const std::unique_ptr<Document> document =
        ParseDocument("<a href='/x' class=\"b c\" data-n>link</a>");
    const Element* anchor = document->FirstElementByTagName("a");
    Expect(anchor != nullptr, "the anchor exists");
    ExpectEqString(*anchor->GetAttribute("href"), "/x", "href");
    ExpectEqString(*anchor->GetAttribute("class"), "b c", "class");
    Expect(anchor->HasAttribute("data-n"), "a bare attribute is present");
  });

  // The single most visible thing a tree builder does. Without implied end
  // tags, `<p>a<p>b` nests and every paragraph on the page is inside the last.
  AddTest(tests, "TreeBuilder/ClosesParagraphsImplicitly", [] {
    ExpectTree("<p>a<p>b", "<html><head></head><body><p>a</p><p>b</p></body></html>");
    ExpectTree("<p>a<div>b</div>",
               "<html><head></head><body><p>a</p><div>b</div></body></html>");
  });

  AddTest(tests, "TreeBuilder/ClosesListItemsImplicitly", [] {
    ExpectTree("<ul><li>a<li>b</ul>",
               "<html><head></head><body><ul><li>a</li><li>b</li></ul></body></html>");
  });

  AddTest(tests, "TreeBuilder/VoidElementsNeverGetChildren", [] {
    // If a void element were pushed onto the stack of open elements, the entire
    // rest of the document would nest inside it.
    ExpectTree("<p>a<br>b</p>",
               "<html><head></head><body><p>a<br>b</p></body></html>");
    ExpectTree("<img src=x><p>after</p>",
               "<html><head></head><body><img src=\"x\"><p>after</p></body></html>");
  });

  AddTest(tests, "TreeBuilder/AnEndTagForSomethingNotOpenIsDropped", [] {
    // Popping anyway would close an unrelated element — `</div>` with no open
    // div must not close the body.
    ExpectTree("<p>a</div>b", "<html><head></head><body><p>ab</p></body></html>");
    ExpectTree("</p>x", "<html><head></head><body>x</body></html>");
  });

  AddTest(tests, "TreeBuilder/NestsElementsThatShouldNest", [] {
    ExpectTree("<div><span>a</span><span>b</span></div>",
               "<html><head></head><body><div><span>a</span><span>b</span></div></body></html>");
  });

  AddTest(tests, "TreeBuilder/AdjacentTextBecomesOneNode", [] {
    // A DOM with adjacent text nodes is observably different from one without,
    // and text arrives split across tokens whenever a character reference is in
    // the middle of it.
    const std::unique_ptr<Document> document = ParseDocument("<p>a&amp;b&amp;c</p>");
    const Element* paragraph = document->FirstElementByTagName("p");
    Expect(paragraph != nullptr, "the paragraph exists");
    ExpectEqInt(static_cast<long long>(paragraph->Children().size()), 1,
                "three text runs and two references are one text node");
    ExpectEqString(paragraph->TextContent(), "a&b&c", "with the references expanded");
  });

  AddTest(tests, "TreeBuilder/ScriptAndStyleContentIsTextNotMarkup", [] {
    const std::unique_ptr<Document> document =
        ParseDocument("<script>if (a<b) { c(); }</script><p>after</p>");
    Expect(document->FirstElementByTagName("b") == nullptr,
           "`<b` inside a script is script text, not an element");
    Expect(document->FirstElementByTagName("p") != nullptr,
           "and the document continues normally afterwards");
    const Element* script = document->FirstElementByTagName("script");
    ExpectEqString(script->TextContent(), "if (a<b) { c(); }", "the script text is intact");
  });

  AddTest(tests, "TreeBuilder/TitleContentIsTextEvenWhenItLooksLikeMarkup", [] {
    const std::unique_ptr<Document> document = ParseDocument("<title>a <b> c</title>");
    Expect(document->FirstElementByTagName("b") == nullptr, "no element was created");
    ExpectEqString(document->FirstElementByTagName("title")->TextContent(), "a <b> c",
                   "the tag text stayed text");
  });

  AddTest(tests, "TreeBuilder/CommentsLandInTheTree", [] {
    ExpectTree("<!--top--><p>x</p>",
               "<!--top--><html><head></head><body><p>x</p></body></html>");
  });

  AddTest(tests, "TreeBuilder/ContentAfterBodyGoesBackIntoIt", [] {
    // What browsers do, and what pages with trailing markup depend on.
    ExpectTree("<body><p>a</p></body>trailing",
               "<html><head></head><body><p>a</p>trailing</body></html>");
  });

  // HTML has no failure mode: every input is a document.
  AddTest(tests, "TreeBuilder/ProducesADocumentForAnyInput", [] {
    for (const std::string_view input : {
             "", "<", "</", "<<<<", "<html", "<p", "</p></p></p>", "<!DOCTYPE",
             "<a href=", "<div><div><div>", "&", "&#x", "<script>", "<title>",
             "<!--", "<p>\xFF\xFE</p>", "<a b='", "<><><>",
         }) {
      const std::unique_ptr<Document> document = ParseDocument(input);
      Expect(document != nullptr, std::string("no document for: ") + std::string(input));
      Expect(document->DocumentElement() != nullptr,
             std::string("no html element for: ") + std::string(input));
    }
  });

  AddTest(tests, "TreeBuilder/DeepNestingDoesNotRecurseUnbounded", [] {
    // A page can nest ten thousand divs. The tree builder must survive it,
    // because a stack overflow here is reachable by anyone who can serve HTML.
    std::string input;
    for (int i = 0; i < 5000; ++i) {
      input += "<div>";
    }
    const std::unique_ptr<Document> document = ParseDocument(input);
    Expect(document->DocumentElement() != nullptr, "a deeply nested document still parses");
    // Destruction is the other half: a tree this deep is destroyed by the same
    // recursion that built it.
  });

  AddTest(tests, "TreeBuilder/ReportsWhenItNeededAnUnimplementedInsertionMode", [] {
    // `<frameset>` has no insertion mode here. That is recorded rather than
    // silently producing a tree different from every other browser's.
    // `<template>` used to be on this list and no longer is -- see
    // TreeBuilder/TemplateContentsAreNotChildren.
    TreeBuilder builder("<frameset><frame></frameset>");
    const std::unique_ptr<Document> document = builder.Build();
    Expect(document != nullptr, "a document is still produced");
    Expect(builder.UnsupportedModeCount() > 0,
           "and the gap is observable, so a caller can say the tree is incomplete rather "
           "than trusting it");

    TreeBuilder ordinary("<table><tr><td>tables are implemented</table>");
    ordinary.Build();
    ExpectEqInt(static_cast<long long>(ordinary.UnsupportedModeCount()), 0,
                "an ordinary document reports nothing unsupported");
  });

  AddTest(tests, "TreeBuilder/SuppliesTheTbodyNobodyWrites", [] {
    // Almost no page writes `<tbody>`, and every browser's DOM has one. Layout
    // and selectors both see the row group whether or not the author typed it.
    ExpectTree("<table><tr><td>cell</td></tr></table>",
               "<html><head></head><body>"
               "<table><tbody><tr><td>cell</td></tr></tbody></table>"
               "</body></html>");
  });

  AddTest(tests, "TreeBuilder/ClosesCellsAndRowsImplicitly", [] {
    // `<td>a<td>b` is two sibling cells. Nesting them instead would indent the
    // whole rest of the table one level per cell.
    ExpectTree("<table><tr><td>a<td>b<tr><td>c</table>",
               "<html><head></head><body><table><tbody>"
               "<tr><td>a</td><td>b</td></tr>"
               "<tr><td>c</td></tr>"
               "</tbody></table></body></html>");
  });

  AddTest(tests, "TreeBuilder/FosterParentsContentThatCannotLiveInATable", [] {
    // §13.2.6.1. Text and stray elements between a table and its rows are moved
    // *before* the table rather than dropped or nested, because a table's
    // children are constrained in a way no other element's are.
    ExpectTree("<table>stray<tr><td>cell</table>",
               "<html><head></head><body>stray"
               "<table><tbody><tr><td>cell</td></tr></tbody></table>"
               "</body></html>");
    ExpectTree("<table><div>d</div><tr><td>cell</table>",
               "<html><head></head><body><div>d</div>"
               "<table><tbody><tr><td>cell</td></tr></tbody></table>"
               "</body></html>");
  });

  AddTest(tests, "TreeBuilder/KeepsWhitespaceInsideTheTable", [] {
    // Whitespace-only runs are the one kind of character data a table keeps, so
    // a pretty-printed table does not shed its indentation into the body.
    ExpectTree("<table> <tr><td>c</table>",
               "<html><head></head><body>"
               "<table> <tbody><tr><td>c</td></tr></tbody></table>"
               "</body></html>");
  });

  AddTest(tests, "TreeBuilder/NestedTablesStaySeparate", [] {
    // Table scope is what keeps the inner `</table>` from closing the outer
    // one. Hacker News nests layout tables several deep.
    ExpectTree("<table><tr><td><table><tr><td>in</table>out</table>",
               "<html><head></head><body><table><tbody><tr><td>"
               "<table><tbody><tr><td>in</td></tr></tbody></table>out"
               "</td></tr></tbody></table></body></html>");
  });

  AddTest(tests, "TreeBuilder/HandlesCaptionsAndColumnGroups", [] {
    ExpectTree("<table><caption>C</caption><col><tr><td>a</table>",
               "<html><head></head><body><table>"
               "<caption>C</caption><colgroup><col></colgroup>"
               "<tbody><tr><td>a</td></tr></tbody>"
               "</table></body></html>");
  });

  AddTest(tests, "TreeBuilder/ReturnsToTheBodyAfterATableCloses", [] {
    // "Reset the insertion mode appropriately": the mode after `</table>` is a
    // function of what is still open, not of what it was before.
    ExpectTree("<table><tr><td>a</table><p>after</p>",
               "<html><head></head><body>"
               "<table><tbody><tr><td>a</td></tr></tbody></table><p>after</p>"
               "</body></html>");
    ExpectTree("<div><table><tr><td>a</table><p>after</p></div>",
               "<html><head></head><body><div>"
               "<table><tbody><tr><td>a</td></tr></tbody></table><p>after</p>"
               "</div></body></html>");
  });

  AddTest(tests, "TreeBuilder/ClosesOptionsAndOptgroupsImplicitly", [] {
    ExpectTree("<select><option>One<option>Two</select>",
               "<html><head></head><body><select>"
               "<option>One</option><option>Two</option>"
               "</select></body></html>");
    ExpectTree("<select><optgroup label='A'><option>One<optgroup label='B'><option>Two</select>",
               "<html><head></head><body><select>"
               "<optgroup label=\"A\"><option>One</option></optgroup>"
               "<optgroup label=\"B\"><option>Two</option></optgroup>"
               "</select></body></html>");
  });

  AddTest(tests, "TreeBuilder/TableContentClosesASelectInTable", [] {
    ExpectTree("<table><tr><td><select><option>One<tr><td>Two</table>",
               "<html><head></head><body><table><tbody><tr><td>"
               "<select><option>One</option></select></td></tr>"
               "<tr><td>Two</td></tr></tbody></table></body></html>");
  });

  AddTest(tests, "TreeBuilder/HrClosesSelectOptionsAndOptgroups", [] {
    ExpectTree("<select><optgroup label='A'><option>One<hr><option>Two</select>",
               "<html><head></head><body><select>"
               "<optgroup label=\"A\"><option>One</option></optgroup>"
               "<hr><option>Two</option></select></body></html>");
  });

  AddTest(tests, "TreeBuilder/DropsTableStructureOutsideATable", [] {
    // A `<td>` with no table is a parse error and inserts nothing. Honouring it
    // would put a cell in the body, which no browser does.
    ExpectTree("<td>orphan</td>", "<html><head></head><body>orphan</body></html>");
    ExpectTree("<tr><td>orphan", "<html><head></head><body>orphan</body></html>");
  });

  AddTest(tests, "TreeBuilder/ATrailingSolidusDoesNotCloseANonVoidElement", [] {
    // `<div/>` is `<div>`, not `<div></div>`. HTML acknowledges the
    // self-closing flag only on void elements and on foreign content, and this
    // parser has no foreign content -- so honouring it anywhere else looks
    // like a fix and is a bug.
    ExpectTree("<div/>text</div>", "<html><head></head><body><div>text</div></body></html>");
    ExpectTree("<br/>", "<html><head></head><body><br></body></html>");

    // The case that found it. Hacker News writes a self-closing spacer row
    // between its rows; closing it early left the following <tr> with no row
    // to close, so every row after it was foster-parented out of the table and
    // the page came apart below the header.
    ExpectTree("<table><tr><td>a</td></tr><tr/><tr><td>b</td></tr></table>",
               "<html><head></head><body><table><tbody><tr><td>a</td></tr><tr></tr>"
               "<tr><td>b</td></tr></tbody></table></body></html>");
  });

  AddTest(tests, "TreeBuilder/FindsElementsByTagName", [] {
    const std::unique_ptr<Document> document =
        ParseDocument("<div><p>a</p><p>b</p><span>c</span></div>");
    ExpectEqInt(static_cast<long long>(document->ElementsByTagName("p").size()), 2,
                "two paragraphs");
    ExpectEqString(document->ElementsByTagName("p").at(1)->TextContent(), "b",
                   "in tree order");
  });

  // --- The fragment parsing algorithm, §13.2.6 --------------------------------

  AddTest(tests, "TreeBuilder/FragmentHasNoImpliedHtmlHeadOrBody", [] {
    // The difference a caller notices first: a document parse invents html,
    // head and body, and a fragment parse must invent none of them or every
    // `innerHTML` assignment would nest the page inside another page.
    ExpectFragment("div", "<p>hi</p>", "<p>hi</p>");
    ExpectFragment("div", "", "");
    ExpectFragment("div", "text", "text");
  });

  AddTest(tests, "TreeBuilder/FragmentTakesItsRulesFromTheContextElement", [] {
    // The whole reason the algorithm takes a context element. The same six
    // bytes are a table cell in one place and bare text in another, and a
    // parser that ignored the context would silently disagree with every other
    // browser on both.
    ExpectFragment("tr", "<td>c</td>", "<td>c</td>");
    ExpectFragment("div", "<td>c</td>", "c");
    // A row inside a table context still gets the tbody nobody writes...
    ExpectFragment("table", "<tr><td>c", "<tbody><tr><td>c</td></tr></tbody>");
    // ...and inside a tbody context it does not, because there is one already.
    ExpectFragment("tbody", "<tr><td>c", "<tr><td>c</td></tr>");
    ExpectFragment("select", "<option>a<option>b",
                   "<option>a</option><option>b</option>");
  });

  AddTest(tests, "TreeBuilder/FragmentContextChoosesTheTokenizerState", [] {
    // §13.2.6 step 3. In a title or a textarea the markup is text, and an
    // engine that built a `<b>` element here would differ from every other one
    // on `titleElement.innerHTML = 'a<b>c'`.
    ExpectFragment("title", "a<b>c", "a&lt;b&gt;c");
    ExpectFragment("textarea", "a<b>c", "a&lt;b&gt;c");
    ExpectFragment("style", "p{color:red}", "p{color:red}");
    ExpectFragment("div", "a<b>c", "a<b>c</b>");
  });

  AddTest(tests, "TreeBuilder/FragmentSurvivesEndTagsThatDoNotMatch", [] {
    // Markup and context are both chosen by a page, and the pair a page will
    // find is the one whose end tags unbalance the stack. The root of a
    // fragment parse is unpoppable for exactly this reason: pop it and every
    // later node lands in a throwaway document the caller never sees.
    ExpectFragment("div", "</div></div></body></html>x", "x");
    ExpectFragment("td", "</td></tr></table>x", "x");
    ExpectFragment("html", "</html></head></body>x", "<head></head><body>x</body>");
    ExpectFragment("select", "</select></option>x", "x");
  });

  // --- `<template>`, §13.2.6.4.4 ----------------------------------------------

  AddTest(tests, "TreeBuilder/TemplateContentsAreNotChildren", [] {
    const std::unique_ptr<Document> document =
        ParseDocument("<body><template><p>inside</p></template></body>");
    const Element* element = document->FirstElementByTagName("template");
    Expect(element != nullptr, "the template is in the tree");
    ExpectEqInt(static_cast<long long>(element->Children().size()), 0,
                "and it has no children -- its markup is not document content");
    Expect(element->Content() != nullptr, "it has a contents fragment");
    ExpectEqString(element->Content()->SerializeChildren(), "<p>inside</p>",
                   "which is where the markup went");
    // The consequence that matters: nothing that walks the document can reach
    // it, so a template full of markup is not styled, laid out, or fetched.
    Expect(document->FirstElementByTagName("p") == nullptr,
           "and a walk of the document does not find it");
    // It still serializes back to what it was, which is what makes a round trip
    // through innerHTML preserve a template rather than empty it.
    ExpectEqString(element->Serialize(), "<template><p>inside</p></template>",
                   "and serializing it produces its contents");
  });

  AddTest(tests, "TreeBuilder/TemplateContentsUseTheModeTheirFirstTagAsksFor", [] {
    // A template's contents are parsed as though they were somewhere else: a
    // `<tr>` inside one builds a row even though there is no table anywhere.
    // Without "in template", the row would be dropped as table structure
    // outside a table -- which is what a page that templates its rows would see.
    const std::unique_ptr<Document> document =
        ParseDocument("<template><tr><td>cell</td></tr></template>");
    const Element* element = document->FirstElementByTagName("template");
    Expect(element != nullptr && element->Content() != nullptr, "the template exists");
    ExpectEqString(element->Content()->SerializeChildren(), "<tr><td>cell</td></tr>",
                   "and its row survived without a table around it");
  });

  AddTest(tests, "TreeBuilder/TemplateInsideATableDoesNotFosterItsContents", [] {
    // Foster parenting moves stray content out of a table. A template's
    // contents are not in the table -- they are not in any document -- so
    // there is nothing to move them out of.
    const std::unique_ptr<Document> document =
        ParseDocument("<table><template><div>x</div></template><tr><td>c</table>");
    const Element* element = document->FirstElementByTagName("template");
    Expect(element != nullptr && element->Content() != nullptr, "the template exists");
    ExpectEqString(element->Content()->SerializeChildren(), "<div>x</div>",
                   "its div stayed inside it rather than being fostered before the table");
    Expect(document->FirstElementByTagName("td") != nullptr, "and the real row still built");
  });

  AddTest(tests, "TreeBuilder/NestedTemplatesRestoreTheModeEachWasOpenedIn", [] {
    // Why the stack of template insertion modes is a stack. With one saved
    // mode, closing the inner template would restore the outer one's and the
    // content after it would land in the wrong place.
    const std::unique_ptr<Document> document = ParseDocument(
        "<template><tr><template><div>in</div></template><td>cell</td></tr></template>");
    const Element* outer = document->FirstElementByTagName("template");
    Expect(outer != nullptr && outer->Content() != nullptr, "the outer template exists");
    ExpectEqString(outer->Content()->SerializeChildren(),
                   "<tr><template><div>in</div></template><td>cell</td></tr>",
                   "the cell after the inner template is still in the row");
  });

  AddTest(tests, "TreeBuilder/FragmentOfATemplateParsesAsTemplateContents", [] {
    // `template.innerHTML = '<td>x'` -- the context is the template, so the
    // contents are parsed with the template's own rules rather than the body's.
    ExpectFragment("template", "<td>x</td>", "<td>x</td>");
    ExpectFragment("template", "<div>x</div>", "<div>x</div>");
  });

  AddTest(tests, "TreeBuilder/NodesKnowTheirAncestors", [] {
    const std::unique_ptr<Document> document = ParseDocument("<div><ul><li>x</li></ul></div>");
    const Element* item = document->FirstElementByTagName("li");
    Expect(item != nullptr, "the list item exists");
    Expect(item->ClosestAncestor("ul") != nullptr, "its list is an ancestor");
    Expect(item->ClosestAncestor("div") != nullptr, "and so is the div");
    Expect(item->ClosestAncestor("table") == nullptr, "but not something that is not");
  });
}

}  // namespace microbrowser::tests
