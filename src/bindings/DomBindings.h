#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dom/Node.h"
#include "js/Interpreter.h"

namespace microbrowser::bindings {

// Gives a script a document to act on.
//
// The only place in the tree that sees both `js` and `dom`, which is what
// makes it the only path from a page's code to its tree -- and therefore the
// one place a same-origin check has to live. See
// docs/adr/0008-dom-bindings.md.
//
// A node is handed to script as a JavaScript object holding a raw `dom::Node*`
// into a tree this class does not own. That is safe only while nothing frees a
// node before its document does; `dom::Node::Remove` would, and nothing calls
// it. Binding `removeChild` is the change that breaks it, which is why it is
// not bound.
class DomBindings {
 public:
  // `document` outlives the bindings, and the interpreter outlives the script
  // that runs in it. Both are references rather than owned, because the engine
  // owns them and a second owner is a second lifetime to get wrong.
  // `url` is the document's address, which `location` reports. Passed in
  // rather than read from anywhere, because this module cannot see `src/url`
  // and should not: what a URL means is the loader's problem, and all this
  // layer needs is the text a page reads back.
  DomBindings(js::Interpreter& interpreter, dom::Document& document, std::string url = {});

  // Declares `document` in the global scope. Separate from the constructor so
  // that a caller can decide *when* a page's script gains access to its tree,
  // which is a decision the engine will want to make per navigation.
  void Install();

  // The wrapper for a node, made once and cached. Public because the engine
  // will need it to hand an event its target.
  js::Value WrapperFor(dom::Node* node);

  // Runs the `click` handlers on `target` and then on each ancestor, which is
  // what bubbling is. True when one called `preventDefault`.
  //
  // A C++ entry point rather than something script can reach, because the only
  // thing allowed to say a click happened is the thing that saw one. A page
  // that could dispatch its own trusted events could make a form submit itself.
  bool DispatchClick(dom::Element& target);

 private:
  // The first element, in document order, that answers to `matches`.
  dom::Element* FindElement(const std::function<bool(const dom::Element&)>& matches) const;
  void ForEachElement(const std::function<void(dom::Element&)>& visit) const;
  // A new element, owned here until something appends it. A node's owner is
  // its parent, so one without a parent needs somewhere to live -- and the
  // alternative, handing script a node it owns, would put a raw pointer's
  // lifetime in a page's hands.
  js::Value CreateElement(const std::string& tag_name);
  js::Value CreateText(const std::string& text);
  // A comment node. A framework uses one as a placeholder marker far more
  // often than a page author writes one.
  js::Value CreateComment(const std::string& data);
  // A parentless bag of nodes. Inserting it inserts its children -- see
  // InsertNodeBefore, which is where that happens.
  js::Value CreateDocumentFragment();
  // Whether an element answers to one of the three selector forms this layer
  // supports. Shared by querySelector, querySelectorAll, matches and closest,
  // which would otherwise be four chances to disagree about what `.a` means.
  static bool Matches(const dom::Element& element, const std::string& selector);
  void InstallWindow();
  // `URLSearchParams`, in UrlSearchParams.cpp. A collection with no node in it,
  // built on the one urlencoded implementation in `util` -- which is what
  // stops it and the engine's form data set from disagreeing about a byte.
  void InstallUrlSearchParams();
  js::Value MakeClassList(dom::Element& element);
  js::Value MakeStyle(dom::Element& element);
  void InstallEventMethods(const js::Value& wrapper);
  // One event object, with its flags and the two ways to stop it.
  // `trusted` says whether the browser made it or a page did -- a page's own
  // event must not be able to cause what a real one causes.
  js::Value MakeEvent(const std::string& type, bool bubbles, bool cancelable, bool trusted);
  // Runs the listeners for `event`'s type on `target` and, when it bubbles, on
  // each ancestor. True when one called `preventDefault`.
  bool DispatchEventTo(dom::Node& target, const js::Value& event);
  // Runs the listeners in `slot` registered on one object -- a node's wrapper
  // or the window. True when one stopped propagation.
  bool RunListenersOn(const js::Value& holder, const js::Value& event, const std::string& slot);
  // Makes `window` an event target. It is the global object, so this is also
  // what gives `globalThis` the same methods.
  void InstallWindowEvents();
  // `Event`, `CustomEvent` and `MouseEvent`.
  void InstallEventConstructors();
  // One event interface and its constructor, built on first use. `parent` is
  // the interface it extends, or null for Event itself -- which is the one
  // that carries the methods.
  js::Value EventPrototype(const char* name, const char* parent);
  // What `document.createEvent` returns: an event with no type yet, and the
  // `initEvent` that gives it one.
  js::Value CreateLegacyEvent();
  void InstallMutationMethods(const js::Value& wrapper);
  // The interfaces, installed once each onto a prototype rather than once per
  // node onto every wrapper. See NodeInterfaces.cpp and ADR 0012.
  void InstallNodeInterface(const js::Value& target);
  void InstallElementInterface(const js::Value& target);
  // Searching and walking, in ElementQueries.cpp. Three rather than one
  // because the specification's mixins are three: what every Node answers,
  // what a ParentNode (Element or Document) answers, and what only an Element
  // does.
  void InstallNodeQueries(const js::Value& target);
  void InstallParentQueries(const js::Value& target);
  void InstallElementIdentity(const js::Value& target);
  // The prototype a wrapper for `node` gets: the one its tag names, whose
  // chain runs up through HTMLElement, Element and Node. Built on first use.
  js::Value PrototypeFor(const dom::Node& node);
  // Creates the whole chain and declares a constructor for each link, so that
  // `instanceof` answers and `class X extends HTMLElement` can be written.
  void EnsureInterfaces();
  // One named prototype, its parent already built.
  js::Value MakeInterface(const char* name, const js::Value& parent);

  // --- Custom elements, in CustomElements.cpp -------------------------------
  void InstallCustomElements();
  js::Value CustomElementRegistry();
  // The wrapper an upgrade is currently running for, which is what
  // HTMLElement's constructor returns so that `super()` inside a page's class
  // yields the element the document already has. Undefined outside an upgrade,
  // which is when calling an interface directly is the error it should be.
  js::Value PendingUpgrade();
  // Runs `element`'s class over the element the document already holds.
  void UpgradeElement(dom::Element& element);
  // `connectedCallback` and its siblings, when the element is a custom one
  // that has been upgraded and defines the reaction.
  void RunElementReaction(dom::Element& element, const char* callback);
  // --- MutationObserver, in MutationObserver.cpp ----------------------------
  void InstallMutationObserver();
  js::Value ObserverList();
  // Queues one delivery per observer per turn, however many mutations it saw.
  void ScheduleObserverDelivery(const js::Value& observer);
  // Records a mutation against every observer watching `node`. `type` is
  // "childList", "attributes" or "characterData", and is also the option name
  // an observer had to have asked for.
  void RecordMutation(dom::Node& node, const char* type, const std::string& name,
                      const js::Value& old_value, const std::vector<dom::Node*>& added,
                      const std::vector<dom::Node*>& removed);

  void RunAttributeReaction(dom::Element& element, const std::string& name,
                            const js::Value& old_value, const js::Value& new_value);
  // Runs connected or disconnected reactions over `node` and its subtree. The
  // subtree matters: appending a detached tree connects everything in it.
  void NotifyConnection(dom::Node& node, bool connected);
  js::Value AdoptInto(dom::Node& parent, dom::Node* child);
  js::Value InsertNodeBefore(dom::Node& parent, dom::Node* child, dom::Node* reference);
  // Detaches `child` and keeps it alive for the life of the document.
  //
  // This is the whole reason removal was not in the first slice. A wrapper
  // holds a raw `dom::Node*`, so a node freed while script still refers to it
  // is a use-after-free reachable from a page. Keeping it instead is the
  // second of the two fixes ADR 0008 names -- it leaks a removed subtree until
  // navigation, which for a browser that navigates away from a page is a
  // bounded leak rather than an unbounded one.
  bool DetachFromTree(dom::Node& child);
  void ClearChildren(dom::Node& parent);
  js::Value AdoptClone(std::unique_ptr<dom::Node> clone);
  js::Value AppendTextTo(dom::Node& parent, const std::string& text);

  js::Interpreter* interpreter_;
  dom::Document* document_;
  std::string url_;
  // The cache from node to wrapper, as a JavaScript object rather than a C++
  // table: a table of `Object*` would have to be a GC root, and the
  // interpreter has no API for a third party to add one. This is reachable
  // from `document`, so the collector already sees it.
  js::Value wrappers_;
  // The prototypes, by interface name. A JavaScript object for the same reason
  // the wrapper cache is one: a C++ table of `Object*` would have to be a GC
  // root and there is no API to add one. Hung off the global, which already is.
  js::Value interfaces_;
  // Nodes made by `createElement` and not yet appended. Emptied into the tree
  // as each is adopted; whatever is left is freed with this object, which is
  // why a wrapper for one of them must not outlive the bindings.
  std::vector<std::unique_ptr<dom::Node>> unattached_;
  // Nodes script removed. Held rather than freed, for the reason on
  // DetachFromTree.
  std::vector<std::unique_ptr<dom::Node>> detached_;
};

}  // namespace microbrowser::bindings
