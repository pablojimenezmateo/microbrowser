#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"

// The DOM binding layer.
//
// The seam ADR 0008 describes: the only path from a page's code to its tree.
// Two properties are worth testing beyond "does it read an attribute" --
// wrapper identity, because script uses a wrapper as a map key, and that a
// binding called on something that is not a node is a TypeError rather than a
// jump through a bad pointer.

namespace microbrowser::tests {

namespace {

struct Bound {
  std::unique_ptr<dom::Document> document;
  std::unique_ptr<js::Interpreter> interpreter;
  std::unique_ptr<bindings::DomBindings> dom_bindings;
};

Bound Bind(std::string_view html) {
  Bound bound;
  bound.document = html::ParseDocument(html);
  bound.interpreter = std::make_unique<js::Interpreter>();
  bound.dom_bindings =
      std::make_unique<bindings::DomBindings>(*bound.interpreter, *bound.document);
  bound.dom_bindings->Install();
  return bound;
}

// Runs `source` against a document and returns its completion value, with a
// thrown value prefixed so a test states which of the two it expects.
std::string Run(std::string_view html, std::string_view source) {
  Bound bound = Bind(html);
  const js::Result result = bound.interpreter->Run(source);
  if (result.completion == js::Completion::Throw) {
    return "throw " + js::ToString(result.value);
  }
  return js::ToString(result.value);
}

void ExpectScript(std::string_view html, std::string_view source, std::string_view expected) {
  ExpectEqString(Run(html, source), std::string(expected),
                 std::string("running: ") + std::string(source));
}

constexpr const char* kPage =
    "<html><body><h1 id=title class='big head'>Hello</h1>"
    "<div id=list><p>one</p><p>two</p></div></body></html>";

}  // namespace

void RegisterDomBindingsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DomBindings/ScriptCanFindElements", [] {
    ExpectScript(kPage, "document.getElementById('title').tagName", "h1");
    ExpectScript(kPage, "document.getElementById('title').textContent", "Hello");
    ExpectScript(kPage, "document.getElementById('nope') === null", "true");
    ExpectScript(kPage, "document.getElementsByTagName('p').length", "2");
    ExpectScript(kPage, "document.getElementsByTagName('p')[1].textContent", "two");
    ExpectScript(kPage, "document.body.tagName", "body");
    ExpectScript(kPage, "document.documentElement.tagName", "html");
  });

  AddTest(tests, "DomBindings/QuerySelectorHandlesTheThreeSimpleForms", [] {
    ExpectScript(kPage, "document.querySelector('p').textContent", "one");
    ExpectScript(kPage, "document.querySelector('#list').tagName", "div");
    ExpectScript(kPage, "document.querySelector('.big').textContent", "Hello");
    // Whole-word, so `.head` matches and `.hea` does not -- a substring match
    // here would make `.btn` select every `btn-large` on the page.
    ExpectScript(kPage, "document.querySelector('.head') === null", "false");
    ExpectScript(kPage, "document.querySelector('.hea') === null", "true");
    ExpectScript(kPage, "document.querySelector('.big') === null", "false");
  });

  AddTest(tests, "DomBindings/AttributesReadAndWrite", [] {
    ExpectScript(kPage, "document.getElementById('title').getAttribute('class')", "big head");
    ExpectScript(kPage, "document.getElementById('title').getAttribute('missing') === null",
                 "true");
    ExpectScript(kPage, "document.getElementById('title').hasAttribute('id')", "true");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.setAttribute('data-x', '7'); "
                 "t.getAttribute('data-x')",
                 "7");
    ExpectScript(kPage, "document.getElementById('title').className", "big head");
    ExpectScript(kPage, "document.getElementById('title').id", "title");
  });

  AddTest(tests, "DomBindings/TheSameNodeIsTheSameObject", [] {
    // Identity, which is what script uses a wrapper for as often as it reads a
    // property off one: a fresh wrapper per access breaks every Set, Map and
    // `===` a page writes without failing loudly anywhere.
    ExpectScript(kPage, "document.body === document.body", "true");
    ExpectScript(kPage,
                 "document.getElementById('title') === document.getElementsByTagName('h1')[0]",
                 "true");
    ExpectScript(kPage,
                 "const p = document.getElementsByTagName('p')[0]; p.parentNode === "
                 "document.getElementById('list')",
                 "true");
    // And it holds through a collection, because the cache is in the heap
    // where the collector can see it.
    ExpectScript(kPage,
                 "const first = document.body; let sink = null; "
                 "for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; } "
                 "first === document.body",
                 "true");
  });

  AddTest(tests, "DomBindings/ChildrenAndChildNodesAnswerDifferentQuestions", [] {
    // The distinction that trips up anyone who indexes into the wrong one and
    // gets a whitespace text node.
    ExpectScript(kPage, "document.getElementById('list').children.length", "2");
    ExpectScript(kPage, "document.getElementById('list').children[0].tagName", "p");
    ExpectScript("<div id=d>text<span></span></div>",
                 "document.getElementById('d').childNodes.length", "2");
    ExpectScript("<div id=d>text<span></span></div>",
                 "document.getElementById('d').children.length", "1");
    ExpectScript("<div id=d>text</div>",
                 "document.getElementById('d').childNodes[0].nodeType", "3");
    ExpectScript(kPage, "document.getElementById('title').nodeType", "1");
  });

  AddTest(tests, "DomBindings/ScriptCanBuildAndAttachNodes", [] {
    ExpectScript(kPage,
                 "const el = document.createElement('section'); el.appendText('made'); "
                 "document.body.appendChild(el); "
                 "document.getElementsByTagName('section')[0].textContent",
                 "made");
    // A created node is owned by the bindings until it is attached, so
    // creating one and dropping it leaks nothing and dangles nothing.
    ExpectScript(kPage, "document.createElement('div').tagName", "div");
    // Moving an attached node would mean detaching it, which is the operation
    // this slice deliberately has no caller for -- see ADR 0008.
    ExpectScript(kPage,
                 "try { document.body.appendChild(document.getElementById('title')) } "
                 "catch (e) { e.name }",
                 "TypeError");
  });

  AddTest(tests, "DomBindings/ABindingCalledOnSomethingElseIsATypeError", [] {
    // A page can call any of these on anything. Every binding checks its
    // receiver rather than trusting it, because the alternative is a jump
    // through whatever number the page put in the slot.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); "
                 "try { t.getAttribute.call(7, 'id') } catch (e) { e.name }",
                 "TypeError");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); "
                 "try { t.setAttribute.call({}, 'a', 'b') } catch (e) { e.name }",
                 "TypeError");
    ExpectScript(kPage,
                 "try { document.body.appendChild(42) } catch (e) { e.name }", "TypeError");
    ExpectScript(kPage,
                 "try { document.body.appendChild.call(null, 1) } catch (e) { e.name }",
                 "TypeError");
  });

  AddTest(tests, "DomBindings/ScriptSeesTheTreeItChanges", [] {
    // The point of the whole layer: a change made by script is a change to the
    // document, not to a copy of it.
    Bound bound = Bind("<div id=host></div>");
    const js::Result result = bound.interpreter->Run(
        "const el = document.createElement('span');"
        "el.setAttribute('class', 'added');"
        "el.appendText('from script');"
        "document.getElementById('host').appendChild(el);"
        "'done'");
    Expect(!result.IsAbrupt(), "the script ran: " + js::ToString(result.value));
    // Asked of the document rather than of the script, so this cannot pass by
    // the bindings agreeing with themselves.
    const std::string html = bound.document->SerializeChildren();
    Expect(html.find("<span class=\"added\">from script</span>") != std::string::npos,
           "the document itself changed: " + html);
  });
}

}  // namespace microbrowser::tests
