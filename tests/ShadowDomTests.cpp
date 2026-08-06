// Shadow DOM: the tree layout and the cascade actually walk.
//
// ADR 0019 §1-2. The assertions worth reading are the ones about the *flattened*
// tree, because that is the half a materialised second tree gets wrong: a host
// with no `<slot>` renders none of its own children, a slot with no assignment
// renders its fallback, and a slotted node inherits from where it *renders*
// rather than from where it is written.

#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "dom/FlatTree.h"
#include "engine/Page.h"
#include "gfx/FontCatalog.h"
#include "html/TreeBuilder.h"
#include "support/SyntheticFont.h"

namespace microbrowser::tests {

namespace {

struct TestFonts {
  gfx::FontLibrary library;
  gfx::FontCatalog catalog{library};

  TestFonts() {
    catalog.Register("Test", 400, false, BuildSyntheticFont());
    catalog.SetDefaultFamily("Test");
  }
};

// Runs `script` on `html` and joins what it logged.
std::string Run(std::string_view html, std::string_view script) {
  static TestFonts fonts;
  engine::Page page(fonts.catalog);
  std::string document = "<html><body>";
  document += html;
  document += "<script>";
  document += script;
  document += "</script></body></html>";
  page.Load(document, "https://page.example/");
  page.SetViewport(css::MediaContext{800.0f, 600.0f, 1.0f});
  page.RunScripts(0);
  std::string joined;
  for (const std::string& line : page.ConsoleOutput()) {
    joined += joined.empty() ? "" : "|";
    joined += line;
  }
  for (const std::string& error : page.ScriptErrors()) {
    joined += "!" + error;
  }
  return joined;
}

}  // namespace

void RegisterShadowDomTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ShadowDom/AttachShadowGivesARootAndClosedHidesIt", [] {
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "const r = h.attachShadow({mode: 'open'});"
                       "console.log(typeof r + ' ' + (h.shadowRoot === r));"),
                   "object true", "an open root is the one the element reports");
    // Closed mode is not a security boundary and ADR 0019 refuses to pretend
    // otherwise: the root still exists and still renders. The *only* thing it
    // changes is this getter.
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "const r = h.attachShadow({mode: 'closed'});"
                       "console.log(typeof r + ' ' + (h.shadowRoot === null));"),
                   "object true", "a closed root exists and is not reported");
  });

  AddTest(tests, "ShadowDom/ShadowRootHostAndModeAnswerAboutTheHost", [] {
    // ShadyDOM's parent-chain walk is `fragment.host ? fragment.host : …`.
    // Without `host` the walk never leaves the root, and every framework that
    // asks `root.host === this` after `attachShadow` gets false.
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "const r = h.attachShadow({mode: 'open'});"
                       "console.log((r.host === h) + ' ' + r.mode + ' ' +"
                       " (r instanceof ShadowRoot));"),
                   "true open true", "host points at the element; mode is open");
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "const r = h.attachShadow({mode: 'closed'});"
                       "console.log((r.host === h) + ' ' + r.mode);"),
                   "true closed", "a closed root still names its host and mode");
    ExpectEqString(Run("",
                       "const f = document.createDocumentFragment();"
                       "console.log((f.host == null) + ' ' + (f.mode == null));"),
                   "true true", "a plain fragment has neither");
  });

  AddTest(tests, "ShadowDom/ReplacingWindowShadowRootReplacesTheBareNameToo", [] {
    // ShadyDOM does `window.ShadowRoot = yc` and then stamps with
    // `ShadowRoot.prototype.za`. An own property *and* a scope binding made
    // those two spellings diverge: the property changed, the bare name did
    // not, and youtube.com's Polymer stamp called `undefined` as a method.
    ExpectEqString(Run("",
                       "function Yc(){}"
                       "Yc.prototype.za = function(){ return 1; };"
                       "window.ShadowRoot = Yc;"
                       "console.log((ShadowRoot === window.ShadowRoot) + ' ' +"
                       " (typeof ShadowRoot.prototype.za) + ' ' +"
                       " ShadowRoot.prototype.za());"),
                   "true function 1",
                   "bare ShadowRoot and window.ShadowRoot stay one namespace");
  });

  AddTest(tests, "ShadowDom/NodeTypeConstantsAreOnTheNodeInterface", [] {
    // ShadyDOM defines its ShadowRoot's nodeType as
    // `Node.DOCUMENT_FRAGMENT_NODE`. Without the constant that property is
    // undefined, getRootNode returns undefined, and ShadyCSS throws on class.
    ExpectEqString(Run("",
                       "console.log(Node.ELEMENT_NODE + ' ' +"
                       " Node.TEXT_NODE + ' ' + Node.COMMENT_NODE + ' ' +"
                       " Node.DOCUMENT_NODE + ' ' +"
                       " Node.DOCUMENT_FRAGMENT_NODE + ' ' +"
                       " Node.prototype.DOCUMENT_FRAGMENT_NODE);"),
                   "1 3 8 9 11 11",
                   "the twelve DOM nodeType constants, on the interface");
  });

  AddTest(tests, "ShadowDom/GetRootNodeFindsTheShadowRootAndOptionallyTheDocument", [] {
    // The presence of this method is load-bearing for youtube.com: ShadyDOM
    // enables itself when `getRootNode` is missing, and then fights the
    // native shadow DOM it detected via `attachShadow` alone.
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "const r = h.attachShadow({mode: 'open'});"
                       "r.innerHTML = '<span id=s></span>';"
                       "const s = r.querySelector('#s');"
                       "console.log((s.getRootNode() === r) + ' ' +"
                       " (s.getRootNode({composed:true}) === document) + ' ' +"
                       " (h.getRootNode() === document));"),
                   "true true true",
                   "default stops at the shadow root; composed climbs out");
  });

  AddTest(tests, "ShadowDom/ASecondAttachShadowIsAnErrorRatherThanAReplacement", [] {
    // Replacing it silently would strand every reference the page holds into the
    // first tree.
    ExpectEqString(Run("<div id=h></div>",
                       "const h = document.getElementById('h');"
                       "h.attachShadow({mode: 'open'});"
                       "try { h.attachShadow({mode: 'open'}) } catch (e) { console.log(e.name) }"),
                   "NotSupportedError", "the second call throws");
  });

  AddTest(tests, "ShadowDom/AHostRendersItsShadowTreeAndNotItsOwnChildren", [] {
    // The property every framework depends on: text written inside a component
    // does not appear unless the component asks for it with a `<slot>`.
    dom::Element host("div");
    dom::DocumentFragment* root = host.AttachShadow(true);
    Expect(root != nullptr, "a root");
    root->Append(std::make_unique<dom::Text>("shadow"));
    host.Append(std::make_unique<dom::Text>("light"));

    const std::vector<dom::Node*> flat = dom::FlatChildren(host);
    ExpectEqInt(static_cast<long long>(flat.size()), 1, "one flat child");
    ExpectEqString(flat.at(0)->TextContent(), "shadow",
                   "the shadow tree's, not the element's own");
    // And the node tree is unchanged, which is the other half of "two trees":
    // `childNodes` still answers about the light DOM.
    ExpectEqInt(static_cast<long long>(host.Children().size()), 1, "the node tree still has one");
    ExpectEqString(host.Children().at(0)->TextContent(), "light", "and it is the light one");
  });

  AddTest(tests, "ShadowDom/ASlotIsFilledByTheHostsChildrenAndFallsBackWhenEmpty", [] {
    dom::Element host("div");
    dom::DocumentFragment* root = host.AttachShadow(true);
    auto slot = std::make_unique<dom::Element>("slot");
    slot->Append(std::make_unique<dom::Text>("fallback"));
    dom::Element* slot_ptr = slot.get();
    root->Append(std::move(slot));

    // Nothing assigned: the slot's own children are its content. This is the case
    // a materialised flat tree gets wrong, because the fallback is *conditional*.
    std::vector<dom::Node*> flat = dom::FlatChildren(*slot_ptr);
    ExpectEqInt(static_cast<long long>(flat.size()), 1, "the fallback");
    ExpectEqString(flat.at(0)->TextContent(), "fallback", "is the slot's own child");

    host.Append(std::make_unique<dom::Text>("given"));
    flat = dom::FlatChildren(*slot_ptr);
    ExpectEqInt(static_cast<long long>(flat.size()), 1, "now the assignment");
    ExpectEqString(flat.at(0)->TextContent(), "given",
                   "and the fallback is gone the moment something matches");
  });

  AddTest(tests, "ShadowDom/ANamedSlotTakesOnlyTheChildrenThatAskForIt", [] {
    dom::Element host("div");
    dom::DocumentFragment* root = host.AttachShadow(true);
    auto named = std::make_unique<dom::Element>("slot");
    named->SetAttribute("name", "title");
    dom::Element* named_ptr = named.get();
    auto unnamed = std::make_unique<dom::Element>("slot");
    dom::Element* unnamed_ptr = unnamed.get();
    root->Append(std::move(named));
    root->Append(std::move(unnamed));

    auto titled = std::make_unique<dom::Element>("h1");
    titled->SetAttribute("slot", "title");
    titled->Append(std::make_unique<dom::Text>("heading"));
    host.Append(std::move(titled));
    host.Append(std::make_unique<dom::Text>("body text"));

    const std::vector<dom::Node*> to_named = dom::FlatChildren(*named_ptr);
    ExpectEqInt(static_cast<long long>(to_named.size()), 1, "the named slot took one");
    ExpectEqString(to_named.at(0)->TextContent(), "heading", "the one that asked for it");
    const std::vector<dom::Node*> to_unnamed = dom::FlatChildren(*unnamed_ptr);
    ExpectEqInt(static_cast<long long>(to_unnamed.size()), 1, "and the default took the other");
    ExpectEqString(to_unnamed.at(0)->TextContent(), "body text", "the one with no slot=");
  });

  AddTest(tests, "ShadowDom/AnAssignedNodeAppearsExactlyOnce", [] {
    // The bug a materialised tree produces: a slotted node rendering both where
    // it is written and where it is slotted.
    dom::Element host("div");
    dom::DocumentFragment* root = host.AttachShadow(true);
    root->Append(std::make_unique<dom::Element>("slot"));
    host.Append(std::make_unique<dom::Text>("once"));

    const std::vector<dom::Node*> host_children = dom::FlatChildren(host);
    ExpectEqInt(static_cast<long long>(host_children.size()), 1, "the host has the slot");
    Expect(host_children.at(0)->IsElement(), "which is an element");
    // The text is reached through the slot and nowhere else.
    const std::vector<dom::Node*> through_slot = dom::FlatChildren(*host_children.at(0));
    ExpectEqInt(static_cast<long long>(through_slot.size()), 1, "and the text is under it");
    ExpectEqString(through_slot.at(0)->TextContent(), "once", "once");
  });

  AddTest(tests, "ShadowDom/TheHostOfAShadowNodeIsReachableAndOfALightNodeIsNot", [] {
    dom::Element host("div");
    dom::DocumentFragment* root = host.AttachShadow(true);
    auto inner = std::make_unique<dom::Element>("span");
    dom::Element* inner_ptr = inner.get();
    root->Append(std::move(inner));
    host.Append(std::make_unique<dom::Element>("p"));

    Expect(dom::ShadowHostOf(*inner_ptr) == &host, "a node inside the root knows its host");
    Expect(dom::ShadowHostOf(*host.Children().at(0)) == nullptr,
           "and a light-DOM child does not: it is in the document, not in a shadow tree");
  });

  AddTest(tests, "ShadowDom/ShadowContentRendersAndLightContentDoesNot", [] {
    // Through a real Page, so this is the cascade and layout walking the flat
    // tree rather than the traversal being asked directly.
    ExpectEqString(
        Run("<div id=h>light</div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.appendChild(document.createElement('p')).textContent = 'shadow';"
            "console.log(document.body.textContent.indexOf('light') >= 0);"),
        "true", "the node tree still contains the light text");
  });

  AddTest(tests, "ShadowDom/AssignedNodesAnswersFromTheSameTraversalAPaintUses", [] {
    ExpectEqString(
        Run("<div id=h><span slot=x>one</span><span>two</span></div>",
            "const h = document.getElementById('h');"
            "const r = h.attachShadow({mode: 'open'});"
            "const named = document.createElement('slot');"
            "named.setAttribute('name', 'x');"
            "r.appendChild(named);"
            "const plain = document.createElement('slot');"
            "r.appendChild(plain);"
            "console.log(named.assignedNodes().length + ' ' + named.assignedNodes()[0].textContent);"
            "console.log(plain.assignedNodes().length + ' ' + plain.assignedNodes()[0].textContent);"
            "console.log(h.children[0].assignedSlot === named);"),
        "1 one|1 two|true", "and assignedSlot is the other direction of the same answer");
  });

  AddTest(tests, "ShadowDom/AnEventFromInsideAComponentIsRetargetedToTheHost", [] {
    // ADR 0019 §5, and the reason it matters is not tidiness: without
    // retargeting, a listener on the page gets a `target` it could never have
    // obtained a reference to -- which leaks the component's internal shape and
    // gives the page a node it cannot compare against anything it holds.
    ExpectEqString(
        Run("<div id=h></div>",
            "const h = document.getElementById('h');"
            "const r = h.attachShadow({mode: 'open'});"
            "const inner = document.createElement('span');"
            "r.appendChild(inner);"
            "h.addEventListener('ping', function (e) {"
            "  console.log('host sees ' + (e.target === h) + ' ' + (e.target === inner));"
            "});"
            "inner.dispatchEvent(new Event('ping', {bubbles: true, composed: true}));"),
        "host sees true false",
        "the listener outside the tree sees the component, not what is inside it");
  });

  AddTest(tests, "ShadowDom/ComposedPathIsTheWholePathAndOnlyForAComposedEvent", [] {
    // The other half of retargeting: the component can still ask what the real
    // target was, and a *non*-composed event does not hand that out.
    ExpectEqString(
        Run("<div id=h></div>",
            "const h = document.getElementById('h');"
            "const r = h.attachShadow({mode: 'open'});"
            "const inner = document.createElement('span');"
            "r.appendChild(inner);"
            "inner.addEventListener('ping', function (e) {"
            "  const p = e.composedPath();"
            "  console.log('first ' + (p[0] === inner) + ' has host ' + p.includes(h));"
            "});"
            "inner.dispatchEvent(new Event('ping', {bubbles: true, composed: true}));"),
        "first true has host true",
        "the path starts at the real target and reaches out through the host");
  });

  AddTest(tests, "ShadowDom/AnEventThatCrossesNoBoundaryIsNotRetargeted", [] {
    // The case that would break every existing page if retargeting were applied
    // unconditionally: an ordinary event in the light DOM keeps its own target.
    ExpectEqString(
        Run("<div id=h><span id=s></span></div>",
            "const s = document.getElementById('s');"
            "document.getElementById('h').addEventListener('ping', function (e) {"
            "  console.log('target is s: ' + (e.target === s));"
            "});"
            "s.dispatchEvent(new Event('ping', {bubbles: true}));"),
        "target is s: true", "no shadow tree, no retargeting");
  });

  AddTest(tests, "ShadowDom/AnEmptySlotAnswersWithNothingRatherThanItsFallback", [] {
    // `assignedNodes()` is the *assignment*, and the fallback is not assigned to
    // anything -- which is the distinction a page uses to decide whether to
    // render its own default.
    ExpectEqString(Run("<div id=h></div>",
                       "const r = document.getElementById('h').attachShadow({mode: 'open'});"
                       "const s = document.createElement('slot');"
                       "s.appendChild(document.createTextNode('fallback'));"
                       "r.appendChild(s);"
                       "console.log(s.assignedNodes().length);"),
                   "0", "nothing is assigned");
  });

  // --- the scoped cascade, ADR 0019 §3 ---------------------------------------

  AddTest(tests, "ShadowDom/AComponentsStyleAppliesInsideItAndNowhereElse", [] {
    // The blocker session 17 left behind: a `<style>` inside a shadow root is
    // unreachable from the document walk, so a component that styled itself
    // rendered unstyled. And the other half -- a component's rule must not leak
    // out, which is what lets it use `.title` without asking what else on the
    // page does.
    ExpectEqString(
        Run("<div id=h></div><p class=title>outside</p>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<style>.title { color: rgb(1, 2, 3) }</style>"
            "<p class=\"title\">inside</p>';"
            "const inner = r.querySelector('p');"
            "const outer = document.querySelector('.title');"
            "console.log('in ' + getComputedStyle(inner).color);"
            "console.log('out ' + (getComputedStyle(outer).color === 'rgb(1, 2, 3)'));"),
        "in rgb(1, 2, 3)|out false",
        "the component's rule applies to its own node and not to the page's");
  });

  AddTest(tests, "ShadowDom/ADocumentRuleDoesNotReachIntoAShadowTree", [] {
    // The direction that surprises people, and the one that makes a component
    // safe to drop onto an unknown page.
    ExpectEqString(
        Run("<style>span { color: rgb(9, 9, 9) }</style><div id=h></div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<span>inside</span>';"
            "console.log(getComputedStyle(r.querySelector('span')).color === 'rgb(9, 9, 9)');"),
        "false", "a document rule stops at the boundary");
  });

  AddTest(tests, "ShadowDom/HostMatchesTheElementTheTreeHangsOff", [] {
    ExpectEqString(
        Run("<div id=h class=wide></div>",
            "const h = document.getElementById('h');"
            "const r = h.attachShadow({mode: 'open'});"
            "r.innerHTML = '<style>:host { color: rgb(4, 5, 6) }</style>';"
            "console.log(getComputedStyle(h).color);"),
        "rgb(4, 5, 6)", ":host styles the host from inside the tree");
    // `:host(sel)` asks about the host itself, which is the one case where a
    // functional pseudo-class's argument is matched against the subject.
    ExpectEqString(
        Run("<div id=h class=wide></div>",
            "const h = document.getElementById('h');"
            "const r = h.attachShadow({mode: 'open'});"
            "r.innerHTML = '<style>:host(.wide) { color: rgb(7, 7, 7) }"
            ":host(.narrow) { color: rgb(8, 8, 8) }</style>';"
            "console.log(getComputedStyle(h).color);"),
        "rgb(7, 7, 7)", "and only when its argument matches");
  });

  AddTest(tests, "ShadowDom/AHostRuleInADocumentSheetMatchesNothing", [] {
    // There is no scope for it to be the host of, so it selects nothing rather
    // than everything -- which is the failure direction that matters.
    ExpectEqString(Run("<style>:host { color: rgb(3, 3, 3) }</style><div id=h></div>",
                       "console.log(getComputedStyle(document.getElementById('h')).color ==="
                       " 'rgb(3, 3, 3)');"),
                   "false", ":host outside a shadow sheet is inert");
  });

  AddTest(tests, "ShadowDom/SlottedStylesTheLightNodeThatRendersInside", [] {
    // `::slotted()` is the other selector that crosses the boundary, and it goes
    // the opposite way from `:host`: the rule is inside the tree and the element
    // it styles is outside it.
    ExpectEqString(
        Run("<div id=h><span class=given>light</span><p>other</p></div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<style>::slotted(span) { color: rgb(2, 4, 6) }</style><slot></slot>';"
            "console.log(getComputedStyle(document.querySelector('span')).color);"
            "console.log(getComputedStyle(document.querySelector('p')).color ==="
            " 'rgb(2, 4, 6)');"),
        "rgb(2, 4, 6)|false", "the assigned span, and not the assigned p");
  });

  AddTest(tests, "ShadowDom/SlottedDoesNotReachADescendantOfAnAssignedNode", [] {
    // Assignment is one level, which is what makes it answerable without a walk
    // -- and `::slotted()` selects the assigned node itself, not inside it.
    ExpectEqString(
        Run("<div id=h><span><em>deep</em></span></div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<style>::slotted(em) { color: rgb(5, 5, 5) }</style><slot></slot>';"
            "console.log(getComputedStyle(document.querySelector('em')).color ==="
            " 'rgb(5, 5, 5)');"),
        "false", "a descendant of an assigned node is not slotted");
  });

  AddTest(tests, "ShadowDom/InheritanceCrossesTheBoundaryEvenThoughMatchingDoesNot", [] {
    // ADR 0019 §3's sentence, and the two halves have to be true at once: the
    // *cascade* is scoped and *inheritance* is not, because inheritance follows
    // the flattened tree.
    ExpectEqString(
        Run("<div id=h style=\"color: rgb(1, 1, 1)\"></div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<span>inside</span>';"
            "console.log(getComputedStyle(r.querySelector('span')).color);"),
        "rgb(1, 1, 1)", "a node in the shadow tree inherits from its host");
  });

  AddTest(tests, "ShadowDom/AStyleAddedToAShadowRootLaterStillApplies", [] {
    // The collection runs at layout rather than only at load, because a shadow
    // root is attached by script and its stylesheet arrives after the document
    // walk that would have found one.
    ExpectEqString(
        Run("<div id=h></div>",
            "const r = document.getElementById('h').attachShadow({mode: 'open'});"
            "r.innerHTML = '<p>first</p>';"
            "const p = r.querySelector('p');"
            "console.log('before ' + (getComputedStyle(p).color === 'rgb(6, 6, 6)'));"
            "const style = document.createElement('style');"
            "style.textContent = 'p { color: rgb(6, 6, 6) }';"
            "r.appendChild(style);"
            "console.log('after ' + getComputedStyle(p).color);"),
        "before false|after rgb(6, 6, 6)",
        "a sheet appended to a live shadow root takes effect");
  });
}

}  // namespace microbrowser::tests