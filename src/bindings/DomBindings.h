#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "bindings/Geometry.h"
#include "bindings/History.h"
#include "bindings/Canvas.h"
#include "bindings/Workers.h"
#include "bindings/IndexedDb.h"
#include "bindings/Media.h"
#include "bindings/Network.h"
#include "bindings/Cookies.h"
#include "bindings/Sockets.h"
#include "bindings/Storage.h"
#include "bindings/Waapi.h"
#include "dom/Node.h"
#include "js/Interpreter.h"
#include "js/StructuredClone.h"

namespace microbrowser::css {
struct Selector;
}

namespace microbrowser::bindings {

// A form submission a script asked for and has not had yet.
//
// Recorded rather than performed, for two reasons and the second is the one
// that matters. This module cannot navigate: it cannot see a URL, a loader or
// a network, which is the module contract working. And a navigation started
// from inside a running script would tear down the interpreter that is running
// it -- ADR 0026 §3 makes document teardown the most safety-critical routine in
// the engine, and "not while script is on the stack" is the first rule of it.
// So the engine takes this after the turn ends.
struct PendingSubmit {
  dom::Element* form = nullptr;
  // The button that submitted, or null. It decides `formaction`, `formmethod`
  // and which submit control appears in the form data set.
  dom::Element* submitter = nullptr;
};

// One pointer act, as the thing that saw it describes it. The coordinates are
// CSS pixels: `client` is measured from the viewport and `page` from the top of
// the document, and they differ by the scroll offset -- which is why both are
// here rather than one plus a subtraction a caller might forget.
struct PointerInput {
  float client_x = 0.0f;
  float client_y = 0.0f;
  float page_x = 0.0f;
  float page_y = 0.0f;
  // The DOM's numbering: 0 is the primary button, and `buttons` is the bitmask
  // of what is still held.
  std::uint8_t button = 0;
  std::uint16_t buttons = 0;
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;
};

// One key press or release, as the thing that saw it describes it.
//
// Three strings rather than one, and ADR 0017 §1 is where the reasoning is: a
// game reads `code` because WASD is a shape on the keyboard, a shortcut reads
// `key` because Ctrl+C is a letter, and an editor reads `text` because a dead
// key produces nothing until the next one. This struct is deliberately not
// `ipc::KeyInputMessage`: this module cannot see `ipc`, and the engine
// translating one into the other at the seam is what keeps it that way.
struct KeyInput {
  bool down = true;
  std::string code;
  std::string key;
  std::string text;
  bool control = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;
  bool repeat = false;
};

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
  // `geometry` is where the layout questions go, or null when the caller has
  // no layout to answer them -- a test with a bare document, for now. Null is
  // an *absence*: `getBoundingClientRect` and `getComputedStyle` are then not
  // declared at all, rather than declared and answering zero. See ADR 0012 --
  // a page that feature-detects a name and finds it walks into the wall behind
  // it, where a missing name sends it to a polyfill that works.
  // `network` is where a page's own requests go, or null when this binding
  // layer has no loader behind it. Null is an *absence* for the reason a null
  // `geometry` is: `fetch` is then not declared at all, rather than declared
  // and always rejecting. See ADR 0012 and Network.h.
  // `storage` is where `sessionStorage` and `localStorage` are answered, or null when
  // there is no store behind them -- and then **neither name is declared**, for the
  // reason a null `network` leaves `fetch` undeclared. A page that feature-detects
  // `window.localStorage` and finds a store that throws on every write is worse off
  // than one that finds nothing: ADR 0012, and ADR 0021 §6 says the same about
  // `navigator.storage.persist()`.
  // `indexed_db` is where `indexedDB` is answered, or null when there is no store
  // behind it -- and then, like `storage`, **the name is not declared**. ADR 0038.
  DomBindings(js::Interpreter& interpreter, dom::Document& document, std::string url = {},
              GeometrySource* geometry = nullptr, NetworkSource* network = nullptr,
              HistorySource* history = nullptr, StorageSource* storage = nullptr,
              CookieSource* cookies = nullptr, SocketSource* sockets = nullptr,
              MediaController* media = nullptr, CanvasSurface* canvas = nullptr,
              WorkerHost* workers = nullptr, IndexedDbSource* indexed_db = nullptr,
              AnimationSource* animations = nullptr);

  // Declares `document` in the global scope. Separate from the constructor so
  // that a caller can decide *when* a page's script gains access to its tree,
  // which is a decision the engine will want to make per navigation.
  void Install();

  dom::Document& Document() { return *document_; }

  // The wrapper for a node, made once and cached. Public because the engine
  // will need it to hand an event its target.
  js::Value WrapperFor(dom::Node* node);

  // Runs the `click` handlers on `target` and then on each ancestor, which is
  // what bubbling is. True when one called `preventDefault`.
  //
  // A C++ entry point rather than something script can reach, because the only
  // thing allowed to say a click happened is the thing that saw one. A page
  // that could dispatch its own trusted events could make a form submit itself.
  bool DispatchClick(dom::Element& target, const PointerInput& pointer);
  // Trusted `pointerdown`/`pointerup`/`mousedown`/`mouseup`, synthesized from
  // a real `PointerInputMessage`. youtube's player listens on `pointerdown`
  // rather than `click`, which is why this exists beside DispatchClick.
  bool DispatchPointerMouse(dom::Element& target, std::string_view type,
                            const PointerInput& pointer);

  // A socket's four events, from the engine. C++ entry points for the reason
  // DispatchClick is one: the only thing allowed to say a message arrived is the thing
  // that read it off the wire. True when a handler ran, which is the caller's signal that
  // the document may have changed.
  bool DeliverSocketOpen(std::uint64_t id);
  bool DeliverSocketMessage(std::uint64_t id, const std::string& data, bool text);
  bool DeliverSocketClose(std::uint64_t id, std::uint16_t code, const std::string& reason,
                          bool clean, bool failed);
  // An `EventSource`'s three events. `permanent` on the error says whether a reconnect
  // follows, which is what lets a page distinguish "reconnecting" from "gave up".
  // A media event at an element, from the state machine that saw the transition. Trusted, and
  // therefore a C++ entry point: a page that could fire `canplay` at its own element could make
  // a player believe data arrived.
  bool DispatchMediaEvent(dom::Element& element, const std::string& type);

  bool DeliverEventSourceOpen(std::uint64_t id);
  bool DeliverEventSourceMessage(std::uint64_t id, const std::string& type,
                                 const std::string& data, const std::string& last_id);
  bool DeliverEventSourceError(std::uint64_t id, bool permanent);

  // Settles `Animation.finished` for programmatic effects that completed or
  // were cancelled since the last take. True when any promise settled.
  bool DeliverFinishedAnimations();

  // Fires `submit` at `form`. True when a handler called `preventDefault`,
  // which is the caller's signal not to submit.
  //
  // A C++ entry point for the same reason DispatchClick is: the browser
  // dispatching an event as part of an algorithm is what makes it trusted, and
  // a page must not be able to forge one. What a page *can* do is ask for the
  // algorithm -- `requestSubmit()` -- which runs this on its way through.
  bool DispatchSubmit(dom::Element& form);

  // Fires `scroll` at `target`, or at the document and the window when it is
  // null. True when something was listening. A C++ entry point for the reason
  // the two above are: the browser is the only thing that knows a scroll
  // happened, and a page that could forge one could make a lazy-loading feed
  // fetch its whole backlog. See ADR 0018 §3.
  bool DispatchScroll(dom::Element* target);

  // Fires `keydown` or `keyup` at `target`, or at the document's body when it
  // is null. True when a handler called `preventDefault`, which is the caller's
  // signal not to run the key's default action -- inserting the character,
  // submitting the form. The action is a step *after* dispatch and never during
  // it: ADR 0017 §2.
  bool DispatchKey(dom::Node* target, const KeyInput& key);

  // Moves focus to `target`, or clears it when null, and fires the four events
  // that go with the move. True when it actually moved, which is the caller's
  // signal that handlers ran and the document may have changed.
  //
  // The one focus-change algorithm, reached from both sides: `element.focus()`
  // calls it, and so do the engine's click and Tab. Two ways to change focus is
  // how `document.activeElement` ends up disagreeing with where the next
  // keystroke goes. See ADR 0017 §4 and FocusBindings.cpp.
  //
  // `visible` is the `:focus-visible` heuristic -- true when the keyboard moved
  // focus, false when a pointer or a script did.
  bool MoveFocus(dom::Element* target, bool visible);
  // The document's focused element, or null. Public because the engine routes
  // every key to it and hit-tests only for pointer events (ADR 0017 §4).
  dom::Element* FocusedElement() const;

  // The submission a script asked for, taken. Empty when it asked for none.
  std::optional<PendingSubmit> TakePendingSubmit();

  // Samples every `IntersectionObserver` and `ResizeObserver` against the
  // layout about to be painted and runs the callbacks whose answers changed.
  // True when one ran, which is the caller's signal that the document may have
  // moved under it.
  //
  // A C++ entry point for the reason DispatchScroll is: the browser is the only
  // thing that knows a frame happened, and an observer a page could sample on
  // demand would be one it could make fire from inside its own scroll handler.
  // `time_ms` is the page's own origin-relative clock, the same one an
  // animation frame is stamped with. See ADR 0018 §5 and ViewObservers.cpp.
  bool DeliverViewObservations(double time_ms);

  // Fires `change` at every `matchMedia` list whose answer has moved since the
  // last frame. Public and per-frame for the reason above: the browser is the
  // only thing that knows the viewport changed, and re-evaluating on demand
  // would let a page fire its own resize handlers.
  bool DeliverMediaQueryChanges();

  // The document lifecycle. `readyState` moves loading -> interactive ->
  // complete, and the two events fire on the transitions rather than being
  // announced separately: a page that hears `DOMContentLoaded` and then reads
  // `readyState` must not be told the parse is still going.
  //
  // Both return whether anything was listening, which is the caller's signal
  // that the document may have changed and needs laying out again. A page with
  // no `load` handler must not cost a relayout for having been loaded.
  bool NotifyDomContentLoaded();
  bool NotifyLoad();

  // Settles the promise `fetch` handed out for request `id`, and runs the
  // microtasks that answer queues. False when nothing was waiting -- an
  // aborted request, or a second delivery -- which the caller drops.
  //
  // A C++ entry point for the reason DispatchClick is: the only thing allowed
  // to say a response arrived is the thing that received one. `response` is
  // already whatever this document may see; nothing here can widen it. See
  // Network.h and FetchBindings.cpp.
  bool DeliverFetchResponse(std::uint64_t id, const ScriptResponse& response);

  // Fires `popstate` at the window, carrying whatever state the entry now
  // current holds. A C++ entry point for the reason DispatchScroll is: the
  // browser is the only thing that knows a traversal happened, and a page that
  // could forge one could make a router believe the user pressed Back.
  //
  // Never on the initial load -- ADR 0026 §2 -- which is the caller's rule
  // because only the caller knows whether a document is new.
  // Moves the address this layer answers with, for a same-document navigation.
  // The `location` object is rewritten in place rather than replaced, because a
  // page holds a reference to it.
  void SetDocumentUrl(std::string url);

  // Fires `slotchange` at every slot whose assignment changed since the last
  // time this was asked. A C++ entry point for the reason DispatchScroll is: the
  // browser is the only thing that knows the tree moved, and it is called at the
  // frame -- one place, so a page cannot make it fire from inside its own handler.
  bool DeliverSlotChanges();

  bool DispatchPopState();
  // Fires `hashchange`, for a navigation that changed only the fragment: the one
  // case that has always been able to move the URL without a load.
  bool DispatchHashChange(const std::string& old_url, const std::string& new_url);

  // While a script the policy already allowed is running, `<script>` nodes it
  // inserts are trusted for `script-src` without carrying the nonce themselves.
  // reddit's polyfill loader appends its tags this way.
  bool InTrustedScriptContext() const { return trusted_script_depth_ > 0; }
  void PushTrustedScriptContext() { ++trusted_script_depth_; }
  void PopTrustedScriptContext() {
    if (trusted_script_depth_ > 0) {
      --trusted_script_depth_;
    }
  }
  void SetTrustedScriptInsertion(bool trusted) {
    if (trusted) {
      PushTrustedScriptContext();
    } else {
      PopTrustedScriptContext();
    }
  }
  void SetTrustedScriptFlush(std::function<void()> hook) { trusted_script_flush_ = std::move(hook); }
  void SetScriptStrictDynamic(bool enabled) { csp_script_strict_dynamic_ = enabled; }
  void MarkCspTrustedScript(const dom::Element& element) { csp_trusted_scripts_.insert(&element); }
  bool IsCspTrustedScript(const dom::Element& element) const {
    return csp_trusted_scripts_.contains(&element);
  }
  void NotifyScriptElementEvent(const dom::Element& element, const char* type);

  void WireTrustedScriptHooks();

 private:
  // Where an event is on its way through the propagation path. The numbers are
  // the DOM's own, because a page reads them back as `event.eventPhase`.
  enum class EventPhase { None = 0, Capturing = 1, AtTarget = 2, Bubbling = 3 };

  // Searching one tree. `root` is a parameter because `document` is no longer
  // the only document a page can hold -- see DocumentOf in DomBindings.cpp.
  static dom::Element* FindElementIn(dom::Node& root,
                                     const std::function<bool(const dom::Element&)>& matches);
  static void ForEachElementIn(dom::Node& root,
                               const std::function<void(dom::Element&)>& visit);
  void ForEachElement(const std::function<void(dom::Element&)>& visit) const;
  dom::Node* DocumentOf(const js::Value& self) const;
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
  // `document.implementation`, and `Document.prototype` -- where every
  // `document.*` method now lives rather than on the one wrapper.
  void InstallImplementation(const js::Value& document_interface);
  void InstallPageVisibility(const js::Value& document_interface);
  // Whether an element answers to a CSS selector. Shared by querySelector,
  // querySelectorAll, matches and closest, which would otherwise be four
  // chances to disagree. Implemented by `src/css` (see MODULE.deps): the
  // three-form toy this used to be (`#id` / `.class` / exact tag) is how
  // youtube.com's `querySelectorAll("ytd-app,ytd-masthead")` returned nothing
  // while `querySelector("ytd-app")` still worked.
  static bool Matches(const dom::Element& element, const std::string& selector);
  // The same, after the caller has parsed the list once -- querySelectorAll
  // walks every element and must not re-tokenize the selector for each.
  static bool MatchesSelectorList(const dom::Element& element,
                                  const std::vector<css::Selector>& selectors);
  // `scrollTop`/`scrollLeft`, `scrollWidth`/`scrollHeight`, and the three
  // methods that write them. Split out of InstallGeometry because the two
  // halves answer different questions: one measures a box and the other moves
  // one, and only the second can change what is on screen.
  void InstallScroll(const js::Value& element_interface);
  void InstallWindowScroll();
  void InstallWindow();
  // `URLSearchParams`, in UrlSearchParams.cpp. A collection with no node in it,
  // built on the one urlencoded implementation in `util` -- which is what
  // stops it and the engine's form data set from disagreeing about a byte.
  void InstallUrlSearchParams();
  // `document.forms`, `form.elements` with `namedItem`, `submit` and
  // `requestSubmit`, and `control.form`. In FormBindings.cpp.
  void InstallFormApis();
  // The named-and-indexed collection both `document.forms` and `form.elements`
  // are: an array, plus `namedItem` and the names as properties.
  js::Value MakeNamedCollection(const std::vector<dom::Element*>& elements);
  // Files a submission for the engine to act on when the turn ends.
  void RecordSubmit(dom::Element& form, dom::Element* submitter);
  // Fires `type` at the window, which is where a page listens for the events
  // that are about the document rather than about a node. True when something
  // was listening.
  bool DispatchAtWindow(const char* type);
  // The same for an event the caller has already built and put fields on --
  // `popstate` carries a state and `hashchange` carries two URLs, and neither
  // can be added after the listeners have run.
  bool DispatchAtWindowWith(const char* type, const js::Value& event);
  // Puts `composedPath()` on an event, over the path dispatch already built.
  // Stored rather than recomputed, because the path is fixed before any handler
  // runs -- a handler that reparents the target must not change it.
  void InstallComposedPath(const js::Value& event, const std::vector<js::Value>& path);
  void SetReadyState(const char* state);
  js::Value MakeClassList(dom::Element& element);
  js::Value MakeStyle(dom::Element& element);
  // Live `data-*` map. A snapshot cannot accept `el.dataset.version = url`, which
  // is how youtube's player stamps the script URL J14 later compares.
  js::Value MakeDataset(dom::Element& element);
  void InstallEventMethods(const js::Value& wrapper);
  // An `on<type>` handler property as an accessor over a hidden slot, defined
  // once on a shared prototype rather than as per-instance data. See the
  // comment on this method in EventBindings.cpp for why a plain data property
  // fires a handler twice.
  void InstallOnEventAccessor(const js::Value& prototype, const char* name);
  // One event object, with its flags and the two ways to stop it.
  // `trusted` says whether the browser made it or a page did -- a page's own
  // event must not be able to cause what a real one causes.
  js::Value MakeEvent(const std::string& type, bool bubbles, bool cancelable, bool trusted);
  // Runs the listeners for `event`'s type on `target` and, when it bubbles, on
  // each ancestor. True when one called `preventDefault`.
  bool DispatchEventTo(dom::Node& target, const js::Value& event);
  // Runs the listeners in `slot` registered on one object -- a node's wrapper
  // or the window -- for one phase. True when one stopped propagation.
  bool RunListenersOn(const js::Value& holder, const js::Value& event, const std::string& slot,
                      EventPhase phase);
  // The `isTrusted` getter, one per answer for the whole process. A getter and
  // not a field, because ADR 0017 §3 requires there to be no way to set it.
  js::Value TrustedGetter(bool trusted);
  // Makes `window` an event target. It is the global object, so this is also
  // what gives `globalThis` the same methods.
  void InstallWindowEvents();
  // Fills in `location`'s parts from `url_`, and `document.URL` with it. Shared
  // by the install and by a same-document navigation, so the two cannot come to
  // disagree about which parts a page can read.
  void WriteLocationFields(const js::Value& location);
  // `new URL(...)`, over the one parser. See WindowBindings.cpp; installed from InstallObjectUrls
  // because that is what needs it, and idempotent because it may be reached twice.
  void InstallUrlConstructor();
  // --- Focus, in FocusBindings.cpp ------------------------------------------
  // `focus()` and `blur()` on HTMLElement, and `document.activeElement`.
  void InstallFocus(const js::Value& target);
  void InstallActiveElement(const js::Value& document);
  // One of the four focus events. `related` is the other end of the move, which
  // a delegating handler reads to know whether focus came from inside it.
  void DispatchFocusEvent(dom::Element& target, const char* type, bool bubbles,
                          dom::Element* related);
  // `document.body`, which is what `activeElement` reports when nothing is
  // focused -- the answer every engine gives and every page tests against.
  dom::Element* BodyElement() const;
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
  // `data`, `length` and the shared behaviour Text and Comment inherit.
  void InstallCharacterData(const js::Value& target);
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
  // --- IntersectionObserver and ResizeObserver, in ViewObservers.cpp --------
  // Installed only when there is a GeometrySource: an observer with no layout
  // behind it would exist, never fire, and send a feed down the native path
  // into a wall. ADR 0012 -- a stub is worse than an absence.
  void InstallViewObservers();
  js::Value ViewObserverList();
  // Measures one observer's targets and queues the records that changed. It
  // does not call anything: sampling every observer before delivering any is
  // what stops a callback's mutation from changing what the next one saw.
  void SampleViewObserver(const js::Value& observer, double time_ms);
  // Queues one delivery per observer per turn, however many mutations it saw.
  void ScheduleObserverDelivery(const js::Value& observer);
  // Records a mutation against every observer watching `node`. `type` is
  // "childList", "attributes" or "characterData", and is also the option name
  // an observer had to have asked for.
  void RecordMutation(dom::Node& node, const char* type, const std::string& name,
                      const js::Value& old_value, const std::vector<dom::Node*>& added,
                      const std::vector<dom::Node*>& removed);
  // Text/Comment data write, with a characterData mutation record. Polymer's
  // ASAP scheduler (and youtube's lazy-list autofill) depends on observing a
  // detached text node and seeing every `textContent`/`data` bump.
  bool SetCharacterData(dom::Node* node, std::string data);

  // Writing an attribute, once: the old value read first, the write, the
  // custom-element reaction and the mutation record. `setAttribute` and every
  // reflected property go through these, because two implementations are two
  // chances to forget one of the last two. See ReflectedAttributes.cpp.
  void SetElementAttribute(dom::Element& element, const std::string& name,
                           const std::string& value);
  void RemoveElementAttribute(dom::Element& element, const std::string& name);
  // The IDL attributes that reflect content attributes, as get/set pairs on
  // the interface each belongs to. `el.value = 'x'` and `setAttribute('value',
  // 'x')` are the same act; before this they were not.
  void InstallReflections();

  // --- Geometry, in GeometryBindings.cpp ------------------------------------
  // `getBoundingClientRect`, `offsetWidth`/`offsetHeight` and
  // `clientWidth`/`clientHeight` on Element, and `getComputedStyle` on the
  // window. Installed only when there is a GeometrySource, for the reason on
  // the constructor.
  void InstallGeometry(const js::Value& element_interface);
  void InstallComputedStyle();
  // The read-only declaration `getComputedStyle` returns: a Proxy over the
  // element, so a property name nobody enumerated in advance still resolves.
  js::Value MakeComputedStyle(dom::Element& element);

  // --- media, in MediaBindings.cpp -------------------------------------------
  // `HTMLMediaElement`'s methods and properties, installed on the `<video>`/`<audio>`
  // prototype. Absent when there is no controller behind them: a page that finds `play` and
  // gets a promise that never settles has no fallback left.
  void InstallMediaElement(const js::Value& target);
  // --- MSE, in MediaSourceBindings.cpp ---------------------------------------
  //
  // ADR 0028 §3. Installed once per document rather than per element: a `MediaSource` is not an
  // element and reaches one only through `URL.createObjectURL`, which is why the object URL registry
  // is installed from here too.
  // --- canvas, in CanvasBindings.cpp (ADR 0029 §2) ---------------------------
  // --- ADR 0029's answers, in PrivacyAnswers.cpp -----------------------------
  //
  // What a page is told when it asks about the machine. The *values* are in bindings/Fingerprint.h,
  // because a page may sniff several of them and two constants meant to agree eventually do not.
  void InstallPrivacyAnswers(const js::Value& navigator);
  void InstallPermissions(const js::Value& navigator);
  void InstallUserActivation(const js::Value& navigator);
  void InstallClipboard(const js::Value& navigator);
  void InstallNotification();
  void InstallCrypto();
  // `crypto.subtle` subset (AES-CTR encrypt, HMAC-SHA-256 sign). In CryptoSubtle.cpp.
  void InstallSubtleCrypto(const js::Value& crypto);
  // `TextEncoder` / `TextDecoder` (Encoding Standard, UTF-8). In EncodingBindings.cpp; installed
  // from InstallWindow because they are window globals, not navigator answers.
  void InstallTextEncoding();
  // `screen.*` and `devicePixelRatio`, both quantised (ADR 0029 §6). In PrivacyAnswers.cpp with the
  // rest of the table, and installed from InstallWindow because they are window properties.
  void InstallScreenAndPixelRatio();
  // Whether the document has been activated by a real gesture (ADR 0017), which is the gate on a
  // clipboard *write*. Asked through the media controller, which is where the one copy of that bit
  // already lives -- a second copy is how two answers about the same gesture come to disagree.
  bool HasUserActivation() const;
  void SetClipboardText(std::string text) { clipboard_ = std::move(text); }
  const std::string& ClipboardText() const { return clipboard_; }

  // --- workers, in WorkerBindings.cpp (ADR 0022 §1) --------------------------
  void InstallWorker();
  void InstallStructuredClone();
  void RememberWorker(std::uint64_t id, const js::Value& worker);

 public:
  // A message or an error from a worker, delivered to the page's `Worker` object. Public because the
  // engine drains the worker queues on the main loop's turn and hands them here.
  bool DeliverWorkerMessage(std::uint64_t id, const std::string& serialized,
                            const std::string& error, bool is_error);

 private:
  void InstallCanvas(const js::Value& target);
  void InstallImageElement(const js::Value& target);
  js::Value MakeCanvasContext(const js::Value& canvas);
  void InstallImageData(const js::Value& context);
  js::Value MakeImageData(int width, int height, const std::vector<std::uint8_t>& rgba);

  void InstallMediaSource();
  void InstallObjectUrls();
  void InstallBlob();
  bool IsBlobValue(const js::Value& value) const;
  std::string BlobBodyOf(const js::Value& blob) const;
  std::string BlobTypeOf(const js::Value& blob) const;
  void DeliverWindowMessage(const js::Value& data);
  void MaybeCompleteEsmsFeatureDetection();
  js::Value MakeTimeRanges(const std::vector<double>& flat);
  void DeliverSourceBufferEvents(const js::Value& buffer, std::uint64_t id);
  void DeliverMediaSourceEvents(const js::Value& source, std::uint64_t id);
  void RegisterMediaSourceWrapper(std::uint64_t id, const js::Value& wrapper);
  // The wrappers, so that an event the engine produced can be delivered to the object a page is
  // holding. Kept as a JS object hung off the interfaces object rather than as a C++ map of
  // `js::Value` -- a `js::Value` in a C++ field is invisible to the collector, and a MediaSource
  // collected while its element was still attached is a video that stops.
  js::Value MediaSourceWrapper(std::uint64_t id) const;

 public:
  // The engine calls this after attaching a source to an element, which is what fires `sourceopen` --
  // and `sourceopen` is how every player learns it may start appending. Public because the attach
  // happens in the engine, on the far side of the seam.
  bool DeliverMediaSourceOpened(std::uint64_t id);
  // The same, found from the object URL that was just attached -- because the attach site has the URL
  // and not the id, and the id-to-URL map is on the far side of the seam.
  bool DeliverMediaSourceOpenedFor(const std::string& url);

 private:

  // --- WebSocket, in SocketBindings.cpp -------------------------------------
  // Installed only when there is a SocketSource, for ADR 0012's reason and its sharpest
  // case: a page that finds `WebSocket` and gets a constructor that never fires `open`
  // waits forever, where a page that finds nothing falls back to polling and works.
  void InstallWebSocket();
  // `EventSource`, on the same source and installed with it: both are long-lived
  // connections and both are absent when there is nothing behind them.
  void InstallEventSource();
  // The live sockets, as a JavaScript array hung off the interfaces object -- which is
  // already a GC root. A C++ table of `js::Value` would be invisible to the collector.
  js::Value LiveSockets();
  js::Value SocketWithId(std::uint64_t id);
  void ForgetSocket(std::uint64_t id);

  // --- storage, in StorageBindings.cpp --------------------------------------
  // `sessionStorage` and `localStorage`, installed only when there is a
  // StorageSource. One function for both: they differ by a `Kind` and by nothing
  // else that this module can see, which is exactly what ADR 0021's seam is for.
  void InstallStorage();

  // --- IndexedDB, in IndexedDbBindings.cpp and IndexedDbRequests.cpp --------
  // `indexedDB`, installed only when there is an IndexedDbSource, for the reason
  // `sessionStorage` is: a name that exists and refuses every write is worse than
  // no name at all. ADR 0038.
  void InstallIndexedDb();
  // An index's keyPath and uniqueness, remembered here because `src/storage` does
  // not carry one (it may not see `js`, so it cannot extract a key from a value)
  // and the engine's seam has no getter for it -- only this binding ever asked to
  // create the index, so only this binding needs to remember what it asked for.
  // Kept as a JavaScript object hung off the interfaces object rather than a C++
  // table, for the reason every other piece of cross-call state here is: the
  // collector can see a property and cannot see a value in a field.
  js::Value IdbIndexMetaTable();
  void RememberIdbIndexMeta(const std::string& db, const std::string& store,
                           const std::string& index, const IndexedDbKeyPath& key_path,
                           bool unique);
  // Nothing when this index was never created, or was created with no keyPath.
  std::optional<IndexedDbKeyPath> IdbIndexKeyPath(const std::string& db, const std::string& store,
                                                  const std::string& index);
  bool IdbIndexIsUnique(const std::string& db, const std::string& store,
                        const std::string& index);
  // Schedules `request`'s `success` (or `error`, for `DeliverIdbError`) as a
  // macrotask -- `TimerQueue::QueueTask`, exactly like a `MessagePort`'s delivery
  // and for the same reason: a page's own transaction-completion promise must
  // actually cross a turn, not settle inside the call that started it.
  void DeliverIdbSuccess(const js::Value& request, const js::Value& result);
  void DeliverIdbError(const js::Value& request, const std::string& name,
                       const std::string& message);
  // Fires `type` on `request` (running its `onX` handler and then any
  // listener), then tells the owning transaction one of its requests
  // finished. The shared tail of `DeliverIdbSuccess` and `DeliverIdbError`,
  // which differ only in which field they set first and which names they
  // pass here. A private member rather than a free function for the reason
  // every other helper below it is: it calls `InterfaceNamed` and
  // `MaybeCompleteIdbTransaction`, and a free function gets no access to
  // either just because its only caller has some.
  void DeliverIdbEvent(const js::Value& request, const char* type, const char* handler_name);
  // Decrements `transaction`'s pending-request count and fires `complete` when it
  // reaches zero. Called at the tail of every request's own delivery, which is
  // late enough: a handler that starts another request synchronously has already
  // incremented the count again by the time this runs.
  void MaybeCompleteIdbTransaction(const js::Value& transaction);
  // A new `IDBRequest` (or `IDBOpenDBRequest`), entangled with `transaction` --
  // undefined for `indexedDB.open()`, since that request is not part of one.
  // Shared by both IndexedDB translation units because every operation in
  // either one starts by making a request.
  js::Value MakeIdbRequest(const char* interface_name, const js::Value& source,
                           const js::Value& transaction);
  // `IDBIndex`, bound to `db`/`store`/`index`/`transaction`. Called from both
  // `IDBObjectStore.createIndex` and `.index` here, and from
  // IndexedDbRequests.cpp where the index's own methods (`get`, `getAll`,
  // `openCursor`) are installed -- declared once because both files make one.
  js::Value MakeIdbIndex(const std::string& db, const std::string& store,
                         const std::string& index, const js::Value& transaction);
  // An `IDBObjectStore` wrapper bound to `db`/`store`/`transaction`. Every
  // call to `IDBTransaction.objectStore` or `IDBDatabase.createObjectStore`
  // makes a fresh one rather than caching it -- a store handed out by one
  // transaction is not valid on another.
  js::Value MakeIdbObjectStore(const std::string& db, const std::string& store,
                              const js::Value& transaction);
  // An `IDBTransaction` for `db` covering `store_names` in `mode`. Shared by
  // `IDBDatabase.transaction` and the versionchange transaction an open
  // request exposes during `upgradeneeded` -- youtube's EntityStore refuses
  // the whole open if `IDBOpenDBRequest.transaction` is null/undefined there
  // (`new v_(a.transaction)` then reads `addEventListener` off undefined).
  js::Value MakeIdbTransaction(const js::Value& database, const std::string& db,
                               const std::vector<std::string>& store_names,
                               const std::string& mode);
  // A fresh `IDBCursor` (or `IDBCursorWithValue`) over `db`/`store`, filtered
  // by `index` (empty for the store's own primary key) and `only`. Delivers
  // as `request`'s result, positioned on the first matching entry or `null`
  // when there is none -- `openCursor` never errors just because nothing
  // matched. In IndexedDbRequests.cpp.
  void OpenIdbCursor(IndexedDbSource& source, const std::string& db, const std::string& store,
                     const std::string& index, const js::Value& only, const js::Value& transaction,
                     const js::Value& request, bool with_value);
  // `IDBIndex`, `IDBKeyRange` and the two cursor interfaces, in
  // IndexedDbRequests.cpp -- split from InstallIndexedDb once that function's
  // file reached the module's line cap.
  void InstallIndexedDbCursors();

  // --- fetch, in FetchBindings.cpp and FetchTypes.cpp -----------------------
  // Installed only when there is a NetworkSource, for the reason the geometry
  // bindings are installed only when there is a GeometrySource: a `fetch` that
  // always rejected is worse than no `fetch` at all (ADR 0012).
  void InstallFetch();
  void InstallHeaders();
  void InstallResponse();
  void InstallReadableStream();
  void InstallRequest();
  void InstallAbortController();
  // The requests in flight, as a JavaScript array hung off the interfaces
  // object -- which is already a GC root. A C++ table of promises would be
  // invisible to the collector, which is the bug this module has had once.
  js::Value PendingFetches();
  // A `Headers` and a `Response`, built from what the network half answered.
  js::Value MakeHeaders(const std::vector<ScriptHeader>& fields);
  js::Value MakeResponse(const ScriptResponse& response);
  // Marks `signal` aborted, rejects every fetch waiting on it, cancels those at
  // the network, and fires `abort`. In that order, because a handler reads all
  // three.
  void AbortSignalled(const js::Value& signal, const js::Value& reason);

  // --- history, in HistoryBindings.cpp --------------------------------------
  // `window.history`. Installed only when there is a HistorySource, for the
  // reason `fetch` is: a page that finds `pushState` and gets nothing has
  // already taken the branch that assumes it works.
  void InstallHistory();
  // The memoized `history.state`. Deserializing on every read would make
  // `history.state === history.state` false, so the object is cached against a
  // generation counter the engine bumps.
  js::Value HistoryStateValue();
  void InvalidateHistoryState();

  // --- XMLHttpRequest, in XhrBindings.cpp -----------------------------------
  // A shim over the same machinery `fetch` uses -- ADR 0020 §1 is explicit that
  // the older shape is expressed in terms of the newer one, so that a page
  // using either passes the same verdict, the same CORS check and the same
  // `connect-src`. Installed only when there is a NetworkSource, like `fetch`.
  void InstallXhr();
  // Moves `readyState` and fires `readystatechange`. One function because the
  // pair is the whole of what a readyState change *is*, and a caller that moved
  // the number without firing the event would be invisible to every page
  // written before `onload` existed.
  void AdvanceXhrState(const js::Value& xhr, double state);
  void FireXhrEvent(const js::Value& xhr, const char* type);
  // A network failure: DONE, status 0, `error` then `loadend`, and deliberately
  // no reason -- the same rule `fetch` follows, because an error that told a
  // page whether a cross-origin resource existed would be the read CORS exists
  // to prevent.
  void FailXhr(const js::Value& xhr);
  // `abort()`: cancels it at the network and fires the events, and fires
  // nothing at all when the request was never in flight -- which would
  // otherwise run a page's cleanup handler twice.
  void AbortXhr(const js::Value& xhr);
  // One response, into an XHR rather than into a promise. Reached from
  // DeliverFetchResponse, which is the one delivery both kinds share.
  void DeliverToXhr(const js::Value& xhr, const ScriptResponse& response);

  // `MessageChannel`/`MessagePort` in MessageChannels.cpp -- a page's way to
  // the *macrotask* queue, hence TimerQueue::QueueTask. `NodeFilter` and the
  // two cursors in TreeWalkers.cpp. `InterfaceNamed` looks one up.
  void InstallMessageChannel();
  void StartPort(const js::Value& port);
  void DeliverPortMessage(const js::Value& port, const js::SerializedValue& serialized);
  void DispatchPortMessage(const js::Value& port, const js::Value& data);
  // `BroadcastChannel`, in BroadcastChannelBindings.cpp -- every channel of one name
  // this document has opened hears every message any of the others posts. See the
  // note at the top of that file for what "this document" leaves out.
  void InstallBroadcastChannel();
  js::Value LiveBroadcastChannels();
  void DeliverBroadcastMessage(const js::Value& sender, const js::SerializedValue& serialized);
  void DispatchBroadcastMessage(const js::Value& target, const js::SerializedValue& serialized);
  js::Value InterfaceNamed(const char* name);
  js::Value DocumentInterface();
  // `window.matchMedia`, in MediaQueries.cpp. Through the geometry seam,
  // because the evaluator is in `src/css` and because `matchMedia` and
  // `innerWidth` must never disagree. Installed only when there is a
  // GeometrySource, like the rest of that file's surface.
  void InstallMatchMedia();
  void TrackMediaQueryList(const js::Value& list);
  void InstallTreeWalkers(const js::Value& document);
  // `Range`, in Ranges.cpp: two boundary points and the ordering between them.
  void InstallRange();

  // --- HTML from script, in HtmlParsing.cpp ---------------------------------
  // `innerHTML`, `outerHTML` and `insertAdjacentHTML`: a page's string of
  // markup becoming nodes, through the fragment parsing algorithm with a
  // context element. On the Element interface, which is where the
  // specification puts all three.
  void InstallHtmlParsing(const js::Value& element_interface);

  // --- shadow DOM, in ShadowBindings.cpp ------------------------------------
  // `attachShadow`, `shadowRoot`, `assignedSlot` and a slot's assignment. On
  // Element because that is where the specification puts them. ADR 0019 §1-2.
  void InstallShadowDom(const js::Value& element_interface);
  // `CSSStyleSheet`, `replaceSync`, and `adoptedStyleSheets` on Document and
  // ShadowRoot. ADR 0019 §4.
  void InstallConstructableStylesheets(const js::Value& document_interface,
                                       const js::Value& shadow_root_interface);
  // `window.CSS` with `supports` (and `escape`) — same answers `@supports`
  // uses, so a page probing via script cannot disagree with a stylesheet.
  // Lives next to constructable sheets because both are the CSSOM surface.
  void InstallCssOm();
  // `Element.animate` / `Animation` — only when `animations_` is set (TD-0021).
  void InstallWaapi(const js::Value& element_interface);
  // Parses `markup` with `context_tag_name` as the fragment parsing
  // algorithm's context element and inserts what it produced into `parent`
  // before `reference`. The one place a page's string becomes tree, so that
  // "was the parser given the right context" is a question with one answer.
  void InsertParsedHtml(std::string_view context_tag_name, dom::Node& parent,
                        dom::Node* reference, const std::string& markup);
  // The same, with the context taken from `parent` -- an element's tag name, or
  // `body` when the parent is a document or a fragment and has none.
  void InsertAdjacentParsedHtml(dom::Node& parent, dom::Node* reference,
                                const std::string& markup);
  // `outerHTML`: the parsed nodes in, then `target` out, in that order.
  void ReplaceWithParsedHtml(dom::Node& parent, dom::Node& target, const std::string& markup);
  // Moves every child of `fragment` into `parent` before `reference` as one
  // operation: one childList record for the batch, and an upgrade and a
  // connection reaction for each node that arrived. Shared with `appendChild`
  // of a DocumentFragment, which is the same insertion by another name.
  void InsertFragmentChildren(dom::Node& parent, dom::Node& fragment, dom::Node* reference);
  // Upgrades every custom element in a subtree. The parser makes elements and
  // knows nothing about a registry, so a subtree that arrived through the
  // parser has to be walked once before it is announced as connected.
  void UpgradeSubtree(dom::Node& node);

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
  // The submission a script asked for. See PendingSubmit for why it waits.
  std::optional<PendingSubmit> pending_submit_;
  // Borrowed, like the interpreter and the document, and null when there is no
  // layout behind this binding layer.
  GeometrySource* geometry_ = nullptr;
  // The same, for a page's own requests, and null when there is no loader
  // behind this binding layer -- in which case `fetch` is not declared.
  NetworkSource* network_ = nullptr;
  // Borrowed and null when there is no history behind this layer, in which case
  // `history` is not declared at all. Same rule as the two above.
  HistorySource* history_ = nullptr;
  StorageSource* storage_ = nullptr;
  // ADR 0038. Borrowed and null when there is no store behind this layer, in
  // which case `indexedDB` is not declared at all -- the same rule `storage_` follows.
  IndexedDbSource* indexed_db_ = nullptr;
  CookieSource* cookies_ = nullptr;
  SocketSource* sockets_ = nullptr;
  MediaController* media_ = nullptr;
  CanvasSurface* canvas_ = nullptr;
  WorkerHost* workers_ = nullptr;
  // TD-0021 / Web Animations. Null leaves `Element.animate` undeclared so a
  // polyfill that writes `el.style` every frame is not preferred over nothing
  // when the engine has no clock behind the name (ADR 0012).
  AnimationSource* animations_ = nullptr;
  // What a page last wrote to the clipboard. Held here rather than handed to the system, because
  // reaching the platform clipboard from the binding layer would be a module boundary crossed for one
  // string -- and a test needs to see what was written either way. The chrome takes it from here.
  std::string clipboard_;
  std::uint32_t trusted_script_depth_ = 0;
  bool csp_script_strict_dynamic_ = false;
  std::function<void()> trusted_script_flush_;
  std::unordered_set<const dom::Element*> csp_trusted_scripts_;
};

}  // namespace microbrowser::bindings
