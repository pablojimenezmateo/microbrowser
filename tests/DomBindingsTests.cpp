#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Network.h"
#include "bindings/AnimationFrames.h"
#include "bindings/IdleCallbacks.h"
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
  AddTest(tests, "DomBindings/TextEncoderAndDecoderRoundTripUtf8", [] {
    // Encoding Standard: encode produces UTF-8 bytes; decode undoes them.
    // youtube's PES path is `(new TextEncoder).encode(s).subarray(0, 16)`.
    ExpectScript("<html><body></body></html>",
                 "const e = new TextEncoder();"
                 "const d = new TextDecoder();"
                 "const u = e.encode('hi café');"
                 "u instanceof Uint8Array && d.decode(u) === 'hi café' && e.encoding === 'utf-8'",
                 "true");
    ExpectScript("<html><body></body></html>",
                 "Array.from(new TextEncoder().encode('A')).join(',')", "65");
    ExpectScript("<html><body></body></html>",
                 "(() => { try { new TextDecoder('windows-1252'); return 'ok'; }"
                 "catch (err) { return err.name; } })()",
                 "RangeError");
    ExpectScript("<html><body></body></html>",
                 "new TextDecoder().decode(new Uint8Array([0xEF,0xBB,0xBF,0x61]))", "a");
  });

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
    ExpectScript(kPage, "document.getElementById('list').parentElement.tagName", "BODY");
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
    // NamedNodeMap, not Array: youtube's property binder calls getNamedItem.
    ExpectScript(kPage,
                 "const a = document.getElementById('title').attributes;"
                 "[...a].map(x => x.name).sort().join() + '|' +"
                 " a.getNamedItem('id').value + '|' + a.length + '|' +"
                 " (a.getNamedItem('nope') === null)",
                 "class,id|title|2|true");
    ExpectScript(kPage,
                 "document.getElementById('title').getAttributeNode('class').value",
                 "big head");
  });

  // Events a page makes and dispatches itself. Dispatch used to exist only as
  // a C++ entry point for a real click; this is the script-facing half, and it
  // is deliberately untrusted -- see the note on DispatchClick.
  AddTest(tests, "DomBindings/AConstructedEventIsAnInstanceOfItsOwnConstructor", [] {
    // **The hierarchy existed and nothing was ever an instance of any of it.**
    // `MakeEvent` gives every event `Event.prototype`, which is right for the
    // ones the browser makes, and the constructors returned those unchanged --
    // so `new CustomEvent('x') instanceof CustomEvent` was false, which is the
    // check a page makes before reading `.detail`.
    ExpectScript("<body></body>",
                 "const c = new CustomEvent('x', { detail: 7 });"
                 "(c instanceof CustomEvent) + ',' + (c instanceof Event) + ',' + c.detail",
                 "true,true,7");
    // UIEvent was missing entirely, so a mouse event chained straight to Event
    // and a library patching `UIEvent.prototype` -- which is where a fix meant
    // for every input event at once goes -- reached nothing.
    ExpectScript("<body></body>",
                 "const w = new WheelEvent('wheel', { bubbles: true });"
                 "(w instanceof MouseEvent) + ',' + (w instanceof UIEvent) + ',' +"
                 "(w instanceof Event) + ',' + w.bubbles + ',' + typeof w.stopPropagation",
                 "true,true,true,true,function");
    ExpectScript("<body></body>",
                 "UIEvent.prototype.patched = 1;"
                 "new MouseEvent('click').patched + ',' + new Event('x').patched",
                 "1,undefined");
    // Every name in the list, and the list is what youtube's bundle actually
    // says -- `WheelEvent` is where it stopped, 88% of the way through 10.7MB.
    ExpectScript("<body></body>",
                 "['Event','UIEvent','MouseEvent','KeyboardEvent','FocusEvent','InputEvent',"
                 " 'WheelEvent','PointerEvent','DragEvent','MessageEvent','ProgressEvent',"
                 " 'PromiseRejectionEvent','CustomEvent','ErrorEvent']"
                 "  .filter(n => typeof globalThis[n] !== 'function').join(',')",
                 "");
  });

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
    // "loading" here rather than "complete": these bindings are installed with
    // no lifecycle run over them, which is exactly the state a document is in
    // while its scripts run. The transitions are PageScript's -- see
    // DomBindings/TheDocumentLifecycleIsAStateMachine.
    ExpectScript(kPage, "document.readyState", "loading");
    ExpectScript(kPage, "document.cookie", "");
    ExpectScript(kPage, "document.cookie.match(/foo/) === null", "true");
    ExpectScript(kPage, "document.createComment('hi').nodeType", "8");
    ExpectScript(kPage, "document.createElementNS('http://www.w3.org/1999/xhtml','div').tagName",
                 "DIV");
    // Namespace ignored; youtube's player bootstraps with setAttributeNS(null, …).
    ExpectScript(kPage,
                 "var e = document.createElement('div');"
                 "e.setAttributeNS(null, 'data-x', '1');"
                 "e.getAttributeNS(null, 'data-x')",
                 "1");
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
                 "B");
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
                 "I");
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
    // Polymer text bindings set `textNode.data` and `textNode.textContent`.
    ExpectScript(kPage,
                 "const t = document.createTextNode('[[label]]');"
                 "t.data = 'bound'; t.data + '|' + t.length + '|' + t.nodeValue",
                 "bound|5|bound");
    ExpectScript(kPage,
                 "const t = document.createTextNode('[[label]]');"
                 "t.textContent = 'bound';"
                 "t.data + '|' + t.childNodes.length + '|' + t.textContent",
                 "bound|0|bound");
    ExpectScript(kPage,
                 "const c = document.createComment('[[x]]'); c.data = 'ok'; c.nodeValue",
                 "ok");
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

    // `whenDefined` resolves when the name is registered, and immediately when
    // it already is -- reddit's bundle waits on several custom elements this way.
    ExpectScript(kPage, "typeof customElements.whenDefined", "function");
    ExpectScript(kPage,
                 "customElements.whenDefined('x-miss') instanceof Promise",
                 "true");
    {
      Bound bound = Bind(kPage);
      (void)bound.interpreter->Run(
          "customElements.whenDefined('x-late').then(function (C) { globalThis._late = C.name; });"
          "class Late extends HTMLElement {}"
          "customElements.define('x-late', Late);");
      bound.interpreter->DrainMicrotasks();
      ExpectEqString(js::ToString(bound.interpreter->Run("globalThis._late").value), "Late",
                     "whenDefined settles when the element is defined");
    }

    // The class's prototype is on the element *before* its constructor runs,
    // which is what lets a constructor call the class's own methods. Every
    // framework's base class does exactly this on its first line -- Polymer's
    // calls `this._initializeProperties()` -- so getting the order wrong does
    // not break an edge case, it breaks every component on the page. It did:
    // twenty-nine of youtube.com's thirty-two upgrades threw here, the throws
    // were swallowed, and the page rendered blank.
    ExpectScript(kPage,
                 "class T extends HTMLElement {"
                 "  constructor(){ super(); this.value = this.compute() }"
                 "  compute(){ return 'ready' } }"
                 "customElements.define('x-t', T);"
                 "document.createElement('x-t').value",
                 "ready");
    // The same for an element already in the document, which is the path a
    // real page takes: markup first, class afterwards.
    ExpectScript("<html><body><my-box></my-box></body></html>",
                 "class Box extends HTMLElement {"
                 "  constructor(){ super(); this.seen = this instanceof Box } }"
                 "customElements.define('my-box', Box);"
                 "document.getElementsByTagName('my-box')[0].seen",
                 "true");
    // And a constructor that throws is *reported* rather than swallowed. The
    // element stays in the tree as the specification says, but a failed
    // upgrade that says nothing is indistinguishable from a page with no
    // components at all.
    ExpectScript("<html><body><my-box></my-box></body></html>",
                 "class Box extends HTMLElement { constructor(){ super(); throw new Error('no') } }"
                 "customElements.define('my-box', Box);"
                 "console.log('marker'); 'done'",
                 "done");

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
    // The parser makes elements and knows nothing about a registry, so a
    // subtree that arrived through `innerHTML` has to be upgraded before it is
    // announced as connected -- `connectedCallback` is a method of the
    // upgraded class, and an element that ran one first would see a half-built
    // object.
    ExpectScript(kPage,
                 "var log = [];"
                 "class T extends HTMLElement { constructor(){ super(); log.push('made') }"
                 "  connectedCallback(){ log.push('in') } }"
                 "customElements.define('x-t', T);"
                 "document.body.innerHTML = '<div><x-t></x-t></div>'; log.join()",
                 "made,in");
    // And a subtree that leaves through the same path is told it left. Before
    // session 14, clearing children ran no reaction at all.
    ExpectScript(kPage,
                 "var log = [];"
                 "class T extends HTMLElement { disconnectedCallback(){ log.push('out') } }"
                 "customElements.define('x-t', T);"
                 "document.body.innerHTML = '<x-t></x-t>';"
                 "document.body.innerHTML = ''; log.join()",
                 "out");
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

  AddTest(tests, "DomBindings/TemplateBindingAttributeValuesArePreserved", [] {
    // Binding tokens are real attribute values until the framework replaces
    // them. Hiding or stripping them blocked Polymer from wiring dom-repeat
    // `items="[[…]]"` and every other attribute binding on youtube.com.
    ExpectScript(kPage,
                 "const el = document.createElement('div');"
                 "el.setAttribute('items', '[[data]]');"
                 "el.getAttribute('items')",
                 "[[data]]");
    ExpectScript(kPage,
                 "const el = document.createElement('div');"
                 "el.setAttribute('prop', '[hostProp]');"
                 "el.getAttribute('prop')",
                 "[hostProp]");
    ExpectScript(kPage,
                 "const el = document.createElement('div');"
                 "el.setAttribute('items', '[1,2,3]');"
                 "el.getAttribute('items')",
                 "[1,2,3]");
    ExpectScript(kPage,
                 "const t = document.createElement('template');"
                 "t.innerHTML = '<span items=\"[[items]]\">[[t]]</span>';"
                 "const stamp = document.importNode(t.content, true);"
                 "const s = stamp.querySelector('span');"
                 "s.getAttribute('items') + '|' + s.textContent",
                 "[[items]]|[[t]]");
    // Binding tokens on a *live* host are stripped *before* the constructor
    // (TD-0017). Polymer deserializes a present boolean attribute as true during
    // `_initializeProperties`, so leaving `hidden="[[data.hideContents]]"` until
    // after construction reflected as `hidden=""` and hid youtube search.
    // Inert template contents keep tokens for `_parseTemplate`.
    ExpectScript("<html><body><x-t v='1'></x-t></body></html>",
                 "var log = [];"
                 "class T extends HTMLElement {"
                 "  static get observedAttributes(){ return ['v'] }"
                 "  attributeChangedCallback(n, o, v){ log.push(n + ':' + o + '->' + v) }"
                 "}"
                 "customElements.define('x-t', T);"
                 "log.join('|')",
                 "v:null->1");
    // Constructor must not see the token — strip precedes ConstructValue.
    ExpectScript("<html><body><x-t v='[[x]]'></x-t></body></html>",
                 "var log = [];"
                 "class T extends HTMLElement {"
                 "  static get observedAttributes(){ return ['v'] }"
                 "  attributeChangedCallback(n, o, v){ log.push(n + ':' + o + '->' + v) }"
                 "  constructor(){ super(); this.seen = this.getAttribute('v') }"
                 "}"
                 "customElements.define('x-t', T);"
                 "document.querySelector('x-t').seen + '|' + "
                 "document.querySelector('x-t').getAttribute('v') + '|' + log.join('|')",
                 "null|null|");
    // Boolean presence: a binding token on `hidden` must not become `hidden=""`
    // via constructor reflect, or UA `[hidden]{display:none}` hides the host.
    ExpectScript("<html><body><x-h hidden='[[data.hideContents]]'></x-h></body></html>",
                 "class H extends HTMLElement {"
                 "  constructor(){"
                 "    super();"
                 "    if (this.hasAttribute('hidden')) this.setAttribute('hidden','');"
                 "  }"
                 "}"
                 "customElements.define('x-h', H);"
                 "document.querySelector('x-h').hasAttribute('hidden')",
                 "false");
  });

  AddTest(tests, "DomBindings/TemplateContentCustomElementsStayInert", [] {
    // `template.innerHTML` must not upgrade custom elements inside `.content`.
    // Doing so stripped Polymer `data="[[…]]"` before `_parseTemplate` ran.
    ExpectScript(
        "<html><body></body></html>",
        "class XFoo extends HTMLElement {"
        "  constructor(){ super(); this.upgraded = true; }"
        "}"
        "customElements.define('x-foo', XFoo);"
        "const t = document.createElement('template');"
        "t.innerHTML = '<x-foo data=\"[[host.data]]\" id=\"primary\"></x-foo>';"
        "const el = t.content.firstElementChild;"
        "const before = (el.upgraded === true ? 'up' : 'plain') + '|' + el.getAttribute('data') + '|' + el.id;"
        "document.body.appendChild(document.importNode(t.content, true));"
        "const stamped = document.querySelector('x-foo');"
        "before + '|' + (stamped.upgraded === true ? 'up' : 'plain') + '|' + "
        "(stamped.getAttribute('data') === null ? 'stripped' : stamped.getAttribute('data'))",
        "plain|[[host.data]]|primary|up|stripped");
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

    // A batch is one record carrying every node, whichever way the batch
    // arrived. All three of these produced *no* record before session 14:
    // appending a fragment, writing `innerHTML`, and clearing children --
    // which meant a framework that assembled its subtree in a fragment, which
    // is the only reason to use one, was invisible to every observer.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { childList: true });"
                 "var f = document.createDocumentFragment();"
                 "f.appendChild(document.createElement('i'));"
                 "f.appendChild(document.createElement('b'));"
                 "document.body.appendChild(f);"
                 "var r = o.takeRecords(); r.length + ':' + r[0].addedNodes.length",
                 "1:2");
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.getElementById('list'), { childList: true });"
                 "document.getElementById('list').innerHTML = '<p>a</p><p>b</p>';"
                 "var r = o.takeRecords();"
                 "r.length + ':' + r[0].removedNodes.length + ':' + r[1].addedNodes.length",
                 "2:2:2");

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

    // characterData: Polymer's ASAP (`_.Ub` / youtube lazy-list autofill) bumps
    // a detached text node's textContent under a characterData observer.
    ExpectScript(kPage,
                 "var t = document.createTextNode('');"
                 "var o = new MutationObserver(() => {});"
                 "o.observe(t, { characterData: true });"
                 "t.textContent = '1';"
                 "var r = o.takeRecords(); r.length + ':' + r[0].type",
                 "1:characterData");
    ExpectScript(kPage,
                 "var t = document.createTextNode('a');"
                 "var o = new MutationObserver(() => {});"
                 "o.observe(t, { characterData: true });"
                 "t.data = 'b';"
                 "o.takeRecords()[0].type",
                 "characterData");

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

    // EventTarget is the root, and `window` is one -- which is where the
    // specification puts `addEventListener` and where a polyfill patches it.
    // youtube's webcomponents bundle branches on `window.EventTarget`, and the
    // else branch is written for browsers from before the name existed.
    ExpectScript(kPage, "document.body instanceof EventTarget", "true");
    ExpectScript(kPage, "window instanceof EventTarget", "true");
    ExpectScript(kPage, "window instanceof Window", "true");
    ExpectScript(kPage, "typeof EventTarget.prototype.addEventListener", "function");
    ExpectScript(kPage, "Node.prototype.hasOwnProperty('addEventListener')", "false");
    // The chain, and it ends where every chain does.
    ExpectScript(kPage, "Object.getPrototypeOf(Node.prototype) === EventTarget.prototype", "true");
    ExpectScript(kPage, "Object.getPrototypeOf(window) === Window.prototype", "true");

    // SVGElement forks off Element rather than HTMLElement, which is the one
    // place this table's chain branches. Its own geometry API is absent because
    // nothing produces an element that would need it: `src/html` has no foreign
    // content, so an SVG subtree's inner tags are not distinguished either.
    ExpectScript(kPage, "document.createElement('svg') instanceof SVGElement", "true");
    ExpectScript(kPage, "document.createElement('svg') instanceof Element", "true");
    ExpectScript(kPage, "document.createElement('svg') instanceof HTMLElement", "false");

    // A shadow root is a DocumentFragment with its own name, and the two are
    // told apart by whether the fragment has a host.
    ExpectScript(kPage,
                 "document.createElement('div').attachShadow({mode: 'open'}) instanceof ShadowRoot",
                 "true");
    ExpectScript(kPage, "document.createDocumentFragment() instanceof ShadowRoot", "false");
    ExpectScript(kPage,
                 "Object.getPrototypeOf(ShadowRoot.prototype) === DocumentFragment.prototype",
                 "true");

    // And the names a polyfill reaches for without ever making one: the type a
    // failed custom-element upgrade is reparented onto, and the registry's own
    // interface, which is what `Object.defineProperty(CustomElementRegistry
    // .prototype, 'define', ...)` needs to exist.
    ExpectScript(kPage, "typeof HTMLUnknownElement.prototype", "object");
    ExpectScript(kPage, "customElements instanceof CustomElementRegistry", "true");
    ExpectScript(kPage, "typeof CustomElementRegistry.prototype.define", "function");

    // `new Image()` is the img element's constructor and nothing more, which
    // is what makes it honest to have: the loading a detached image does not
    // do is the synchronous-loading gap, which an `<img>` added by script has
    // equally.
    ExpectScript(kPage, "new Image() instanceof HTMLImageElement", "true");
    ExpectScript(kPage, "new Image().tagName", "IMG");
    ExpectScript(kPage, "new Image(4, 5).getAttribute('height')", "5");

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
    ExpectScript(kPage, "document.getElementById('title').tagName", "H1");
    ExpectScript(kPage, "document.getElementById('title').textContent", "Hello");
    ExpectScript(kPage, "document.getElementById('nope') === null", "true");
    ExpectScript(kPage, "document.getElementsByTagName('p').length", "2");
    ExpectScript(kPage, "document.getElementsByTagName('p')[1].textContent", "two");
    ExpectScript(kPage, "document.body.tagName", "BODY");
    ExpectScript(kPage, "document.documentElement.tagName", "HTML");
  });

  AddTest(tests, "DomBindings/QuerySelectorHandlesTheThreeSimpleForms", [] {
    ExpectScript(kPage, "document.querySelector('p').textContent", "one");
    ExpectScript(kPage, "document.querySelector('#list').tagName", "DIV");
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

  AddTest(tests, "DomBindings/ReflectedPropertiesWriteTheAttribute", [] {
    // The half that was missing. `id` and `className` were getter-only, so
    // assigning to either succeeded, read back, and changed nothing about the
    // element -- which means the cascade never saw the class.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.id = 'moved';"
                 "t.getAttribute('id') + '/' + document.getElementById('moved').tagName",
                 "moved/H1");
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); t.className = 'a b';"
                 "t.getAttribute('class')",
                 "a b");
    // What the reddit challenge actually writes: three reflected attributes in
    // one `Object.assign` and no setAttribute anywhere.
    ExpectScript(kPage,
                 "const i = Object.assign(document.createElement('input'),"
                 "  {name: 'solution', type: 'hidden', value: 'abc'});"
                 "[i.getAttribute('name'), i.getAttribute('type'), i.getAttribute('value')]"
                 ".join(',')",
                 "solution,hidden,abc");
    // Presence, not text: assigning false removes the attribute, because
    // writing "false" into it would leave the control disabled.
    ExpectScript(kPage,
                 "const i = document.createElement('input'); i.disabled = true;"
                 "const on = i.hasAttribute('disabled'); i.disabled = false;"
                 "on + '/' + i.hasAttribute('disabled') + '/' + i.disabled",
                 "true/false/false");
    // HTMLElement.hidden is the same presence reflection. Polymer stamps
    // `hidden="[[!isExpanded]]"` and writes the boolean property; an expando
    // would leave the attribute unset and the UA `[hidden]` rule never match.
    ExpectScript(kPage,
                 "const d = document.createElement('div'); d.hidden = true;"
                 "const on = d.hasAttribute('hidden'); d.hidden = false;"
                 "on + '/' + d.hasAttribute('hidden') + '/' + d.hidden",
                 "true/false/false");
    // Presence + ToBoolean: `hidden = undefined` must clear. Without a real
    // `document.all`, polymer_resin replaces undefined with `"zClosurez"` and
    // that string is truthy — which stuck `hidden=""` on youtube search.
    ExpectScript(kPage,
                 "const d = document.createElement('div'); d.hidden = true;"
                 "d.hidden = undefined;"
                 "d.hasAttribute('hidden') + '/' + d.hidden",
                 "false/false");
    // A missing `type` is a text input, which is what a page branching on it
    // expects to read.
    ExpectScript(kPage, "document.createElement('input').type", "text");
    // A textarea's value is its text until something sets one, which is what
    // the engine's own form data set reads.
    ExpectScript(kPage,
                 "const t = document.createElement('textarea'); t.appendText('typed'); t.value",
                 "typed");
    ExpectScript(kPage,
                 "const t = document.createElement('textarea'); t.value = 'set'; t.value",
                 "set");
    // Reflection is an attribute write like any other, so an observer sees it
    // and a custom element's attributeChangedCallback runs. That is the whole
    // reason both paths go through one helper.
    ExpectScript(kPage,
                 "var o = new MutationObserver(() => {});"
                 "o.observe(document.body, { attributes: true, attributeOldValue: true });"
                 "document.body.id = 'first'; document.body.id = 'second';"
                 "var r = o.takeRecords(); r.length + ':' + r[1].attributeName + ':' + r[1].oldValue",
                 "2:id:first");
  });

  AddTest(tests, "DomBindings/DocumentAllIsHTMLDDA", [] {
    // HTML [[IsHTMLDDA]]: falsy, typeof "undefined", == null, but still an
    // object so `!== undefined` is true. Polymer resin's
    // `!Z && Z !== document.all` relies on that last clause — without a real
    // `document.all`, `undefined !== document.all` is false and undefined sinks
    // become the string `"zClosurez"`.
    ExpectScript(kPage,
                 "[typeof document.all, !document.all, document.all == null, "
                 "document.all == undefined, document.all !== undefined, "
                 "undefined !== document.all, "
                 "!(!undefined && undefined !== document.all)].join(',')",
                 "undefined,true,true,true,true,true,false");
    ExpectScript(kPage,
                 "Object.prototype.toString.call(document.all)",
                 "[object HTMLAllCollection]");
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
    ExpectScript(kPage, "document.getElementById('list').children[0].tagName", "P");
    ExpectScript("<div id=d>text<span></span></div>",
                 "document.getElementById('d').childNodes.length", "2");
    ExpectScript("<div id=d>text<span></span></div>",
                 "document.getElementById('d').children.length", "1");
    ExpectScript("<div id=d>text</div>",
                 "document.getElementById('d').childNodes[0].nodeType", "3");
    ExpectScript(kPage, "document.getElementById('title').nodeType", "1");
    // ParentNode mixin surface: own on Element/Document, absent on Node/Text.
    // ShadyDOM's noPatch capture requires the Element.prototype own descriptor.
    ExpectScript(kPage,
                 "Object.prototype.hasOwnProperty.call(Element.prototype, 'children') && "
                 "!Object.prototype.hasOwnProperty.call(Node.prototype, 'children') && "
                 "Object.prototype.hasOwnProperty.call(Document.prototype, 'children') && "
                 "document.createTextNode('x').children === undefined",
                 "true");
  });

  AddTest(tests, "DomBindings/ScriptCanBuildAndAttachNodes", [] {
    ExpectScript(kPage,
                 "const el = document.createElement('section'); el.appendText('made'); "
                 "document.body.appendChild(el); "
                 "document.getElementsByTagName('section')[0].textContent",
                 "made");
    // A created node is owned by the bindings until it is attached, so
    // creating one and dropping it leaks nothing and dangles nothing.
    ExpectScript(kPage, "document.createElement('div').tagName", "DIV");
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
    ExpectScript(kPage, "document.querySelector('p').closest('#list').tagName", "DIV");
    ExpectScript(kPage, "document.querySelector('p').closest('p').tagName", "P");
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
    // `nodeName` and `tagName` are the *same string* for an element, which is
    // what the DOM says and what every browser does. They used to differ here
    // -- `nodeName` upper-cased and `tagName` handed back the parser's stored
    // lower-case name -- and a comment above this assertion called that
    // deliberate. It was a bug: `el.tagName === 'SCRIPT'` is written
    // everywhere and was silently false.
    ExpectScript(kPage,
                 "document.getElementById('title').nodeName + ' ' + "
                 "document.getElementById('title').tagName",
                 "H1 H1");
    // The names for everything that is not an element, which had one answer
    // (`#document`) for four different kinds.
    ExpectScript("<div id=d>t<!--c--></div>",
                 "const d = document.getElementById('d');"
                 "d.firstChild.nodeName + ' ' + d.lastChild.nodeName + ' ' + "
                 "document.nodeName + ' ' + document.createDocumentFragment().nodeName",
                 "#text #comment #document #document-fragment");
    // And a node that is not an element has no `tagName` at all, rather than
    // the node name under a second spelling.
    ExpectScript("<div id=d>t</div>",
                 "typeof document.getElementById('d').firstChild.tagName", "undefined");
  });

  AddTest(tests, "DomBindings/ATreeWalkerWalksAndAFilterDecides", [] {
    // youtube.com's `webcomponents-all-noPatch.js` opens with
    // `document.createTreeWalker(document, NodeFilter.SHOW_ALL, null, !1)` at
    // module scope, so an absent NodeFilter ended the polyfill on its first
    // line -- and with it every custom element the page is built out of.
    static constexpr const char* kTree =
        "<div id=r>one<span id=s>two</span><!--c--><p id=p>three</p></div>";
    // `whatToShow` is applied before the filter, and the mask is
    // `1 << (nodeType - 1)` -- which is why SHOW_TEXT is 4 and not 2.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, null, false);"
                 "let out = [], n; while ((n = w.nextNode())) out.push(n.id); out.join(',')",
                 "s,p");
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ALL, null, false);"
                 "let out = [], n; while ((n = w.nextNode())) out.push(n.nodeType);"
                 "out.join(',')",
                 "3,1,3,8,1,3");
    // Absent `whatToShow` is SHOW_ALL, which is what `createTreeWalker(root)`
    // relies on.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'));"
                 "let n = 0; while (w.nextNode()) n++; n",
                 "6");
    // **Reject takes the subtree and skip does not.** The one thing about
    // this API that is easy to get backwards, so both are asserted: `span`
    // rejected loses its text child, `span` skipped keeps it.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ALL, function (node) {"
                 "    return node.nodeName === 'SPAN' ? NodeFilter.FILTER_REJECT"
                 "                                    : NodeFilter.FILTER_ACCEPT });"
                 "let n = 0; while (w.nextNode()) n++; n",
                 "4");
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ALL, function (node) {"
                 "    return node.nodeName === 'SPAN' ? NodeFilter.FILTER_SKIP"
                 "                                    : NodeFilter.FILTER_ACCEPT });"
                 "let n = 0; while (w.nextNode()) n++; n",
                 "5");
    // A filter object with `acceptNode`, which is what a polyfill written
    // against the original interface passes.
    ExpectScript(kTree,
                 "let seen = 0;"
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, { acceptNode: function () { seen++; return 1 } });"
                 "while (w.nextNode()) {} seen",
                 "2");
    // The five relative moves, and that each leaves `currentNode` alone when
    // it finds nothing -- which is what a `while (w.nextSibling())` loop is
    // written against.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, null, false);"
                 "w.firstChild().id + ' ' + w.nextSibling().id + ' ' +"
                 "(w.nextSibling() === null) + ' ' + w.currentNode.id + ' ' + w.parentNode().id",
                 "s p true p r");
    // `currentNode` is writable, which is how a page positions the walk.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, null, false);"
                 "w.currentNode = document.getElementById('p');"
                 "(w.previousNode() || {}).id + ' ' + (w.nextNode() || {}).id + ' ' +"
                 "(w.nextNode() === null)",
                 "s p true");
    // A NodeIterator is the flat sequence, and its first `nextNode` answers
    // with the root itself.
    ExpectScript(kTree,
                 "const it = document.createNodeIterator(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, null);"
                 "let out = [], n; while ((n = it.nextNode())) out.push(n.id); out.join(',')",
                 "r,s,p");
    // A throw out of a filter stops the walk and propagates. Not swallowed
    // into a reject: a filter that threw has not answered, and continuing
    // would be inventing one.
    ExpectScript(kTree,
                 "const w = document.createTreeWalker(document.getElementById('r'),"
                 "  NodeFilter.SHOW_ELEMENT, function () { throw new Error('no') });"
                 "try { w.nextNode(); 'no throw' } catch (e) { e.message }",
                 "no");
    // The constants a page reads far more often than it implements the
    // interface.
    ExpectScript(kTree,
                 "NodeFilter.SHOW_ALL + ' ' + NodeFilter.SHOW_ELEMENT + ' ' +"
                 "NodeFilter.SHOW_TEXT + ' ' + NodeFilter.SHOW_COMMENT + ' ' +"
                 "(NodeFilter.FILTER_ACCEPT + NodeFilter.FILTER_REJECT + NodeFilter.FILTER_SKIP)",
                 "4294967295 1 4 128 6");
  });

  AddTest(tests, "DomBindings/AMessageChannelIsATaskAndACopy", [] {
    // youtube's kevlar bundle does `TJg((new MessageChannel).port2)` and stops
    // dead without this. The two properties below are what that use is for and
    // what a wrong implementation would silently break.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);

    bound.interpreter->Run(
        "globalThis.order = [];"
        "globalThis.seen = [];"
        "const c = new MessageChannel();"
        "c.port1.onmessage = e => { order.push('msg'); seen.push(e.data.n) };"
        "const o = { n: 7 };"
        "c.port2.postMessage(o);"
        "o.n = 99;"
        "Promise.resolve().then(() => order.push('microtask'));"
        "order.push('sync');");
    bound.interpreter->DrainMicrotasks();
    // **Nothing has been delivered yet, and that is the feature.** A message
    // through a port is a task: it runs after the turn that posted it, which
    // is why a page uses a channel to yield to the event loop at all. A
    // microtask here would make a scheduler starve the work it was written to
    // let through.
    ExpectEqString(js::ToString(bound.interpreter->Run("order.join(',')").value),
                   "sync,microtask", "the microtask ran and the message did not");
    Expect(timers.RunDue(*bound.interpreter, 0), "the message is a due task");
    ExpectEqString(js::ToString(bound.interpreter->Run("order.join(',')").value),
                   "sync,microtask,msg", "and it arrives after both");
    // Structured-cloned, so mutating the object after the post does not change
    // what arrives -- both ends are in this heap, and that is exactly why the
    // copy has to be real.
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + seen[0]").value), "7",
                   "the message is a copy taken at the post");
    // And the event a port delivers is a real MessageEvent, which is what a
    // page checks before trusting `.data`.
    bound.interpreter->Run(
        "globalThis.kind = '';"
        "const q = new MessageChannel();"
        "q.port1.onmessage = e => { kind = '' + (e instanceof MessageEvent) };"
        "q.port2.postMessage(1);");
    timers.RunDue(*bound.interpreter, 0);
    ExpectEqString(js::ToString(bound.interpreter->Run("kind").value), "true",
                   "the delivered event is a MessageEvent");

    // A post before the far end is listening is queued, not dropped: a page
    // routinely hands a port somewhere that posts to it immediately.
    bound.interpreter->Run(
        "globalThis.late = '';"
        "const m = new MessageChannel();"
        "m.port1.postMessage('early');"
        "m.port2.onmessage = e => { late += e.data };");
    Expect(timers.RunDue(*bound.interpreter, 1), "the queued message runs once started");
    ExpectEqString(js::ToString(bound.interpreter->Run("late").value), "early",
                   "and it is the one posted before the handler existed");

    // A closed port stops delivering rather than throwing: closing is how a
    // page tears a channel down and the last post racing it is normal.
    bound.interpreter->Run(
        "globalThis.after = 0;"
        "const k = new MessageChannel();"
        "k.port1.onmessage = () => { after++ };"
        "k.port1.close();"
        "k.port2.postMessage(1);");
    timers.RunDue(*bound.interpreter, 2);
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + after").value), "0",
                   "a closed port delivers nothing");

    // The transfer list is refused rather than silently copied. A transfer
    // detaches what it names; copying instead leaves a page holding two live
    // views on what it believes is one.
    ExpectEqString(
        js::ToString(bound.interpreter
                         ->Run("const t = new MessageChannel();"
                               "try { t.port1.postMessage(1, [t.port2]); 'no throw' }"
                               "catch (e) { e.message }")
                         .value),
        "DataCloneError: transferring objects is not supported", "a transfer is refused");

    // The interface is real, which is what a polyfill checks before it
    // decides whether to build its own.
    ExpectEqString(js::ToString(bound.interpreter
                                    ->Run("const p = new MessageChannel().port1;"
                                          "typeof MessagePort + ' ' + (p instanceof MessagePort) +"
                                          "' ' + (typeof p.addEventListener)")
                                    .value),
                   "function true function", "MessagePort is an EventTarget with a name");
  });

  AddTest(tests, "DomBindings/AMessageChannelTaskClearsASpentStepBudget", [] {
    // TD-0018: after a host turn burns `kMaxSteps`, MessageChannel delivery
    // must BeginTask or kevlar's scheduler continuations abort mid-stamp.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter->Run("globalThis.got = 0;"
                                  "const c = new MessageChannel();"
                                  "c.port1.onmessage = () => { got = 42 };"
                                  "c.port2.postMessage(0);"
                                  "while (true) {}")
               .completion == js::Completion::Throw,
           "burn the hang guard without absorbing it");
    Expect(timers.RunDue(*bound.interpreter, 0), "deliver the queued task");
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + got").value), "42",
                   "MessageChannel task ran after a spent budget");
  });

  AddTest(tests, "DomBindings/MessageChannelHostTasksBatchInOneRunDue", [] {
    // TD-0018: a cooperative scheduler posts the next slice from inside the
    // previous one. Without draining those in the same RunDue, each slice
    // forces its own LayoutAndPaint.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter
               ->Run("globalThis.n = 0;"
                     "const c = new MessageChannel();"
                     "c.port1.onmessage = () => {"
                     "  if (++n < 20) c.port2.postMessage(0);"
                     "};"
                     "c.port2.postMessage(0);")
               .completion == js::Completion::Normal,
           "queue the first host task");
    Expect(timers.RunDue(*bound.interpreter, 0), "one RunDue drains the chain");
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "20",
                   "all twenty host tasks ran in one turn");
    Expect(!timers.RunDue(*bound.interpreter, 0), "nothing left for a second turn");
  });

  AddTest(tests, "DomBindings/SetTimeoutZeroDoesNotBatchInsideRunDue", [] {
    // The host-task drain must not apply to setTimeout(0): that is the rule
    // that stops a busy page owning the turn forever. Nested clamp then
    // forces 4ms so the loop sleeps rather than spins.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter
               ->Run("globalThis.n = 0;"
                     "function tick() { if (++n < 5) setTimeout(tick, 0); }"
                     "setTimeout(tick, 0);")
               .completion == js::Completion::Normal,
           "arm the zero-delay chain");
    Expect(timers.RunDue(*bound.interpreter, 0), "first timeout runs");
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "1",
                   "only the timers due at the start of the pass");
  });

  AddTest(tests, "DomBindings/NestedSetTimeoutClampsToFourMs", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter
               ->Run("globalThis.n = 0;"
                     "function tick() {"
                     "  if (++n > 10) return;"
                     "  setTimeout(tick, 0);"
                     "}"
                     "setTimeout(tick, 0);")
               .completion == js::Completion::Normal,
           "arm a nested zero-delay chain");
    // Walk nesting with synthetic now. Clamp applies when scheduling from a
    // task whose nesting_level is already > 5.
    std::int64_t now = 0;
    for (int i = 0; i < 8; ++i) {
      Expect(timers.RunDue(*bound.interpreter, now), "timer fires");
      const auto delay = timers.NextDelay(now);
      Expect(delay.has_value(), "another timeout was scheduled");
      if (i < 5) {
        ExpectEqInt(static_cast<long long>(*delay), 0, "sub-clamp nesting stays immediate");
      } else {
        Expect(*delay >= 4, "past five nestings the delay is ≥4ms");
        now += static_cast<std::int64_t>(*delay);
      }
    }
  });

  AddTest(tests, "DomBindings/DOMExceptionExistsForInstanceofAndName", [] {
    // youtube's player catches with `err instanceof DOMException` then reads
    // `.name`. A missing binding turns that into a ReferenceError that aborts
    // the media path before any Error fallback runs.
    ExpectScript("<body></body>", "typeof DOMException", "function");
    ExpectScript("<body></body>",
                 "(() => { const e = new DOMException('full', 'QuotaExceededError');"
                 " return (e instanceof DOMException) + '|' + (e instanceof Error) + '|' + e.name;"
                 " })()",
                 "true|true|QuotaExceededError");
    ExpectScript("<body></body>",
                 "(() => { try { null.x } catch (e) {"
                 "   return (e instanceof DOMException) + '|' + (e instanceof Error);"
                 " } })()",
                 "false|true");
  });

  AddTest(tests, "DomBindings/SetTimeoutClearsASpentStepBudget", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter->Run("globalThis.got = 0;"
                                  "setTimeout(() => { got = 7 }, 0);"
                                  "while (true) {}")
               .completion == js::Completion::Throw,
           "burn the hang guard");
    Expect(timers.RunDue(*bound.interpreter, 0), "deliver setTimeout");
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + got").value), "7",
                   "setTimeout ran after a spent budget");
  });

  AddTest(tests, "DomBindings/BlobUrlAndWindowPostMessage", [] {
  struct StubNetwork final : bindings::NetworkSource {
    std::uint64_t StartFetch(const bindings::ScriptRequest&) override { return 0; }
    void AbortFetch(std::uint64_t) override {}
    std::string ResolveUrl(std::string_view, std::string_view) const override { return {}; }
    std::string RegisterBlobUrl(std::string body, std::string) override {
      registered = std::move(body);
      return "blob:null/42";
    }
    void RevokeBlobUrl(const std::string&) override {}
    std::string registered;
  } network;

  auto document = html::ParseDocument("<html><body></body></html>");
  auto interpreter = std::make_unique<js::Interpreter>();
  bindings::DomBindings dom(*interpreter, *document, "https://example.org/", nullptr, &network,
                            nullptr, nullptr, nullptr, nullptr, nullptr);
  dom.Install();

  const js::Result blob = interpreter->Run(
      "const b = new Blob(['self._d=u=>import(u)'], {type: 'text/javascript'});"
      "typeof Blob + '|' + URL.createObjectURL(b)");
  ExpectEqString(js::ToString(blob.value), "function|blob:null/42", "Blob registers a url");
  ExpectEqString(network.registered, "self._d=u=>import(u)", "blob body preserved");

  const js::Result message = interpreter->Run(
      "let data = ''; window.addEventListener('message', e => { data = e.data[0]; });"
      "window.postMessage(['esms', true], '*'); data");
  ExpectEqString(js::ToString(message.value), "esms", "postMessage delivers to window");
  });

  AddTest(tests, "DomBindings/IframeInsertCompletesEsmsFeatureDetection", [] {
    auto document = html::ParseDocument("<html><head></head><body></body></html>");
    auto interpreter = std::make_unique<js::Interpreter>();
    bindings::DomBindings dom(*interpreter, *document, "https://example.org/", nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr);
    dom.Install();

    const js::Result outcome = interpreter->Run(
        "let flags = null;"
        "window.addEventListener('message', e => { flags = e.data; });"
        "document.head.appendChild(document.createElement('iframe'));"
        "flags ? flags[0] + ':' + flags[1] + ':' + flags[2] : 'none'");
    ExpectEqString(js::ToString(outcome.value), "esms:false:true",
                 "iframe insert delivers synthetic esms feature tuple");
  });

  AddTest(tests, "DomBindings/ARangeIsTwoBoundaryPointsAndTheirOrder", [] {
    // Closure's `goog.dom.Range` is how youtube's bundle asks "is this node
    // before that one", and it stopped on the name. Everything asserted here
    // falls out of one ordering function over (node, offset) pairs -- which is
    // the point of it being one function.
    static constexpr const char* kTree =
        "<div id=r>one<span id=s>two</span>three</div>";
    ExpectScript(kTree,
                 "const r = document.getElementById('r');"
                 "const a = document.createRange(); a.selectNodeContents(r);"
                 "a.toString() + '|' + a.collapsed + '|' + a.commonAncestorContainer.id",
                 "onetwothree|false|r");
    // `selectNode` puts the boundaries either side; `selectNodeContents` puts
    // them inside. One character of difference in the name, and the whole
    // difference in what is covered.
    ExpectScript(kTree,
                 "const b = document.createRange();"
                 "b.selectNode(document.getElementById('s'));"
                 "b.toString() + '|' + b.startOffset + '|' + b.endOffset + '|' +"
                 "b.startContainer.id",
                 "two|1|2|r");
    // A range that starts and ends inside two *different* text nodes, which is
    // the case a naive implementation gets wrong.
    ExpectScript(kTree,
                 "const r = document.getElementById('r');"
                 "const c = document.createRange();"
                 "c.setStart(r.firstChild, 1); c.setEnd(r.lastChild, 2);"
                 "c.toString()",
                 "netwoth");
    // compareBoundaryPoints, and the four constants a page reads off the
    // constructor as often as off an instance.
    ExpectScript(kTree,
                 "const r = document.getElementById('r');"
                 "const a = document.createRange(); a.selectNodeContents(r);"
                 "const b = document.createRange(); b.selectNode(document.getElementById('s'));"
                 "a.compareBoundaryPoints(Range.START_TO_START, b) + ',' +"
                 "b.compareBoundaryPoints(Range.END_TO_END, a) + ',' +"
                 "a.compareBoundaryPoints(Range.START_TO_START, a) + ',' +"
                 "[Range.START_TO_START, Range.START_TO_END, Range.END_TO_END,"
                 " Range.END_TO_START].join('')",
                 "-1,-1,0,0123");
    // Setting a start past the end collapses onto it, so `collapsed` is
    // honest without anything checking the order at read time.
    ExpectScript(kTree,
                 "const r = document.getElementById('r');"
                 "const e = document.createRange();"
                 "e.setEnd(r.firstChild, 1); e.setStart(r.lastChild, 2);"
                 "'' + e.collapsed",
                 "true");
    // `collapse()` defaults to the *end*, which is the one people get wrong.
    ExpectScript(kTree,
                 "const r = document.getElementById('r');"
                 "const a = document.createRange(); a.selectNodeContents(r);"
                 "const toEnd = a.cloneRange(); toEnd.collapse();"
                 "const toStart = a.cloneRange(); toStart.collapse(true);"
                 "toEnd.startOffset + ',' + toStart.startOffset + ',' + a.startOffset",
                 "3,0,0");
    // Two ranges in different trees are a WrongDocumentError rather than an
    // arbitrary answer -- the one case the ordering function cannot decide.
    ExpectScript(kTree,
                 "const a = document.createRange();"
                 "a.selectNodeContents(document.getElementById('r'));"
                 "const other = document.createRange();"
                 "other.selectNodeContents(document.createElement('div'));"
                 "try { a.compareBoundaryPoints(Range.START_TO_START, other); 'no throw' }"
                 "catch (err) { err.message }",
                 "WrongDocumentError: the ranges are in different trees");
    ExpectScript(kTree,
                 "const a = document.createRange();"
                 "(a instanceof Range) + ',' + (new Range() instanceof Range) + ',' +"
                 "(typeof document.createRange)",
                 "true,true,function");
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
    // Iterable + length: youtube's path builder does `_.A(el.classList)`.
    ExpectScript(kPage,
                 "const t = document.getElementById('title'); "
                 "[...t.classList].join(',') + '|' + t.classList.length + '|' +"
                 " t.classList.item(1)",
                 "big,head|2|head");
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
    ExpectScript(kPage, "location.hostname", "example.org");
    ExpectScript(kPage, "location.port", "");
    // The query used to be part of the path, which is not a cosmetic error: a
    // page that reads `location.search` off this got `undefined`, built an
    // empty URLSearchParams from it and carried on.
    ExpectScript(kPage, "location.pathname", "/a/b");
    ExpectScript(kPage, "location.search", "?q=1");
    ExpectScript(kPage, "location.hash", "");
    ExpectScript(kPage, "location.origin", "https://example.org");
    ExpectScript(kPage, "'' + location", "https://example.org/a/b?q=1");
    // The same object under both names, which a page checks by identity.
    ExpectScript(kPage, "document.location === location", "true");
    // The user agent is a fingerprinting surface before it is anything else.
    // This one says what the browser is and nothing about the machine it is
    // on, so every copy answers the same.
    ExpectScript(kPage, "navigator.userAgent", "microbrowser");
  });

  AddTest(tests, "DomBindings/PageVisibilityAndUserActivation", [] {
    ExpectScript(kPage, "document.hidden", "false");
    ExpectScript(kPage, "document.visibilityState", "visible");
    ExpectScript(kPage, "document.hasFocus()", "true");
    ExpectScript(kPage, "typeof navigator.userActivation", "object");
    ExpectScript(kPage, "navigator.userActivation.isActive", "false");
    ExpectScript(kPage, "navigator.userActivation.hasBeenActive", "false");
  });

  AddTest(tests, "DomBindings/LocationSplitsTheShapesAUrlComesIn", [] {
    const auto at = [](std::string url, std::string_view source) {
      Bound bound = Bind(kPage, std::move(url));
      return js::ToString(bound.interpreter->Run(source).value);
    };
    // A port belongs to `host` and to `port`, and to neither `hostname` nor
    // `origin`'s host half by itself.
    ExpectEqString(at("http://example.org:8080/x", "location.host"), "example.org:8080", "host");
    ExpectEqString(at("http://example.org:8080/x", "location.hostname"), "example.org", "hostname");
    ExpectEqString(at("http://example.org:8080/x", "location.port"), "8080", "port");
    ExpectEqString(at("http://example.org:8080/x", "location.origin"),
                   "http://example.org:8080", "origin carries the port");
    // An IPv6 host is full of colons, so the port separator is the one after
    // the closing bracket rather than the last colon in the string.
    ExpectEqString(at("http://[::1]:9/x", "location.hostname"), "[::1]", "IPv6 hostname");
    ExpectEqString(at("http://[::1]:9/x", "location.port"), "9", "IPv6 port");
    ExpectEqString(at("http://[::1]/x", "location.port"), "", "and no port when there is none");
    // Credentials are part of the authority and part of no location property.
    ExpectEqString(at("http://u:p@example.org/x", "location.host"), "example.org",
                   "credentials are not the host");
    // Fragment and query, together and apart.
    ExpectEqString(at("https://example.org/p?a=1#top", "location.hash"), "#top", "hash");
    ExpectEqString(at("https://example.org/p?a=1#top", "location.search"), "?a=1",
                   "the fragment is not part of the query");
    ExpectEqString(at("https://example.org/p#top", "location.search"), "", "no query");
    // A URL with no authority has an opaque path and a null origin -- which is
    // the answer the eventual pushState check must never take from here.
    ExpectEqString(at("about:blank", "location.origin"), "null", "opaque origin");
    ExpectEqString(at("about:blank", "location.pathname"), "blank", "opaque path");
  });

  AddTest(tests, "DomBindings/FormsAreReachableAndOwnTheirControls", [] {
    // Ownership rather than containment: `form="login"` puts a control in a
    // form it is nowhere near, and the engine's form data set already knows
    // that. Two answers to "which controls does this form own" is the bug this
    // is written against.
    constexpr const char* kForms =
        "<body><form id=login name=login action='/in'>"
        "<input name=user value=u><input name=pass value=p>"
        "<img src=x.png></form>"
        "<input name=extra form=login value=e>"
        "<form id=other></form></body>";
    ExpectScript(kForms, "document.forms.length", "2");
    ExpectScript(kForms, "document.forms[0].tagName", "FORM");
    ExpectScript(kForms, "document.forms.namedItem('login').id", "login");
    ExpectScript(kForms, "document.forms.namedItem('nothing') === null", "true");
    ExpectScript(kForms, "document.forms[0].elements.length", "3");
    ExpectScript(kForms, "document.forms[0].elements.namedItem('extra').getAttribute('value')",
                 "e");
    ExpectScript(kForms, "document.forms[0].elements.namedItem('user').tagName", "INPUT");
    // An `<img>` inside a form is not one of its elements.
    ExpectScript(kForms,
                 "[...document.forms[0].elements].map(e => e.tagName).join(',')",
                 "INPUT,INPUT,INPUT");
    ExpectScript(kForms, "document.forms[0].elements.namedItem('user').form.id", "login");
    ExpectScript(kForms, "document.body.form === null", "true");
  });

  AddTest(tests, "DomBindings/DocumentCollectionsCoverScriptsImagesAndLinks", [] {
    constexpr const char* kPage =
        "<body><script src=a.js></script><script>1</script>"
        "<img id=i src=x.png><a href=/go>go</a><a name=n>no</a></body>";
    ExpectScript(kPage, "document.scripts.length", "2");
    ExpectScript(kPage, "document.images.length", "1");
    ExpectScript(kPage, "document.images[0].id", "i");
    ExpectScript(kPage, "document.links.length", "1");
    ExpectScript(kPage, "document.links[0].getAttribute('href')", "/go");
  });

  AddTest(tests, "DomBindings/QuerySelectorUsesTheCssSelectorEngine", [] {
    // The three-form toy (`#id` / `.class` / exact tag) made selector lists and
    // combinators silently match nothing -- which is how youtube.com's
    // `querySelectorAll("ytd-app,ytd-masthead")` returned 0 while
    // `querySelector("ytd-app")` still found the app.
    constexpr const char* kPage =
        "<body><div id=a><span class=x>1</span></div>"
        "<div id=b><span class=y>2</span></div>"
        "<ytd-app></ytd-app><ytd-masthead></ytd-masthead></body>";
    ExpectScript(kPage, "document.querySelectorAll('ytd-app,ytd-masthead').length", "2");
    ExpectScript(kPage, "document.querySelectorAll('div > span').length", "2");
    ExpectScript(kPage, "document.querySelectorAll('div span.x').length", "1");
    ExpectScript(kPage, "document.querySelector('#a > span.x').textContent", "1");
    ExpectScript(kPage, "document.querySelector('span.x').matches('div > span')", "true");
  });

  AddTest(tests, "DomBindings/RequestSubmitIsNotAnAliasForSubmit", [] {
    // The distinction with a page-shaped consequence: `requestSubmit()` fires
    // `submit` and `submit()` does not. A browser that aliases them submits the
    // form without the fields the page's own handler was going to add, and
    // reports nothing anywhere. See ADR 0026 §4.
    constexpr const char* kForm = "<body><form id=f action='/go'><input name=a value=1>"
                                  "<input type=submit id=go></form></body>";
    ExpectScript(kForm,
                 "let fired = 0; const f = document.getElementById('f');"
                 "f.addEventListener('submit', () => fired++); f.submit(); fired",
                 "0");
    ExpectScript(kForm,
                 "let fired = 0; const f = document.getElementById('f');"
                 "f.addEventListener('submit', () => fired++); f.requestSubmit(); fired",
                 "1");
    // The handler as a property, which is how reddit's interstitial writes it.
    ExpectScript(kForm,
                 "let seen = ''; const f = document.getElementById('f');"
                 "f.onsubmit = e => { seen = e.type + ':' + e.target.id + ':' + e.isTrusted };"
                 "f.requestSubmit(); seen",
                 "submit:f:true");
    // The submitter has to be a submit button belonging to this form, and
    // neither check is a warning.
    ExpectScript(kForm,
                 "document.getElementById('f').requestSubmit(document.body)",
                 "throw TypeError: the submitter must be a submit button");
    ExpectScript(kForm,
                 "const b = document.createElement('input'); b.type = 'submit';"
                 "document.getElementById('f').requestSubmit(b)",
                 "throw NotFoundError: the submitter does not belong to this form");
  });

  AddTest(tests, "DomBindings/AListenerCanAskToRunOnce", [] {
    ExpectScript(kPage,
                 "let n = 0; const b = document.body;"
                 "b.addEventListener('x', () => n++, {once: true});"
                 "b.dispatchEvent(new Event('x')); b.dispatchEvent(new Event('x')); n",
                 "1");
    ExpectScript(kPage,
                 "let n = 0; const b = document.body;"
                 "b.addEventListener('x', () => n++);"
                 "b.dispatchEvent(new Event('x')); b.dispatchEvent(new Event('x')); n",
                 "2");
    // Removable before it fires, which needs the wrapper to be transparent to
    // identity.
    ExpectScript(kPage,
                 "let n = 0; const f = () => n++; const b = document.body;"
                 "b.addEventListener('x', f, {once: true}); b.removeEventListener('x', f);"
                 "b.dispatchEvent(new Event('x')); n",
                 "0");
    // An event handler *attribute* that returns false has prevented the
    // default; a listener returning false has not.
    ExpectScript(kPage,
                 "const b = document.body; b.onx = () => false;"
                 "const e = new Event('x', {cancelable: true}); b.dispatchEvent(e);"
                 "e.defaultPrevented",
                 "true");
    ExpectScript(kPage,
                 "const b = document.body; b.addEventListener('x', () => false);"
                 "const e = new Event('x', {cancelable: true}); b.dispatchEvent(e);"
                 "e.defaultPrevented",
                 "false");
  });

  AddTest(tests, "DomBindings/UrlSearchParamsIsTheEnginesOwnUrlencoder", [] {
    ExpectScript(kPage, "new URLSearchParams('?a=1&b=2').get('a')", "1");
    ExpectScript(kPage, "new URLSearchParams('a=1').get('zz') === null", "true");
    ExpectScript(kPage, "new URLSearchParams('a=1&a=2').getAll('a').join(',')", "1,2");
    ExpectScript(kPage, "new URLSearchParams('a=1').has('a')", "true");
    ExpectScript(kPage, "new URLSearchParams('a=1&b=2').size", "2");
    // `set` replaces in place and drops the duplicates, which is observable
    // through toString and is what a page building a URL expects.
    ExpectScript(kPage, "const p = new URLSearchParams('a=1&b=2&a=3'); p.set('a','9');"
                        "p.toString()", "a=9&b=2");
    ExpectScript(kPage, "const p = new URLSearchParams('a=1'); p.append('a','2');"
                        "p.toString()", "a=1&a=2");
    ExpectScript(kPage, "const p = new URLSearchParams('a=1&b=2'); p.delete('a');"
                        "p.toString()", "b=2");
    ExpectScript(kPage, "const p = new URLSearchParams('b=2&a=1&b=1'); p.sort();"
                        "p.toString()", "a=1&b=2&b=1");
    // The callback is `(value, name)`, and reversing it is the classic way to
    // get this wrong -- reddit's challenge builds hidden inputs out of the two
    // and produces a form with its names and values swapped if they are.
    ExpectScript(kPage,
                 "const out = []; new URLSearchParams('a=1&b=2')"
                 ".forEach((value, name) => out.push(name + ':' + value)); out.join(',')",
                 "a:1,b:2");
    // Iterable, so destructuring and spread both work.
    ExpectScript(kPage, "[...new URLSearchParams('a=1&b=2')].map(e => e.join('')).join(',')",
                 "a1,b2");
    ExpectScript(kPage,
                 "let s = ''; for (const [n, v] of new URLSearchParams('a=1&b=2')) s += n + v; s",
                 "a1b2");
    ExpectScript(kPage, "new URLSearchParams('a=1&b=2').keys().join(',')", "a,b");
    ExpectScript(kPage, "new URLSearchParams('a=1&b=2').values().join(',')", "1,2");
    // The three other things a page constructs one from.
    ExpectScript(kPage, "new URLSearchParams({x: '1', y: '2'}).toString()", "x=1&y=2");
    ExpectScript(kPage, "new URLSearchParams([['k','v'],['k','w']]).toString()", "k=v&k=w");
    ExpectScript(kPage, "new URLSearchParams().toString()", "");
    // A copy, not an alias: writing to one must not show up in the other.
    ExpectScript(kPage,
                 "const a = new URLSearchParams('n=1'); const b = new URLSearchParams(a);"
                 "b.set('n','2'); a.get('n') + b.get('n')", "12");
    // The urlencoded serializer, shared with the form data set. `+` is a space
    // going in and coming out.
    ExpectScript(kPage, "new URLSearchParams('q=a+b').get('q')", "a b");
    ExpectScript(kPage, "const p = new URLSearchParams(); p.set('q', \"it's\"); p.toString()",
                 "q=it%27s");
    // What the page actually does: read the query, and turn it into a form.
    ExpectScript(kPage, "new URLSearchParams(location.search).get('q')", "1");
  });

  AddTest(tests, "DomBindings/DocumentExposesItsPartsAsAccessors", [] {
    // Accessors rather than stored values, so they follow the tree instead of
    // freezing what it looked like when the bindings were installed.
    ExpectScript("<html><head><title>Some Page</title></head><body></body></html>",
                 "document.title", "Some Page");
    ExpectScript("<html><head></head><body></body></html>", "document.head.tagName", "HEAD");
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
    const bool prevented = bound.dom_bindings->DispatchClick(*inner, {});
    Expect(!prevented, "nothing called preventDefault");

    // From the target up, which is what bubbling is -- and `this` is the node
    // the listener was registered on, not the one that was clicked.
    ExpectEqString(js::ToString(bound.interpreter->Run("seen.join(' ')").value),
                   "inner:click:true outer:true:true", "both ran, target first");
  });

  AddTest(tests, "DomBindings/PointerDownIsTrustedAndBubbles", [] {
    Bound bound = Bind("<div id=outer><button id=inner>go</button></div>");
    const js::Result setup = bound.interpreter->Run(
        "globalThis.seen = [];"
        "document.getElementById('inner').addEventListener('pointerdown', function(e) {"
        "  seen.push('inner:' + e.type + ':' + e.isTrusted);"
        "});"
        "document.getElementById('outer').addEventListener('pointerdown', function(e) {"
        "  seen.push('outer:' + e.pointerType);"
        "});"
        "'ready'");
    Expect(!setup.IsAbrupt(), "the listeners registered");
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
    bindings::PointerInput pointer;
    pointer.buttons = 1;
    Expect(!bound.dom_bindings->DispatchPointerMouse(*inner, "pointerdown", pointer),
           "nothing called preventDefault");
    ExpectEqString(js::ToString(bound.interpreter->Run("seen.join(' ')").value),
                   "inner:pointerdown:true outer:mouse", "pointerdown bubbled as PointerEvent");
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
      const bool prevented = inner != nullptr && bound.dom_bindings->DispatchClick(*inner, {});
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

  // --- The dispatch algorithm, rule by rule --------------------------------
  //
  // ADR 0017's consequence list names these as the tests that decay: each rule
  // is small, each is silently wrong until something asserts it, and a page
  // that depends on one fails in a way that looks like a bug somewhere else.

  // The element with the given id, which every dispatch test needs.
  const auto element_by_id = [](Bound& bound, const char* wanted) -> dom::Element* {
    dom::Element* found = nullptr;
    bound.document->ForEachDescendant([&](const dom::Node& node) {
      const std::string* id = node.IsElement()
                                  ? static_cast<const dom::Element&>(node).GetAttribute("id")
                                  : nullptr;
      if (id != nullptr && *id == wanted) {
        found = const_cast<dom::Element*>(&static_cast<const dom::Element&>(node));
      }
    });
    return found;
  };

  AddTest(tests, "DomBindings/DispatchRunsCaptureThenTargetThenBubble", [element_by_id] {
    Bound bound = Bind("<div id=outer><span id=inner>x</span></div>");
    bound.interpreter->Run(
        "globalThis.seen = [];"
        "const mark = n => e => seen.push(n + ':' + e.eventPhase);"
        "window.addEventListener('click', mark('win-cap'), true);"
        "window.addEventListener('click', mark('win-bub'));"
        "document.getElementById('outer').addEventListener('click', mark('out-cap'), true);"
        "document.getElementById('outer').addEventListener('click', mark('out-bub'));"
        "document.getElementById('inner').addEventListener('click', mark('in-cap'), true);"
        "document.getElementById('inner').addEventListener('click', mark('in-bub'));");
    dom::Element* inner = element_by_id(bound, "inner");
    Expect(inner != nullptr, "the inner element exists");
    bound.dom_bindings->DispatchClick(*inner, {});

    // Down from the window, both kinds at the target in registration order, then
    // up again. The phase numbers are the DOM's: 1 capturing, 2 at target, 3
    // bubbling -- and at the target it is 2 for both listeners, which is what a
    // handler comparing against Event.AT_TARGET is relying on.
    ExpectEqString(js::ToString(bound.interpreter->Run("seen.join(' ')").value),
                   "win-cap:1 out-cap:1 in-cap:2 in-bub:2 out-bub:3 win-bub:3",
                   "capture down, both at the target, bubble up");
  });

  AddTest(tests, "DomBindings/CaptureIsPartOfAListenersIdentity", [element_by_id] {
    Bound bound = Bind("<div id=d>x</div>");
    bound.interpreter->Run(
        "globalThis.seen = '';"
        "globalThis.f = () => { seen += 'f' };"
        "const d = document.getElementById('d');"
        "d.addEventListener('click', f, true);"
        "d.addEventListener('click', f);"
        // Removing the bubbling one must not take the capturing one with it.
        "d.removeEventListener('click', f);");
    dom::Element* target = element_by_id(bound, "d");
    Expect(target != nullptr, "the div exists");
    bound.dom_bindings->DispatchClick(*target, {});
    ExpectEqString(js::ToString(bound.interpreter->Run("seen").value), "f",
                   "the capturing registration survived removal of the bubbling one");
  });

  AddTest(tests, "DomBindings/StopImmediatePropagationStopsTheCurrentNodeToo", [element_by_id] {
    Bound bound = Bind("<div id=outer><span id=inner>x</span></div>");
    bound.interpreter->Run(
        "globalThis.seen = '';"
        "const inner = document.getElementById('inner');"
        "inner.addEventListener('click', e => { seen += 'a'; e.stopPropagation(); });"
        "inner.addEventListener('click', () => { seen += 'b'; });"
        "document.getElementById('outer').addEventListener('click', () => { seen += 'c'; });");
    dom::Element* inner = element_by_id(bound, "inner");
    bound.dom_bindings->DispatchClick(*inner, {});
    ExpectEqString(js::ToString(bound.interpreter->Run("seen").value), "ab",
                   "stopPropagation lets the rest of this node's listeners run");

    Bound immediate = Bind("<div id=outer><span id=inner>x</span></div>");
    immediate.interpreter->Run(
        "globalThis.seen = '';"
        "const inner = document.getElementById('inner');"
        "inner.addEventListener('click', e => { seen += 'a'; e.stopImmediatePropagation(); });"
        "inner.addEventListener('click', () => { seen += 'b'; });"
        "document.getElementById('outer').addEventListener('click', () => { seen += 'c'; });");
    dom::Element* target = element_by_id(immediate, "inner");
    immediate.dom_bindings->DispatchClick(*target, {});
    ExpectEqString(js::ToString(immediate.interpreter->Run("seen").value), "a",
                   "stopImmediatePropagation does not");
  });

  AddTest(tests, "DomBindings/APassiveListenerCannotPreventTheDefault", [element_by_id] {
    // The whole content of `{passive: true}`: the page promised not to cancel,
    // so the browser is entitled to have already started, and honouring the
    // call afterwards would make the promise meaningless.
    Bound bound = Bind("<div id=d>x</div>");
    bound.interpreter->Run(
        "const d = document.getElementById('d');"
        "d.addEventListener('click', e => e.preventDefault(), {passive: true});");
    dom::Element* target = element_by_id(bound, "d");
    Expect(!bound.dom_bindings->DispatchClick(*target, {}),
           "a passive listener's preventDefault did nothing");

    Bound active = Bind("<div id=d>x</div>");
    active.interpreter->Run(
        "const d = document.getElementById('d');"
        "d.addEventListener('click', e => e.preventDefault());");
    dom::Element* live = element_by_id(active, "d");
    Expect(active.dom_bindings->DispatchClick(*live, {}),
           "and the same listener without the flag still cancels");
  });

  AddTest(tests, "DomBindings/AListenerAddedDuringDispatchDoesNotRunForThisEvent",
          [element_by_id] {
    // The set that runs on a node is the set that existed when the event
    // reached it. Without the copy this is an infinite loop written by
    // accident, which is the bug it is here to keep out.
    Bound bound = Bind("<div id=d>x</div>");
    bound.interpreter->Run(
        "globalThis.n = 0;"
        "const d = document.getElementById('d');"
        "d.addEventListener('click', () => { n++; d.addEventListener('click', () => { n += 10 }); });");
    dom::Element* target = element_by_id(bound, "d");
    bound.dom_bindings->DispatchClick(*target, {});
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "1",
                   "only the listener that was registered ran");
    bound.dom_bindings->DispatchClick(*target, {});
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "12",
                   "and the one it added runs on the next event");
  });

  AddTest(tests, "DomBindings/IsTrustedSaysWhoMadeTheEventAndCannotBeSet", [element_by_id] {
    // ADR 0017 §3. The gates that will eventually read this -- opening a window,
    // entering fullscreen, reading the clipboard -- read it as a statement about
    // the user, so a page being able to write it is a security bug rather than a
    // fidelity one.
    Bound bound = Bind("<div id=d>x</div>");
    bound.interpreter->Run(
        "globalThis.seen = '';"
        "const d = document.getElementById('d');"
        "d.addEventListener('click', e => {"
        "  e.isTrusted = true;"
        "  seen += e.isTrusted;"
        "});");
    dom::Element* target = element_by_id(bound, "d");
    bound.dom_bindings->DispatchClick(*target, {});
    ExpectEqString(js::ToString(bound.interpreter->Run("seen").value), "true",
                   "the browser's own click is trusted");

    ExpectScript("<div id=d>x</div>",
                 "const e = new Event('click');"
                 "e.isTrusted = true;"
                 "'' + e.isTrusted",
                 "false");
  });

  AddTest(tests, "DomBindings/AKeyReachesTheFocusedElementWithItsThreeStrings", [element_by_id] {
    Bound bound = Bind("<div id=outer><input id=field></div>");
    bound.interpreter->Run(
        "globalThis.seen = [];"
        "document.getElementById('outer').addEventListener('keydown', e => {"
        "  seen.push(e.type + ' ' + e.key + ' ' + e.code + ' ' + e.keyCode + ' ' +"
        "            e.ctrlKey + ' ' + (e instanceof KeyboardEvent));"
        "});");
    dom::Element* field = element_by_id(bound, "field");
    Expect(field != nullptr, "the field exists");

    bindings::KeyInput key;
    key.code = "Escape";
    key.key = "Escape";
    key.control = true;
    Expect(!bound.dom_bindings->DispatchKey(field, key), "nothing cancelled it");
    // The whole point of the message set change: `Escape` was unreachable
    // before, because a key crossed the seam as the text it produced and Escape
    // produces none.
    ExpectEqString(js::ToString(bound.interpreter->Run("seen.join('|')").value),
                   "keydown Escape Escape 27 true true", "the key bubbled with its identity");
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
    bound.dom_bindings->DispatchClick(*target, {});
    // Only the anonymous one is left. Removal is by identity, which is why an
    // inline arrow cannot be removed -- and is what every browser does.
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + n").value), "10",
                   "the named handler was removed and the other still ran");
  });

  // ADR 0011 states the line these guard: *a page with no pending animation
  // frame does not schedule a frame at all.* That is the point where a browser
  // normally starts burning a core on an idle page, and a change that starts
  // doing it has to make one of these fail first.
  AddTest(tests, "AnimationFrames/NoRequestMeansNoFrameIsScheduled", [] {
    js::Interpreter interpreter;
    bindings::AnimationFrames frames;
    frames.Install(interpreter, 1000);
    Expect(!frames.NextDelay(1000).has_value(),
           "nothing asked for a frame, so no frame is scheduled and the loop may block");

    interpreter.Run("requestAnimationFrame(() => {});");
    Expect(frames.NextDelay(1000).has_value(), "one request, one frame deadline");
    Expect(frames.RunDue(interpreter, 1000), "and it runs at the boundary");
    Expect(!frames.NextDelay(1000).has_value(),
           "after which nothing is scheduled again -- a settled page must not keep a 60Hz "
           "loop running behind it");
  });

  AddTest(tests, "AnimationFrames/ACallbackThatAsksAgainGetsTheNextFrameAndNotThisOne", [] {
    js::Interpreter interpreter;
    bindings::AnimationFrames frames;
    frames.Install(interpreter, 0);
    interpreter.Run(
        "globalThis.n = 0;"
        "globalThis.again = () => { n++; requestAnimationFrame(again); };"
        "requestAnimationFrame(again);");

    Expect(frames.RunDue(interpreter, 0), "the first frame runs");
    ExpectEqString(js::ToString(interpreter.Run("String(n)").value), "1",
                   "exactly once: a callback that asks for another frame is asking for the "
                   "next one, and running it now would spin the loop inside a single turn");
    const std::optional<std::uint32_t> next = frames.NextDelay(0);
    Expect(next.has_value(), "the next frame is scheduled");
    ExpectEqInt(static_cast<long long>(*next),
                static_cast<long long>(bindings::kFrameIntervalMs),
                "a frame interval away, so the cadence does not depend on how long the "
                "callback took");
    Expect(!frames.RunDue(interpreter, 1), "and it does not run before its boundary");
  });

  AddTest(tests, "AnimationFrames/EveryCallbackInAFrameSeesTheSameTimestamp", [] {
    js::Interpreter interpreter;
    bindings::AnimationFrames frames;
    frames.Install(interpreter, 500);
    interpreter.Run(
        "globalThis.seen = [];"
        "requestAnimationFrame(t => seen.push(t));"
        "requestAnimationFrame(t => seen.push(t));");
    Expect(frames.RunDue(interpreter, 700), "the frame runs");
    ExpectEqString(js::ToString(interpreter.Run("seen.join(',')").value), "200,200",
                   "one frame is one moment, measured from when the page loaded -- two "
                   "callbacks handed two different times is how animations desynchronise");
  });

  AddTest(tests, "AnimationFrames/CancellingLeavesNothingScheduled", [] {
    js::Interpreter interpreter;
    bindings::AnimationFrames frames;
    frames.Install(interpreter, 0);
    interpreter.Run(
        "globalThis.n = 0;"
        "const id = requestAnimationFrame(() => { n++ });"
        "cancelAnimationFrame(id);");
    Expect(!frames.NextDelay(0).has_value(), "the request is gone");
    Expect(!frames.RunDue(interpreter, 100), "and nothing runs");
    ExpectEqString(js::ToString(interpreter.Run("String(n)").value), "0", "the callback did not");
  });

  AddTest(tests, "AnimationFrames/AFrameCallbackClearsASpentStepBudget", [] {
    // TD-0018: youtube autofill chains through requestAnimationFrame after
    // kevlar has already burned kMaxSteps. Same BeginTask contract as idle
    // callbacks and MessageChannel tasks.
    js::Interpreter interpreter;
    bindings::AnimationFrames frames;
    frames.Install(interpreter, 0);
    Expect(interpreter.Run("globalThis.got = 0;"
                           "requestAnimationFrame(() => { got = 42 });"
                           "while (true) {}")
               .completion == js::Completion::Throw,
           "burn the hang guard without absorbing it");
    Expect(frames.RunDue(interpreter, 0), "deliver the frame callback");
    ExpectEqString(js::ToString(interpreter.Run("'' + got").value), "42",
                   "frame callback ran after a spent budget");
  });

  AddTest(tests, "IdleCallbacks/NothingScheduledMeansTheLoopMayBlock", [] {
    js::Interpreter interpreter;
    bindings::IdleCallbacks idle;
    idle.Install(interpreter, 1000);
    Expect(!idle.NextDelay(1000).has_value(),
           "nothing asked for idle work, so no deadline and the loop may block");

    interpreter.Run("requestIdleCallback(() => {});");
    Expect(idle.NextDelay(1000).has_value(), "one request, one idle deadline");
    Expect(idle.RunDue(interpreter, 1000), "and it runs on the next idle pass");
    Expect(!idle.NextDelay(1000).has_value(),
           "after which nothing is scheduled again -- a settled page must not keep waking");
  });

  AddTest(tests, "IdleCallbacks/DeadlineReportsTimeoutAndTimeRemaining", [] {
    js::Interpreter interpreter;
    bindings::IdleCallbacks idle;
    idle.Install(interpreter, 0);
    interpreter.Run(
        "globalThis.seen = '';"
        "requestIdleCallback(d => {"
        "  seen = d.didTimeout + ':' + d.timeRemaining();"
        "}, { timeout: 40 });");
    Expect(idle.RunDue(interpreter, 50), "the timeout eventually fires");
    ExpectEqString(js::ToString(interpreter.Run("seen").value), "true:50",
                   "a timed-out callback is told it timed out and gets a budget");
  });

  AddTest(tests, "IdleCallbacks/AnIdleCallbackClearsASpentStepBudget", [] {
    // TD-0018: youtube's scheduler.js stamps through requestIdleCallback.
    // After kevlar spends kMaxSteps those callbacks must BeginTask.
    js::Interpreter interpreter;
    bindings::IdleCallbacks idle;
    idle.Install(interpreter, 0);
    Expect(interpreter.Run("globalThis.got = 0;"
                           "requestIdleCallback(() => { got = 42 });"
                           "while (true) {}")
               .completion == js::Completion::Throw,
           "burn the hang guard without absorbing it");
    Expect(idle.RunDue(interpreter, 0), "deliver the idle callback");
    ExpectEqString(js::ToString(interpreter.Run("'' + got").value), "42",
                   "idle callback ran after a spent budget");
  });

  AddTest(tests, "DomBindings/AMutationObserverCallbackClearsASpentStepBudget", [] {
    // TD-0018: Polymer's ASAP observer is a MutationObserver microtask.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter
               ->Run("globalThis.got = 0;"
                     "const o = new MutationObserver(() => { got = 42 });"
                     "o.observe(document.body, { childList: true });"
                     "setTimeout(() => document.body.appendChild(document.createElement('i')), 0);")
               .completion == js::Completion::Normal,
           "arm the observer delivery");
    Expect(bound.interpreter->Run("while (true) {}").completion == js::Completion::Throw,
           "burn the hang guard without absorbing it");
    Expect(timers.RunDue(*bound.interpreter, 0), "deliver the timeout");
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + got").value), "42",
                   "MutationObserver callback ran after a spent budget");
  });

  AddTest(tests, "DomBindings/InsertFragmentUpgradeClearsASpentStepBudget", [] {
    // TD-0018: DOM insertion upgrades custom elements after a long stamp.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    Expect(bound.interpreter
               ->Run("globalThis.got = 0;"
                     "class X extends HTMLElement {"
                     "  connectedCallback() { this.dataset.ok = '1'; }"
                     "}"
                     "customElements.define('x-el', X);"
                     "setTimeout(() => {"
                     "  const f = document.createDocumentFragment();"
                     "  f.innerHTML = '<x-el></x-el>';"
                     "  document.body.appendChild(f);"
                     "}, 0);")
               .completion == js::Completion::Normal,
           "arm the fragment insert");
    Expect(bound.interpreter->Run("while (true) {}").completion == js::Completion::Throw,
           "burn the hang guard without absorbing it");
    Expect(timers.RunDue(*bound.interpreter, 0), "deliver the timeout");
    ExpectEqString(js::ToString(bound.interpreter
                                    ->Run("document.querySelector('x-el').dataset.ok")
                                    .value),
                   "1", "connectedCallback ran after a spent budget");
  });

  AddTest(tests, "DomBindings/SuspenseReplaceHoistsTemplateForMarkup", [] {
    // reddit's inline boot script (feed HTML): `<suspense-replace
    // target="#s_…" template="template[for=s_…]">` adopts the template
    // contents, upgrades custom elements on the fragment, then replaceWith.
    static constexpr const char* kPage =
        "<html><body>"
        "<suspense-placeholder id=\"s_feed\" name=\"feed\"></suspense-placeholder>"
        "<template for=\"s_feed\"><article class=\"post\">feed</article></template>"
        "<suspense-replace target=\"#s_feed\" template=\"template[for=s_feed]\">"
        "</suspense-replace>"
        "</body></html>";
    ExpectScript(kPage,
                 "async function hoist(target, tpl) {"
                 "  const frag = document.adoptNode(tpl.content);"
                 "  customElements.upgrade(frag);"
                 "  target.replaceWith(frag);"
                 "  tpl.remove();"
                 "}"
                 "class SuspenseReplace extends HTMLElement {"
                 "  connectedCallback() {"
                 "    const root = this.getRootNode();"
                 "    const target = root.querySelector(this.getAttribute('target'));"
                 "    const tpl = root.querySelector(this.getAttribute('template'));"
                 "    if (target && tpl) hoist(target, tpl);"
                 "    this.remove();"
                 "  }"
                 "}"
                 "customElements.define('suspense-replace', SuspenseReplace);"
                 "document.querySelectorAll('.post').length + ':' +"
                 " document.querySelectorAll('suspense-replace').length + ':' +"
                 " document.querySelectorAll('template[for]').length",
                 "1:0:0");
  });

  AddTest(tests, "DomBindings/CustomElementsUpgradeWalksAFragment", [] {
    static constexpr const char* kPage =
        "<html><body><div id='host'></div></body></html>";
    ExpectScript(kPage,
                 "class XFoo extends HTMLElement {"
                 "  connectedCallback() { this.dataset.up = '1'; }"
                 "}"
                 "customElements.define('x-foo', XFoo);"
                 "const t = document.createElement('template');"
                 "t.content.appendChild(document.createElement('x-foo'));"
                 "const frag = document.adoptNode(t.content);"
                 "customElements.upgrade(frag);"
                 "document.getElementById('host').appendChild(frag);"
                 "document.querySelector('x-foo').dataset.up",
                 "1");
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
                 "1 P one");
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
                 "one:P:true");
  });

  AddTest(tests, "DomBindings/NodesCanBeInsertedAndReplacedInPlace", [] {
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const fresh = document.createElement('em');"
                 "list.insertBefore(fresh, list.children[0]);"
                 "list.children[0].tagName + ' ' + list.children.length",
                 "EM 3");
    // A null reference appends, which is what the specification says and what
    // a page relies on when it inserts before "nothing".
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.insertBefore(document.createElement('em'), null);"
                 "list.children[list.children.length - 1].tagName",
                 "EM");
    // In before out, so the replacement lands where the old node was rather
    // than at the end -- the whole difference from remove-then-append.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "const fresh = document.createElement('em');"
                 "const old = list.children[0];"
                 "const returned = list.replaceChild(fresh, old);"
                 "list.children[0].tagName + ' ' + list.children.length + ' ' + "
                 "(returned === old) + ' ' + returned.textContent",
                 "EM 2 true one");
  });

  AddTest(tests, "DomBindings/AppendingAnAttachedNodeMovesIt", [] {
    // Which works only because detaching hands the node over rather than
    // destroying it. This is how a page reorders a list.
    ExpectScript("<div id=box><a></a><b></b><c></c></div>",
                 "const box = document.getElementById('box');"
                 "box.appendChild(box.children[0]);"
                 "Array.from(box.children).map(e => e.tagName).join('')",
                 "BCA");
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
                 "P:one");

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

  AddTest(tests, "DomBindings/TheHtmlPropertiesReadAndWrite", [] {
    ExpectScript(kPage, "document.getElementById('list').innerHTML",
                 "<p>one</p><p>two</p>");
    ExpectScript(kPage, "document.getElementById('title').outerHTML",
                 "<h1 id=\"title\" class=\"big head\">Hello</h1>");
    // Writing runs the fragment parsing algorithm with this element as the
    // context, which is what makes the markup become nodes rather than text.
    // Until session 14 both were read-only, because a document parser given a
    // fragment builds the wrong tree quietly.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.innerHTML = '<i>x</i>';"
                 "t.children.length + ':' + t.children[0].tagName + ':' + t.textContent",
                 "1:I:x");
    // The context element is the observable difference: `<td>` becomes a cell
    // in a row and bare text anywhere else.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.innerHTML = '<td>c</td>';"
                 "t.children.length + ':' + t.innerHTML",
                 "0:c");
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.innerHTML = '<table><tr><td>c</td></tr></table>';"
                 "t.querySelector('td').textContent + ':' + "
                 "  (t.querySelector('tbody') !== null)",
                 "c:true");
    // Writing replaces what was there, rather than appending to it.
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.innerHTML = '<p>only</p>';"
                 "list.children.length + ':' + list.textContent",
                 "1:only");
    // `outerHTML` replaces the element itself, in place.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "const parent = t.parentNode;"
                 "t.outerHTML = '<h2 id=\"t2\">new</h2>';"
                 "document.getElementById('title') === null ? "
                 "  document.getElementById('t2').tagName : 'still there'",
                 "H2");
  });

  AddTest(tests, "DomBindings/InnerHtmlDoesNotRunScripts", [] {
    // The property that stops `el.innerHTML = userText` from being arbitrary
    // code execution. A script element that arrives this way is an element and
    // nothing more -- `PageScript::Collect` gathers a document's scripts once,
    // when the document is parsed.
    ExpectScript(kPage,
                 "window.ran = false;"
                 "document.getElementById('title').innerHTML = "
                 "  '<script>window.ran = true<\\/script>';"
                 "String(window.ran) + ':' + "
                 "  document.getElementById('title').children.length",
                 "false:1");
  });

  AddTest(tests, "DomBindings/InsertAdjacentHtmlPlacesNodesRelativeToTheElement", [] {
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.insertAdjacentHTML('afterbegin', '<p>zero</p>');"
                 "list.insertAdjacentHTML('beforeend', '<p>three</p>');"
                 "Array.from(list.children).map(c => c.textContent).join(',')",
                 "zero,one,two,three");
    ExpectScript(kPage,
                 "const list = document.getElementById('list');"
                 "list.insertAdjacentHTML('beforebegin', '<div id=\"before\"></div>');"
                 "list.insertAdjacentHTML('afterend', '<div id=\"after\"></div>');"
                 "const kids = Array.from(list.parentNode.children).map(c => c.id || c.tagName);"
                 "kids.indexOf('before') + ',' + kids.indexOf('list') + ',' + kids.indexOf('after')",
                 "1,2,3");
    // A mistyped position throws rather than silently doing nothing, which
    // otherwise looks exactly like markup the parser dropped.
    ExpectScript(kPage,
                 "try { document.getElementById('list').insertAdjacentHTML('inside', 'x'); 'no' }"
                 "catch (e) { 'threw' }",
                 "threw");
  });

  AddTest(tests, "DomBindings/ParentNodeAppendAndReplaceChildren", [] {
    // reddit's `ac-render-template` hoists with `replaceChildren` and
    // `append`; Polymer stamps with `append`. All three must move fragments
    // and strings the way `appendChild` already does.
    static constexpr const char* kTemplatePage =
        "<html><body><div id='host'><i>old</i></div>"
        "<template id='t'><p class='row'>x</p><span>y</span></template>"
        "</body></html>";
    ExpectScript(kTemplatePage, "typeof document.getElementById('host').append", "function");
    ExpectScript(kTemplatePage, "typeof document.getElementById('host').replaceChildren",
                 "function");
    ExpectScript(kTemplatePage,
                 "const host = document.getElementById('host');"
                 "host.replaceChildren('a', document.createElement('b'));"
                 "host.childNodes.length + ':' + host.textContent + ':' + host.lastChild.tagName",
                 "2:a:B");
    ExpectScript(kTemplatePage,
                 "const t = document.getElementById('t');"
                 "const host = document.getElementById('host');"
                 "host.replaceChildren(t.content.cloneNode(true));"
                 "document.querySelectorAll('.row').length + ':' + host.textContent.trim() + ':' +"
                 " t.content.children.length",
                 "1:xy:2");
    ExpectScript(kTemplatePage,
                 "const host = document.getElementById('host');"
                 "const rep = document.createElement('em');"
                 "host.firstChild.replaceWith(rep, 'tail');"
                 "host.childNodes.length + ':' + host.innerHTML",
                 "2:<em></em>tail");
  });

  AddTest(tests, "DomBindings/TemplateContentIsReachableOnlyThroughContent", [] {
    // A template's markup is not document content: no walk of the tree finds
    // it, and `content` is the only way to reach it. That is what makes a
    // template a place a page keeps markup it has not used yet.
    static constexpr const char* kTemplatePage =
        "<html><body><template id='t'><p class='row'>x</p></template></body></html>";
    ExpectScript(kTemplatePage, "document.querySelectorAll('.row').length", "0");
    ExpectScript(kTemplatePage, "document.getElementById('t').children.length", "0");
    ExpectScript(kTemplatePage,
                 "const c = document.getElementById('t').content;"
                 "c.children.length + ':' + c.children[0].className",
                 "1:row");
    // The usual idiom: clone the contents and append the clone. It only works
    // if a deep clone follows the contents rather than the (empty) child list.
    ExpectScript(kTemplatePage,
                 "const t = document.getElementById('t');"
                 "document.body.appendChild(t.content.cloneNode(true));"
                 "document.querySelectorAll('.row').length + ':' + t.content.children.length",
                 "1:1");
    // Writing a template's `innerHTML` writes its contents, not its children.
    ExpectScript(kTemplatePage,
                 "const t = document.getElementById('t');"
                 "t.innerHTML = '<span>y</span>';"
                 "t.children.length + ':' + t.content.children.length + ':' + t.innerHTML",
                 "0:1:<span>y</span>");
  });

  AddTest(tests, "DomBindings/ImportNodeStampsATemplateTheWayPolymerDoes", [] {
    // Polymer (and Lit, and every webcomponents polyfill) stamps with
    // `document.importNode(template.content, true)`, not `cloneNode`. The two
    // must agree on the tree, and `importNode` must exist: a missing name is
    // what left youtube.com as two upgraded hosts with empty shadows.
    static constexpr const char* kTemplatePage =
        "<html><body><template id='t'><p class='row'>x</p><span>y</span></template>"
        "</body></html>";
    ExpectScript(kTemplatePage, "typeof document.importNode", "function");
    ExpectScript(kTemplatePage, "typeof document.adoptNode", "function");
    ExpectScript(kTemplatePage,
                 "const t = document.getElementById('t');"
                 "const stamp = document.importNode(t.content, true);"
                 "document.body.appendChild(stamp);"
                 "document.querySelectorAll('.row').length + ':' +"
                 " document.body.textContent.trim() + ':' + t.content.children.length",
                 "1:xy:2");
    // Shallow import copies the fragment and none of its children -- the
    // specification's default, and the footgun Polymer avoids by passing true.
    ExpectScript(kTemplatePage,
                 "const stamp = document.importNode(document.getElementById('t').content);"
                 "stamp.childNodes.length",
                 "0");
    ExpectScript(kTemplatePage,
                 "try { document.importNode(document); 'no' } catch (e) { 'threw' }",
                 "threw");
    ExpectScript(kTemplatePage,
                 "const host = document.createElement('div');"
                 "const root = host.attachShadow({mode:'open'});"
                 "try { document.importNode(root, true); 'no' } catch (e) { 'threw' }",
                 "threw");
    // adoptNode of a node already here is identity, and leaves it in place.
    ExpectScript(kTemplatePage,
                 "const p = document.createElement('p');"
                 "document.body.appendChild(p);"
                 "document.adoptNode(p) === p && p.parentNode === document.body",
                 "true");
    // Comments are children. Polymer indexes stamp targets by child offset,
    // so a clone that dropped them would point every listener at the wrong
    // node (or at undefined). Built with createComment rather than markup:
    // the HTML parser is free to drop empty comments, and this assertion is
    // about the clone, not the parser.
    ExpectScript("<html><body></body></html>",
                 "const t = document.createElement('template');"
                 "t.content.appendChild(document.createComment(''));"
                 "t.content.appendChild(document.createElement('p'));"
                 "t.content.appendChild(document.createComment(''));"
                 "const s = document.importNode(t.content, true);"
                 "s.childNodes.length + ':' + s.childNodes[0].nodeType + ':' +"
                 " s.childNodes[2].nodeType",
                 "3:8:8");
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
    // CSSOM methods, not CSS properties. The Proxy used to answer every unknown
    // name as a declaration and return "", so `typeof style.setProperty` was
    // `"string"` and ShadyCSS's `style.setProperty(...)` threw. youtube.com.
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "typeof t.style.setProperty + ' ' + typeof t.style.getPropertyValue + ' ' +"
                 " typeof t.style.removeProperty",
                 "function function function");
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.style.setProperty('background-color', 'green');"
                 "t.style.getPropertyValue('background-color') + '|' + t.style.backgroundColor +"
                 " '|' + t.getAttribute('style')",
                 "green|green|background-color: green");
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.style.setProperty('color', 'red');"
                 "t.style.removeProperty('color') + '|' + t.style.color + '|' +"
                 " (t.getAttribute('style') || '')",
                 "red||");
    ExpectScript(kPage,
                 "const t = document.getElementById('title');"
                 "t.style.setProperty('margin', '1px', 'important');"
                 "t.getAttribute('style')",
                 "margin: 1px !important");
  });

  AddTest(tests, "DomBindings/ChildElementCountSkipsTextNodes", [] {
    ExpectScript("<div id=d>a<span></span>b<em></em>c</div>",
                 "document.getElementById('d').childElementCount", "2");
    ExpectScript("<div id=d></div>", "document.getElementById('d').childElementCount", "0");
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

  AddTest(tests, "DomBindings/DatasetWritesReachTheAttribute", [] {
    // youtube's player does `movie_player.dataset.version = jsUrl`; a snapshot
    // object dropped that write and J14's version check then destroyed the stamp.
    ExpectScript("<div id=d></div>",
                 "const el = document.getElementById('d');"
                 "el.dataset.version = '/s/player/x/base.js';"
                 "el.getAttribute('data-version') + '|' + el.dataset.version",
                 "/s/player/x/base.js|/s/player/x/base.js");
    ExpectScript("<div id=d data-user-id='1'></div>",
                 "const el = document.getElementById('d');"
                 "el.dataset.userId = '9';"
                 "el.getAttribute('data-user-id')",
                 "9");
  });

  AddTest(tests, "DomBindings/CloningCopiesRatherThanShares", [] {
    // Shallow by default, which catches out everyone who forgets the argument
    // and is what the specification says.
    ExpectScript(kPage,
                 "const c = document.getElementById('list').cloneNode();"
                 "c.tagName + ' ' + c.children.length + ' ' + c.getAttribute('id')",
                 "DIV 0 list");
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
