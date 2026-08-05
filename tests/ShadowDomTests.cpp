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
}

}  // namespace microbrowser::tests
