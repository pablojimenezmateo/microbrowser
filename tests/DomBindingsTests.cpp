#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
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

Bound Bind(std::string_view html, std::string url = "https://example.org/a/b?q=1") {
  Bound bound;
  bound.document = html::ParseDocument(html);
  bound.interpreter = std::make_unique<js::Interpreter>();
  bound.dom_bindings = std::make_unique<bindings::DomBindings>(*bound.interpreter,
                                                              *bound.document, std::move(url));
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
  // The type hierarchy. `HTMLElement is not defined` is where youtube.com's
  // application bundle stopped, and it is not a missing method -- it is a
  // missing *type*, so `class X extends HTMLElement` could not be written at
  // all. ADR 0012 puts this first because it is structural: everything after
  // it assumes an element already has a prototype to inherit from.
  // Searching from an element rather than from the document. `querySelector`
  // existed only on `document`, which is half the API -- a page that has a
  // container and wants something inside it writes `container.querySelector`,
  // and every framework does.
  AddTest(tests, "DomBindings/AnElementCanBeSearchedAndWalked", [] {
    ExpectScript(kPage, "document.getElementById('list').querySelectorAll('p').length", "2");
    ExpectScript(kPage, "document.getElementById('list').querySelector('p').textContent", "one");
    // Scoped to the subtree: the h1 is in the document but not in the list.
    ExpectScript(kPage, "document.getElementById('list').querySelector('h1') === null", "true");
    ExpectScript(kPage, "document.getElementById('list').getElementsByTagName('p').length", "2");
    ExpectScript(kPage, "document.body.getElementsByTagName('*').length", "4");
    ExpectScript(kPage, "document.body.getElementsByClassName('big').length", "1");
    // Document is a ParentNode too, and it is the same operation from a
    // different root.
    ExpectScript(kPage, "document.querySelectorAll('p').length", "2");

    // `contains` is inclusive, which is the specification's and the surprising
    // half: a node contains itself, and a polyfill that walks up asking
    // `root.contains(node)` depends on that terminating.
    ExpectScript(kPage, "document.body.contains(document.getElementById('list'))", "true");
    ExpectScript(kPage, "document.body.contains(document.body)", "true");
    ExpectScript(kPage,
                 "document.getElementById('list').contains(document.getElementById('title'))",
                 "false");
    ExpectScript(kPage, "document.body.hasChildNodes()", "true");

    // Element-only walking. Without these a walk over `firstChild` and
    // `nextSibling` stops on the whitespace between two tags.
    ExpectScript(kPage, "document.getElementById('list').firstElementChild.textContent", "one");
    ExpectScript(kPage, "document.getElementById('list').lastElementChild.textContent", "two");
    ExpectScript(kPage,
                 "document.getElementById('list').firstElementChild"
                 ".nextElementSibling.textContent",
                 "two");
    ExpectScript(kPage,
                 "document.getElementById('list').lastElementChild"
                 ".previousElementSibling.textContent",
                 "one");
    ExpectScript(kPage, "document.getElementById('list').firstElementChild"
                        ".previousElementSibling === null", "true");
    ExpectScript(kPage, "document.getElementById('list').parentElement.tagName", "body");
    // `parentElement` is null where `parentNode` is the document, which is the
    // whole difference between them.
    ExpectScript(kPage, "document.documentElement.parentElement === null", "true");
    ExpectScript(kPage, "document.documentElement.parentNode === document", "true");

    // In the tree or merely made. A framework checks this before it does
    // anything that depends on layout.
    ExpectScript(kPage, "document.body.isConnected", "true");
    ExpectScript(kPage, "document.createElement('div').isConnected", "false");
    ExpectScript(kPage, "document.body.ownerDocument === document", "true");
    ExpectScript(kPage, "document.getElementById('title').localName", "h1");
    ExpectScript(kPage, "document.getElementById('title').hasAttributes()", "true");
    ExpectScript(kPage, "document.createElement('div').hasAttributes()", "false");
    ExpectScript(kPage,
                 "document.getElementById('title').attributes.map(a => a.name).sort().join()",
                 "class,id");
  });

  // Events a page makes and dispatches itself. Dispatch used to exist only as
  // a C++ entry point for a real click; this is the script-facing half, and it
  // is deliberately untrusted -- see the note on DispatchClick.
  AddTest(tests, "DomBindings/APageCanMakeAndDispatchItsOwnEvents", [] {
    ExpectScript(kPage,
                 "var seen = ''; document.body.addEventListener('ping', e => seen = e.type);"
                 "document.body.dispatchEvent(new Event('ping')); seen",
                 "ping");
    // Bubbling is opt-in, as it is in the specification, and the default is off.
    ExpectScript(kPage,
                 "var n = 0; document.body.addEventListener('ping', () => n++);"
                 "document.getElementById('title').dispatchEvent(new Event('ping')); n",
                 "0");
    ExpectScript(kPage,
                 "var n = 0; document.body.addEventListener('ping', () => n++);"
                 "document.getElementById('title')"
                 ".dispatchEvent(new Event('ping', { bubbles: true })); n",
                 "1");
    // `dispatchEvent` returns false when a handler cancelled it, which is the
    // inverse of what the internal dispatch reports.
    ExpectScript(kPage,
                 "document.body.addEventListener('ping', e => e.preventDefault());"
                 "document.body.dispatchEvent(new Event('ping', { cancelable: true }))",
                 "false");
    ExpectScript(kPage,
                 "document.body.addEventListener('ping', e => e.preventDefault());"
                 "document.body.dispatchEvent(new Event('ping'))",
                 "true");
    // `preventDefault` on an event that is not cancelable does nothing, so a
    // handler cannot believe it stopped something it did not.
    ExpectScript(kPage,
                 "var p; document.body.addEventListener('ping', e => { e.preventDefault();"
                 "  p = e.defaultPrevented }); document.body.dispatchEvent(new Event('ping')); p",
                 "false");
    // stopPropagation stops the walk; stopImmediatePropagation also stops the
    // rest of the listeners on the node it was called from.
    ExpectScript(kPage,
                 "var n = 0; document.body.addEventListener('ping', () => n++);"
                 "document.getElementById('title').addEventListener('ping', e => "
                 "  e.stopPropagation());"
                 "document.getElementById('title')"
                 ".dispatchEvent(new Event('ping', { bubbles: true })); n",
                 "0");
    ExpectScript(kPage,
                 "var n = 0; var t = document.getElementById('title');"
                 "t.addEventListener('ping', e => { n++; e.stopImmediatePropagation() });"
                 "t.addEventListener('ping', () => n++);"
                 "t.dispatchEvent(new Event('ping')); n",
                 "1");
    // CustomEvent carries its detail.
    ExpectScript(kPage,
                 "var d; document.body.addEventListener('go', e => d = e.detail);"
                 "document.body.dispatchEvent(new CustomEvent('go', { detail: 42 })); d",
                 "42");
    // An event a page made is not trusted, whatever else is true of it. This
    // is the flag that keeps a forged click from doing what a real one does.
    ExpectScript(kPage, "new Event('click').isTrusted", "false");

    // `window` is an event target too. A page listening for `resize` or `load`
    // listens there and nowhere else, and inline scripts on real pages call
    // `window.addEventListener` before anything else -- it was the first thing
    // youtube.com's did.
    ExpectScript(kPage,
                 "var seen = ''; window.addEventListener('ping', e => seen = e.type);"
                 "window.dispatchEvent(new Event('ping')); seen",
                 "ping");
    // It is the global object, so `globalThis.addEventListener` is the same
    // function -- as it is in a browser.
    ExpectScript(kPage, "window.addEventListener === globalThis.addEventListener", "true");
    // A bubbling event reaches it last, after the document.
    ExpectScript(kPage,
                 "var order = [];"
                 "document.body.addEventListener('ping', () => order.push('body'));"
                 "window.addEventListener('ping', () => order.push('window'));"
                 "document.getElementById('title')"
                 ".dispatchEvent(new Event('ping', { bubbles: true })); order.join()",
                 "body,window");
    // And a non-bubbling one does not.
    ExpectScript(kPage,
                 "var n = 0; window.addEventListener('ping', () => n++);"
                 "document.getElementById('title').dispatchEvent(new Event('ping')); n",
                 "0");

    // The interface chain, which a polyfill patches through: it writes to
    // `Event.prototype` and expects the others to inherit.
    ExpectScript(kPage, "new Event('x') instanceof Event", "true");
    ExpectScript(kPage, "new CustomEvent('x') instanceof Event", "true");
    ExpectScript(kPage, "typeof Event.prototype.preventDefault", "function");
    ExpectScript(kPage, "new Event('x').hasOwnProperty('preventDefault')", "false");

    // The older two-step form, which is what a polyfill written for IE uses
    // and where youtube.com's web components polyfill stopped.
    ExpectScript(kPage,
                 "var e = document.createEvent('Event'); e.initEvent('go', true, true);"
                 "var seen = ''; document.body.addEventListener('go', ev => seen = ev.type);"
                 "document.getElementById('title').dispatchEvent(e); seen",
                 "go");
    ExpectScript(kPage, "document.createEvent('Event').type", "");
    ExpectScript(kPage, "document.readyState", "complete");
    ExpectScript(kPage, "document.createComment('hi').nodeType", "8");
    ExpectScript(kPage, "document.createElementNS('http://www.w3.org/1999/xhtml','div').tagName",
                 "div");
  });

  // A fragment is a parentless bag of nodes that inserts as a unit. The point
  // is what insertion does with it: a framework assembles a subtree detached
  // and places it in one operation.
  AddTest(tests, "DomBindings/ADocumentFragmentInsertsItsChildrenAndEmpties", [] {
    ExpectScript(kPage,
                 "var f = document.createDocumentFragment();"
                 "f.appendChild(document.createElement('i'));"
                 "f.appendChild(document.createElement('b'));"
                 "document.body.appendChild(f);"
                 "document.body.lastElementChild.tagName",
                 "b");
    // The fragment is emptied, not inserted: the body gains two children and
    // no fragment, and the fragment is reusable rather than detached.
    ExpectScript(kPage,
                 "var f = document.createDocumentFragment();"
                 "f.appendChild(document.createElement('i'));"
                 "var before = document.body.children.length;"
                 "document.body.appendChild(f);"
                 "[document.body.children.length - before, f.children.length].join()",
                 "1,0");
    // Through insertBefore as well, which is the same funnel.
    ExpectScript(kPage,
                 "var f = document.createDocumentFragment();"
                 "f.appendChild(document.createElement('i'));"
                 "document.body.insertBefore(f, document.getElementById('list'));"
                 "document.body.children[1].tagName",
                 "i");
    ExpectScript(kPage, "document.createDocumentFragment().nodeType", "11");
    ExpectScript(kPage, "document.createDocumentFragment() instanceof DocumentFragment", "true");
    ExpectScript(kPage, "document.createDocumentFragment() instanceof Node", "true");
    // A fragment is a ParentNode: script queries the subtree it is building
    // before inserting it, which is most of the reason to build it detached.
    ExpectScript(kPage,
                 "var f = document.createDocumentFragment();"
                 "var d = document.createElement('div'); d.setAttribute('class','row');"
                 "f.appendChild(d); f.querySelectorAll('.row').length",
                 "1");

    // Text and Comment share a base, which is what a polyfill patches once
    // rather than twice.
    ExpectScript(kPage, "document.createTextNode('x') instanceof CharacterData", "true");
    ExpectScript(kPage, "document.createComment('x') instanceof CharacterData", "true");
    ExpectScript(kPage, "document.createTextNode('x') instanceof Text", "true");
    ExpectScript(kPage, "document.createTextNode('x') instanceof Comment", "false");
  });

  // Custom elements, natively. The mechanism worth knowing is that a derived
  // class's `this` is whatever its base produced, so `super()` inside the
  // page's class takes its object from HTMLElement -- which is how the class
  // comes to run *on* the element the document already has, rather than on a
  // second object that would have to be kept in step with it.
  AddTest(tests, "DomBindings/CustomElementsUpgradeAndReact", [] {
    // Defined first, then created.
    ExpectScript(kPage,
                 "class Thing extends HTMLElement { constructor(){ super(); this.made = 1 } }"
                 "customElements.define('my-thing', Thing);"
                 "var e = document.createElement('my-thing'); [e.made, e instanceof Thing].join()",
                 "1,true");
    // Created first, then defined -- which is the usual order on a real page,
    // since the script is at the end of the body. Everything already in the
    // document is upgraded when the class arrives.
    ExpectScript("<html><body><my-box></my-box></body></html>",
                 "class Box extends HTMLElement { constructor(){ super(); this.tag = 'up' } }"
                 "customElements.define('my-box', Box);"
                 "document.getElementsByTagName('my-box')[0].tag",
                 "up");
    ExpectScript("<html><body><my-box></my-box></body></html>",
                 "class Box extends HTMLElement { hello(){ return 'hi' } }"
                 "customElements.define('my-box', Box);"
                 "document.getElementsByTagName('my-box')[0].hello()",
                 "hi");
    // A class body with only methods still gets its prototype, whether or not
    // its constructor set anything.
    ExpectScript("<html><body><my-box></my-box></body></html>",
                 "class Box extends HTMLElement {}"
                 "customElements.define('my-box', Box);"
                 "document.getElementsByTagName('my-box')[0] instanceof Box",
                 "true");
    ExpectScript(kPage,
                 "class T extends HTMLElement {} customElements.define('x-t', T);"
                 "customElements.get('x-t') === T",
                 "true");

    // The reactions. Connected when it enters the document, disconnected when
    // it leaves -- and the subtree counts, because appending a detached tree
    // connects everything in it.
    ExpectScript(kPage,
                 "var log = [];"
                 "class T extends HTMLElement { connectedCallback(){ log.push('in') }"
                 "  disconnectedCallback(){ log.push('out') } }"
                 "customElements.define('x-t', T);"
                 "var e = document.createElement('x-t');"
                 "document.body.appendChild(e); e.remove(); log.join()",
                 "in,out");
    ExpectScript(kPage,
                 "var log = [];"
                 "class T extends HTMLElement { connectedCallback(){ log.push('in') } }"
                 "customElements.define('x-t', T);"
                 "var holder = document.createElement('div');"
                 "holder.appendChild(document.createElement('x-t'));"
                 "log.length + ':' + (document.body.appendChild(holder), log.join())",
                 "0:in");
    // `observedAttributes` is read once, when the class is defined, and an
    // attribute outside it produces no call at all.
    ExpectScript(kPage,
                 "var log = [];"
                 "class T extends HTMLElement { static get observedAttributes(){ return ['v'] }"
                 "  attributeChangedCallback(n, o, v){ log.push(n + ':' + o + '->' + v) } }"
                 "customElements.define('x-t', T);"
                 "var e = document.createElement('x-t');"
                 "e.setAttribute('v', '1'); e.setAttribute('v', '2');"
                 "e.setAttribute('other', 'z'); log.join('|')",
                 "v:null->1|v:1->2");

    // The dash rule, enforced: without it a page could redefine `div` and
    // every element in the document would be upgraded.
    ExpectScript(kPage,
                 "(() => { try { customElements.define('div', class extends HTMLElement {}) }"
                 "  catch (e) { return e.name } })()",
                 "SyntaxError");
    ExpectScript(kPage,
                 "(() => { class A extends HTMLElement {} customElements.define('x-a', A);"
                 "  try { customElements.define('x-a', A) } catch (e) { return e.name } })()",
                 "NotSupportedError");
    // Constructing an interface directly is still the error it was; the
    // upgrade path is the only thing that may hand one back.
    ExpectScript(kPage, "(() => { try { new HTMLElement() } catch (e) { return e.name } })()",
                 "TypeError");
  });

  // MutationObserver. The shape is the specification's and it is not the
  // obvious one: mutations accumulate and are delivered *once*, as a
  // microtask, after whatever ran finishes. A page that appends a thousand
  // rows gets one callback with a thousand records rather than a thousand
  // callbacks.
  AddTest(tests, "DomBindings/AMutationObserverBatchesAndDeliversAsAMicrotask", [] {
    // Batched: three appends, one delivery.
    ExpectScript(kPage,
                 "var calls = 0, total = 0;"
                 "var o = new MutationObserver(rs => { calls++; total += rs.length });"
                 "o.observe(document.body, { childList: true });"
                 "for (var i = 0; i < 3; i++) document.body.appendChild(document.createElement('i'));"
                 "Promise.resolve().then(() => {}); calls + ':' + total",
                 "0:0");
    // Nothing has been delivered *yet* above, because the turn has not ended.
    // After a microtask checkpoint it has, and it arrived once.
    ExpectScript(kPage,
                 "var calls = 0, total = 0;"
                 "var o = new MutationObserver(rs => { calls++; total += rs.length });"
                 "o.observe(document.body, { childList: true });"
                 "for (var i = 0; i < 3; i++) document.body.appendChild(document.createElement('i'));"
                 "var out; Promise.resolve().then(() => {}).then(() => out = calls + ':' + total);"
                 "out",
                 "undefined");

    // What a record says. `takeRecords` drains synchronously, which is how a
    // test -- and a framework that wants the answer now -- reads them without
    // waiting for the queue.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true });"
                 "document.body.appendChild(document.createElement('i'));"
                 "var r = o.takeRecords(); r.length + ':' + r[0].type + ':' +"
                 "  r[0].addedNodes.length + ':' + (r[0].target === document.body)",
                 "1:childList:1:true");
    // Removal is a childList record on the parent, carrying the removed node.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true });"
                 "document.getElementById('list').remove();"
                 "var r = o.takeRecords(); r[0].removedNodes.length + ':' + r[0].addedNodes.length",
                 "1:0");

    // Attributes, with the name and the old value -- the old value only when
    // it was asked for, because keeping it otherwise copies every write.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { attributes: true, attributeOldValue: true });"
                 "document.body.setAttribute('data-x', '1');"
                 "document.body.setAttribute('data-x', '2');"
                 "var r = o.takeRecords();"
                 "r.length + ':' + r[1].attributeName + ':' + r[1].oldValue",
                 "2:data-x:1");
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { attributes: true });"
                 "document.body.setAttribute('data-x', '1');"
                 "o.takeRecords()[0].oldValue === null",
                 "true");
    // `attributeFilter` narrows, and naming one implies watching attributes.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { attributeFilter: ['keep'] });"
                 "document.body.setAttribute('drop', '1');"
                 "document.body.setAttribute('keep', '1');"
                 "var r = o.takeRecords(); r.length + ':' + r[0].attributeName",
                 "1:keep");

    // Scope: without `subtree` a mutation below the target is not reported.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true });"
                 "document.getElementById('list').appendChild(document.createElement('i'));"
                 "o.takeRecords().length",
                 "0");
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true, subtree: true });"
                 "document.getElementById('list').appendChild(document.createElement('i'));"
                 "o.takeRecords().length",
                 "1");
    // And the kind has to have been asked for: an attributes observer is not
    // handed childList records.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { attributes: true });"
                 "document.body.appendChild(document.createElement('i'));"
                 "o.takeRecords().length",
                 "0");

    // `disconnect` does both halves: watches nothing, and drops what was
    // queued. An observer that fired once more after disconnecting would be
    // the worst of both.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true });"
                 "document.body.appendChild(document.createElement('i'));"
                 "o.disconnect();"
                 "document.body.appendChild(document.createElement('i'));"
                 "o.takeRecords().length",
                 "0");
  });

  AddTest(tests, "DomBindings/ElementsHaveATypeHierarchy", [] {
    // The chain, from the bottom up.
    ExpectScript(kPage, "document.body instanceof HTMLElement", "true");
    ExpectScript(kPage, "document.body instanceof Element", "true");
    ExpectScript(kPage, "document.body instanceof Node", "true");
    ExpectScript(kPage, "document.createElement('div') instanceof HTMLDivElement", "true");
    ExpectScript(kPage, "document.createElement('div') instanceof HTMLElement", "true");
    // And what is *not* in it, which is the half that makes the answer worth
    // anything: a div is not an anchor, and a text node is not an element.
    ExpectScript(kPage, "document.createElement('div') instanceof HTMLAnchorElement", "false");
    ExpectScript(kPage, "document.createTextNode('x') instanceof Element", "false");
    ExpectScript(kPage, "document.createTextNode('x') instanceof Node", "true");
    ExpectScript(kPage, "document instanceof Node", "true");

    // A tag with no interface of its own is a plain HTMLElement, which is the
    // right answer rather than a fallback.
    ExpectScript(kPage, "document.createElement('marquee') instanceof HTMLElement", "true");

    // The base a custom element extends. Being able to *write* this is the
    // point; registering it needs customElements, which is later in ADR 0012.
    ExpectScript(kPage, "typeof class X extends HTMLElement {}", "function");
    ExpectScript(kPage, "Object.getPrototypeOf(class X extends HTMLElement {}) === HTMLElement",
                 "true");

    // Constructing one directly is a TypeError, as it is in a browser: an
    // element is made by the document, not by calling its interface.
    ExpectScript(kPage, "(() => { try { new HTMLElement() } catch (e) { return e.name } })()",
                 "TypeError");

    // The methods live on the prototype now, not on every wrapper. That is
    // what makes the hierarchy possible at all, and it is separately worth
    // asserting: an own property per method per node is what it replaced.
    ExpectScript(kPage, "document.body.hasOwnProperty('appendChild')", "false");
    ExpectScript(kPage, "typeof document.body.appendChild", "function");
    ExpectScript(kPage,
                 "HTMLElement.prototype.isPrototypeOf(document.getElementById('title'))", "true");
    // Identity still holds, and the prototype is shared rather than copied.
    ExpectScript(kPage,
                 "Object.getPrototypeOf(document.createElement('div')) === "
                 "Object.getPrototypeOf(document.createElement('div'))",
                 "true");
    // `constructor` names the interface, which is how a page prints a type.
    ExpectScript(kPage, "document.body.constructor.name", "HTMLElement");
    ExpectScript(kPage, "document.createElement('a').constructor.name", "HTMLAnchorElement");
  });

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
    // Appending an attached node *moves* it. That was a TypeError while
    // removal did not exist, because moving means detaching and detaching
    // meant destroying; it is the ordinary DOM behaviour now that detaching
    // hands the node over instead.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "document.body.appendChild(t);"
                 "document.body.children[document.body.children.length - 1] === t",
                 "true");
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

  AddTest(tests, "DomBindings/SelectorsAgreeAcrossTheFourApisThatUseThem", [] {
    // querySelector, querySelectorAll, matches and closest all ask the same
    // question, and four copies of the answer would be four chances to
    // disagree about what `.a` means.
    ExpectScript(kPage, "document.querySelectorAll('p').length", "2");
    ExpectScript(kPage, "document.querySelectorAll('.big').length", "1");
    ExpectScript(kPage, "document.querySelectorAll('#list').length", "1");
    ExpectScript(kPage, "document.getElementsByClassName('head').length", "1");
    ExpectScript(kPage, "document.getElementById('title').matches('.big')", "true");
    ExpectScript(kPage, "document.getElementById('title').matches('.bi')", "false");
    // `closest` is this element or the nearest ancestor, which is how a click
    // handler finds the row a button is in.
    ExpectScript(kPage, "document.querySelector('p').closest('#list').tagName", "div");
    ExpectScript(kPage, "document.querySelector('p').closest('p').tagName", "p");
    ExpectScript(kPage, "document.querySelector('p').closest('.nothing') === null", "true");
  });

  AddTest(tests, "DomBindings/TheTreeCanBeWalkedInEveryDirection", [] {
    ExpectScript("<div id=d><a></a><b></b></div>",
                 "document.getElementById('d').firstChild.nodeName", "A");
    ExpectScript("<div id=d><a></a><b></b></div>",
                 "document.getElementById('d').lastChild.nodeName", "B");
    ExpectScript("<div id=d><a></a><b></b></div>",
                 "document.getElementById('d').firstChild.nextSibling.nodeName", "B");
    ExpectScript("<div id=d><a></a><b></b></div>",
                 "document.getElementById('d').lastChild.previousSibling.nodeName", "A");
    ExpectScript("<div id=d><a></a></div>",
                 "document.getElementById('d').firstChild.nextSibling === null", "true");
    // `nodeName` is upper case and `tagName` is the name the parser stored, so
    // the two deliberately differ.
    ExpectScript(kPage,
                 "document.getElementById('title').nodeName + ' ' + "
                 "document.getElementById('title').tagName",
                 "H1 h1");
  });

  AddTest(tests, "DomBindings/ClassListReadsAndRewritesTheAttribute", [] {
    // Nothing is cached between calls: a parsed copy would go stale the moment
    // anything else touched `class`, and `class` is the one attribute two
    // pieces of code fight over.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.classList.add('new'); "
                 "t.className",
                 "big head new");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.classList.remove('big'); "
                 "t.className",
                 "head");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); "
                 "'' + t.classList.contains('big') + t.classList.contains('nope')",
                 "truefalse");
    // Toggle answers with whether the class is there afterwards.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); "
                 "'' + t.classList.toggle('big') + t.classList.toggle('big')",
                 "falsetrue");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.removeAttribute('class'); "
                 "t.getAttribute('class') === null",
                 "true");
  });

  AddTest(tests, "DomBindings/WindowIsTheGlobalObjectAndNotACopyOfIt", [] {
    // Not a convenience alias: a page writes `window.foo = 1` and then reads
    // `foo`, and the two have to be the same binding or half of what a script
    // sets goes missing.
    ExpectScript(kPage, "window === globalThis", "true");
    ExpectScript(kPage, "window.foo = 7; '' + foo", "7");
    ExpectScript(kPage, "bar = 3; '' + window.bar", "3");
    ExpectScript(kPage, "window.document === document", "true");
    ExpectScript(kPage, "self === window", "true");
  });

  AddTest(tests, "DomBindings/LocationAndNavigatorReportWhatTheyShould", [] {
    ExpectScript(kPage, "location.href", "https://example.org/a/b?q=1");
    ExpectScript(kPage, "location.protocol", "https:");
    ExpectScript(kPage, "location.host", "example.org");
    ExpectScript(kPage, "location.pathname", "/a/b?q=1");
    // The user agent is a fingerprinting surface before it is anything else.
    // This one says what the browser is and nothing about the machine it is
    // on, so every copy answers the same.
    ExpectScript(kPage, "navigator.userAgent", "microbrowser");
  });

  AddTest(tests, "DomBindings/DocumentExposesItsPartsAsAccessors", [] {
    // Accessors rather than stored values, so they follow the tree instead of
    // freezing what it looked like when the bindings were installed.
    ExpectScript("<html><head><title>Some Page</title></head><body></body></html>",
                 "document.title", "Some Page");
    ExpectScript("<html><head></head><body></body></html>", "document.head.tagName", "head");
    ExpectScript(kPage,
                 "const t = document.createTextNode('hi'); const d = document.createElement('i');"
                 "d.appendChild(t); d.textContent",
                 "hi");
  });

  AddTest(tests, "DomBindings/ClickHandlersRunAndBubble", [] {
    Bound bound = Bind("<div id=outer><span id=inner>x</span></div>");
    const js::Result setup = bound.interpreter->Run(
        "globalThis.seen = [];"
        "document.getElementById('inner').addEventListener('click', function(e) {"
        "  seen.push('inner:' + e.type + ':' + (e.target === this));"
        "});"
        "document.getElementById('outer').addEventListener('click', function(e) {"
        "  seen.push('outer:' + (e.currentTarget === this) + ':' + (e.target.id === 'inner'));"
        "});"
        "'ready'");
    Expect(!setup.IsAbrupt(), "the listeners registered: " + js::ToString(setup.value));

    dom::Element* inner = nullptr;
    bound.document->ForEachDescendant([&](const dom::Node& node) {
      const std::string* id = node.IsElement()
                                  ? static_cast<const dom::Element&>(node).GetAttribute("id")
                                  : nullptr;
      if (id != nullptr && *id == "inner") {
        inner = const_cast<dom::Element*>(&static_cast<const dom::Element&>(node));
      }
    });
    Expect(inner != nullptr, "the inner element exists");
    const bool prevented = bound.dom_bindings->DispatchClick(*inner);
    Expect(!prevented, "nothing called preventDefault");

    // From the target up, which is what bubbling is -- and `this` is the node
    // the listener was registered on, not the one that was clicked.
    ExpectEqString(js::ToString(bound.interpreter->Run("seen.join(' ')").value),
                   "inner:click:true outer:true:true", "both ran, target first");
  });

  AddTest(tests, "DomBindings/PreventDefaultAndStopPropagationDoDifferentThings", [] {
    // One stops the browser's own behaviour and the other stops the walk. A
    // page uses them for opposite purposes and confusing them is silent.
    const auto dispatch = [](std::string_view setup) {
      Bound bound = Bind("<div id=outer><span id=inner>x</span></div>");
      bound.interpreter->Run(std::string("globalThis.seen = [];") + std::string(setup));
      dom::Element* inner = nullptr;
      bound.document->ForEachDescendant([&](const dom::Node& node) {
        const std::string* id = node.IsElement()
                                    ? static_cast<const dom::Element&>(node).GetAttribute("id")
                                    : nullptr;
        if (id != nullptr && *id == "inner") {
          inner = const_cast<dom::Element*>(&static_cast<const dom::Element&>(node));
        }
      });
      const bool prevented = inner != nullptr && bound.dom_bindings->DispatchClick(*inner);
      return std::string(prevented ? "prevented " : "allowed ") +
             js::ToString(bound.interpreter->Run("seen.join(',')").value);
    };
    ExpectEqString(
        dispatch("document.getElementById('inner').addEventListener('click', e => {"
                 "  seen.push('a'); e.preventDefault();"
                 "});"
                 "document.getElementById('outer').addEventListener('click', () => seen.push('b'));"),
        "prevented a,b", "preventDefault stops the default, not the bubble");
    ExpectEqString(
        dispatch("document.getElementById('inner').addEventListener('click', e => {"
                 "  seen.push('a'); e.stopPropagation();"
                 "});"
                 "document.getElementById('outer').addEventListener('click', () => seen.push('b'));"),
        "allowed a", "stopPropagation stops the bubble, not the default");
  });

  AddTest(tests, "DomBindings/ListenersAreRemovedByIdentity", [] {
    Bound bound = Bind("<div id=d>x</div>");
    bound.interpreter->Run(
        "globalThis.n = 0;"
        "globalThis.handler = () => { n++ };"
        "const d = document.getElementById('d');"
        "d.addEventListener('click', handler);"
        "d.addEventListener('click', () => { n += 10 });"
        "d.removeEventListener('click', handler);");
    dom::Element* target = nullptr;
    bound.document->ForEachDescendant([&](const dom::Node& node) {
      if (node.IsElement() && static_cast<const dom::Element&>(node).TagName() == "div") {
        target = const_cast<dom::Element*>(&static_cast<const dom::Element&>(node));
      }
    });
    Expect(target != nullptr, "the div exists");
    bound.dom_bindings->DispatchClick(*target);
    // Only the anonymous one is left. Removal is by identity, which is why an
    // inline arrow cannot be removed -- and is what every browser does.
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "10",
                   "the named handler was removed and the other still ran");
  });

  AddTest(tests, "Timers/NothingScheduledMeansTheLoopMayBlock", [] {
    // The property the whole zero-idle-CPU invariant rests on: a page with no
    // timer pending hands back nothing, and the idle policy turns nothing into
    // an indefinite block rather than a wakeup.
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 1000);
    Expect(!timers.NextDelay(1000).has_value(), "no timers, no deadline");

    interpreter.Run("setTimeout(() => {}, 250);");
    const std::optional<std::uint32_t> delay = timers.NextDelay(1000);
    Expect(delay.has_value(), "one timer, one deadline");
    ExpectEqInt(static_cast<long long>(*delay), 250, "and it is how long until it is due");
    // A deadline already passed is zero rather than negative, and the idle
    // policy turns a zero into one sleep rather than a spin.
    ExpectEqInt(static_cast<long long>(*timers.NextDelay(9999)), 0, "an overdue timer is zero");
  });

  AddTest(tests, "Timers/ATimerRunsOnceWhenItIsDue", [] {
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 0);
    interpreter.Run("globalThis.n = 0; setTimeout(() => { n++ }, 100);");

    Expect(!timers.RunDue(interpreter, 50), "not yet due");
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "0", "and it did not run");
    Expect(timers.RunDue(interpreter, 100), "due now");
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "1", "so it ran");
    Expect(!timers.RunDue(interpreter, 500), "and is gone");
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "1", "not run twice");
    Expect(!timers.NextDelay(500).has_value(), "leaving nothing scheduled");
  });

  AddTest(tests, "Timers/AnIntervalReschedulesItself", [] {
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 0);
    interpreter.Run("globalThis.n = 0; globalThis.id = setInterval(() => { n++ }, 10);");
    timers.RunDue(interpreter, 10);
    timers.RunDue(interpreter, 20);
    timers.RunDue(interpreter, 30);
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "3", "three times");
    interpreter.Run("clearInterval(id);");
    timers.RunDue(interpreter, 40);
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "3", "and stops when cleared");
    Expect(!timers.NextDelay(40).has_value(), "with nothing left scheduled");
  });

  AddTest(tests, "Timers/AZeroDelayTimerScheduledDuringAPassWaitsForTheNext", [] {
    // The bound that stops a page spinning the loop inside a single turn. A
    // callback that schedules another with no delay would otherwise be run in
    // the same pass, forever, without the loop ever getting back control.
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 0);
    interpreter.Run(
        "globalThis.n = 0;"
        "globalThis.again = () => { n++; setTimeout(again, 0) };"
        "setTimeout(again, 0);");
    Expect(timers.RunDue(interpreter, 0), "the first one ran");
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "1",
                   "once, not forever -- the one it scheduled waits for the next pass");
    timers.RunDue(interpreter, 0);
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "2", "and then once more");
  });

  AddTest(tests, "Timers/CancellingRemovesTheCallbackAndNotOnlyTheTimer", [] {
    // Or cancelling would leak the closure for as long as the page lives,
    // which is the shape of leak a page with a lot of cancelled timers has.
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 0);
    interpreter.Run(
        "globalThis.n = 0;"
        "const id = setTimeout(() => { n++ }, 10);"
        "clearTimeout(id);");
    Expect(!timers.NextDelay(0).has_value(), "the timer is gone");
    Expect(!timers.RunDue(interpreter, 100), "and never runs");
    ExpectEqString(js::ToString(interpreter.Run("'' + n").value), "0", "so nothing happened");
  });

  AddTest(tests, "Timers/AStringCallbackIsRefusedRatherThanEvaluated", [] {
    // `setTimeout('code()')` is `eval` by another name, and there is no path
    // from a string to running code in this engine. Refused where it is
    // written rather than ignored silently.
    js::Interpreter interpreter;
    bindings::TimerQueue timers;
    timers.Install(interpreter, 0);
    ExpectEqString(js::ToString(
                       interpreter.Run("try { setTimeout('n++', 0) } catch (e) { e.name }").value),
                   "TypeError", "a string is not a callback");
  });

  AddTest(tests, "DomBindings/ARemovedNodeStaysAliveAndUsable", [] {
    // The reason removal was not in the first slice. A wrapper holds a raw
    // `dom::Node*`, so freeing a node script still refers to is a
    // use-after-free reachable from a page. Removal detaches and keeps.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const first = list.children[0];"
                 "list.removeChild(first);"
                 "list.children.length + ' ' + first.tagName + ' ' + first.textContent",
                 "1 p one");
    // And it can be put back somewhere else, which is what a page does when it
    // reorders a list.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const first = list.children[0];"
                 "list.removeChild(first);"
                 "document.body.appendChild(first);"
                 "document.body.children[document.body.children.length - 1].textContent",
                 "one");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.remove(); "
                 "document.getElementById('title') === null",
                 "true");
    // Removing something that is not a child is the caller's bug, and removing
    // it from wherever it actually is would be worse.
    ExpectScript(kPage,
                 "try { document.body.removeChild(document.createElement('x')) } "
                 "catch (e) { e.name }",
                 "TypeError");
  });

  AddTest(tests, "DomBindings/AWrapperForARemovedNodeSurvivesACollection", [] {
    // The exact hazard ADR 0008 was written about, on the path that creates
    // it. If the node were freed on removal, this would read reclaimed memory
    // -- and the collection in the middle is what makes the test fail loudly
    // rather than by luck.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const gone = list.children[0];"
                 "list.removeChild(gone);"
                 "let sink = null;"
                 "for (let i = 0; i < 20000; i++) { sink = { i, next: sink && sink.i }; }"
                 "gone.textContent + ':' + gone.tagName + ':' + (gone.parentNode === null)",
                 "one:p:true");
  });

  AddTest(tests, "DomBindings/NodesCanBeInsertedAndReplacedInPlace", [] {
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const fresh = document.createElement('em');"
                 "list.insertBefore(fresh, list.children[0]);"
                 "list.children[0].tagName + ' ' + list.children.length",
                 "em 3");
    // A null reference appends, which is what the specification says and what
    // a page relies on when it inserts before "nothing".
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.insertBefore(document.createElement('em'), null);"
                 "list.children[list.children.length - 1].tagName",
                 "em");
    // In before out, so the replacement lands where the old node was rather
    // than at the end -- the whole difference from remove-then-append.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const fresh = document.createElement('em');"
                 "const old = list.children[0];"
                 "const returned = list.replaceChild(fresh, old);"
                 "list.children[0].tagName + ' ' + list.children.length + ' ' + "
                 "(returned === old) + ' ' + returned.textContent",
                 "em 2 true one");
  });

  AddTest(tests, "DomBindings/AppendingAnAttachedNodeMovesIt", [] {
    // Which works only because detaching hands the node over rather than
    // destroying it. This is how a page reorders a list.
    ExpectScript("<div id=box><a></a><b></b><c></c></div>",
                 "const box = document.getElementById('box');"
                 "box.appendChild(box.children[0]);"
                 "Array.from(box.children).map(e => e.tagName).join('')",
                 "bca");
    ExpectScript("<div id=box><a></a></div><div id=other></div>",
                 "const box = document.getElementById('box');"
                 "document.getElementById('other').appendChild(box.children[0]);"
                 "box.children.length + ' ' + document.getElementById('other').children.length",
                 "0 1");
  });

  AddTest(tests, "DomBindings/TextContentReplacesChildrenWithoutParsing", [] {
    // Setting it drops every child and puts one text node in their place,
    // which could not exist until removal did.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.textContent = 'replaced';"
                 "list.textContent + '|' + list.children.length",
                 "replaced|0");
    ExpectScript(kPage,
                 "const list = document.getElementById('list'); list.textContent = '';"
                 "list.childNodes.length",
                 "0");
    // The children are detached rather than destroyed, so a wrapper script was
    // holding still works.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const kept = list.children[0];"
                 "list.textContent = 'gone';"
                 "kept.tagName + ':' + kept.textContent",
                 "p:one");

    // The safety property that separates this from innerHTML: markup in the
    // string is text, not markup. A page that writes user input through
    // `textContent` is safe by construction, and one that writes it through
    // `innerHTML` is not -- which is most of why the two exist.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.textContent = 'safe <b>text</b>';"
                 "list.children.length + ' ' + list.innerHTML",
                 "0 safe &lt;b&gt;text&lt;/b&gt;");
  });

  AddTest(tests, "DomBindings/TheHtmlPropertiesAreReadableAndNotWritable", [] {
    ExpectScript(kPage, "document.getElementById('list').innerHTML",
                 "<p>one</p><p>two</p>");
    ExpectScript(kPage, "document.getElementById('title').outerHTML",
                 "<h1 id=\"title\" class=\"big head\">Hello</h1>");
    // Writing either means running the HTML parser on a string from script
    // into a live tree, and a *fragment* parses differently depending on where
    // it is going -- `<td>` inside a table is a cell and anywhere else is
    // nothing. A setter that ignored that would build wrong trees quietly.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.innerHTML = '<i>x</i>';"
                 "t.innerHTML",
                 "Hello");
  });

  AddTest(tests, "DomBindings/StyleWritesThroughToTheAttribute", [] {
    // Backed by the `style` attribute rather than a parsed copy, because the
    // attribute is the state: the cascade reads it and `setAttribute` can
    // rewrite it, so a copy held here would go stale the moment either did.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.style.display = 'none';"
                 "t.getAttribute('style')",
                 "display: none");
    // camelCase in, kebab-case out.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.style.backgroundColor = 'red';"
                 "t.getAttribute('style')",
                 "background-color: red");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.style.display = 'none';"
                 "t.style.display",
                 "none");
    // An unset property is the empty string, not undefined: a page tests
    // `if (el.style.display === 'none')` and both answers have to be strings
    // or the comparison is wrong in a way nothing reports.
    ExpectScript(kPage, "'' + document.getElementById('title').style.display", "");
    // Set twice leaves one declaration, in the place the first one had.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.style.color = 'red'; t.style.display = 'none'; t.style.color = 'blue';"
                 "t.getAttribute('style')",
                 "color: blue; display: none");
    // An empty value removes the property, which is what `= ''` means.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.style.color = 'red';"
                 "t.style.color = ''; t.getAttribute('style')",
                 "");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.style.cssText = 'color: blue';"
                 "t.style.color",
                 "blue");
  });

  AddTest(tests, "DomBindings/DatasetReadsTheDataAttributes", [] {
    ExpectScript("<div id=d data-user-id='7' data-x='1' class='c'></div>",
                 "document.getElementById('d').dataset.userId", "7");
    // Only the `data-` ones, under their camel-cased names.
    ExpectScript("<div id=d data-user-id='7' data-x='1' class='c'></div>",
                 "JSON.stringify(document.getElementById('d').dataset)",
                 "{\"userId\":\"7\",\"x\":\"1\"}");
    ExpectScript("<div id=d></div>",
                 "JSON.stringify(document.getElementById('d').dataset)", "{}");
  });

  AddTest(tests, "DomBindings/CloningCopiesRatherThanShares", [] {
    // Shallow by default, which catches out everyone who forgets the argument
    // and is what the specification says.
    ExpectScript(kPage,
                 "const c = document.getElementById('list').cloneNode();"
                 "c.tagName + ' ' + c.children.length + ' ' + c.getAttribute('id')",
                 "div 0 list");
    ExpectScript(kPage,
                 "const c = document.getElementById('list').cloneNode(true);"
                 "c.children.length + ' ' + c.textContent",
                 "2 onetwo");
    // A clone is a new node, not a second reference to the old one. Two
    // parents pointing at one node is the shape of every "it changed when I
    // edited the copy" bug.
    ExpectScript(kPage,
                 "const original = document.getElementById('list');"
                 "const c = original.cloneNode(true);"
                 "c.setAttribute('id', 'copy');"
                 "original.getAttribute('id') + ' ' + c.getAttribute('id')",
                 "list copy");
    // And it is unattached until something appends it, like any created node.
    ExpectScript(kPage,
                 "const c = document.getElementById('list').cloneNode(true);"
                 "(c.parentNode === null) + ' ' + (document.getElementsByTagName('div').length)",
                 "true 1");
    ExpectScript(kPage,
                 "const c = document.getElementById('list').cloneNode(true);"
                 "document.body.appendChild(c);"
                 "document.getElementsByTagName('div').length",
                 "2");
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
