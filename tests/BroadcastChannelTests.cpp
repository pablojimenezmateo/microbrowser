// `BroadcastChannel`.
//
// ADR 0038. youtube's Woffle offline store syncs its entity cache across tabs with one
// named channel, and `plI` only constructs the PES encoder after both this and
// `indexedDB` pass feature detection. The assertions worth making beyond "the name
// exists" are the ones DomBindingsTests.cpp makes for MessageChannel and for the same
// reasons: delivery is a task rather than a microtask, the message is a structured
// clone rather than an alias, a channel does not hear its own post, and a closed
// channel stops receiving.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
#include "html/TreeBuilder.h"
#include "js/Interpreter.h"

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
  bound.dom_bindings = std::make_unique<bindings::DomBindings>(
      *bound.interpreter, *bound.document, std::string("https://example.org/"));
  bound.dom_bindings->Install();
  return bound;
}

}  // namespace

void RegisterBroadcastChannelTests(std::vector<TestCase>& tests) {
  AddTest(tests, "BroadcastChannel/TheYpsShapedFeatureDetectPasses", [] {
    Bound bound = Bind("<html><body></body></html>");
    ExpectEqString(
        js::ToString(bound.interpreter
                         ->Run("typeof BroadcastChannel + ' ' + typeof BroadcastChannel.prototype")
                         .value),
        "function object", "the constructor and its prototype both exist");
  });

  AddTest(tests, "BroadcastChannel/IsAnEventTargetConstructedWithAName", [] {
    Bound bound = Bind("<html><body></body></html>");
    ExpectEqString(
        js::ToString(bound.interpreter
                         ->Run("const c = new BroadcastChannel('x');"
                               "(c instanceof BroadcastChannel) + ' ' + c.name + ' ' + "
                               "typeof c.addEventListener + ' ' + typeof c.postMessage")
                         .value),
        "true x function function", "a real instance, over a real EventTarget");
  });

  AddTest(tests, "BroadcastChannel/PostMessageDeliversAsATaskNotAMicrotask", [] {
    // Same property MessageChannel's own test asserts, for the same reason: a
    // page uses one of these to yield across a macrotask boundary, and a
    // microtask here would make that scheduling promise false.
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    bound.interpreter->Run(
        "globalThis.order = [];"
        "const a = new BroadcastChannel('sync');"
        "const b = new BroadcastChannel('sync');"
        "b.onmessage = e => order.push('msg:' + e.data.n);"
        "const o = {n: 7};"
        "a.postMessage(o);"
        "o.n = 99;"
        "Promise.resolve().then(() => order.push('microtask'));"
        "order.push('sync');");
    bound.interpreter->DrainMicrotasks();
    ExpectEqString(js::ToString(bound.interpreter->Run("order.join(',')").value),
                   "sync,microtask", "the microtask ran and the message did not");
    Expect(timers.RunDue(*bound.interpreter, 0), "the delivery is a due task");
    ExpectEqString(js::ToString(bound.interpreter->Run("order.join(',')").value),
                   "sync,microtask,msg:7",
                   "and it arrives after both, with the value taken at post time");
  });

  AddTest(tests, "BroadcastChannel/EveryOtherChannelOfTheSameNameHearsItAndNoOthersDo", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    bound.interpreter->Run(
        "globalThis.heard = [];"
        "const a1 = new BroadcastChannel('room');"
        "const a2 = new BroadcastChannel('room');"
        "const a3 = new BroadcastChannel('room');"
        "const other = new BroadcastChannel('elsewhere');"
        "a1.onmessage = () => heard.push('a1');"
        "a2.onmessage = () => heard.push('a2');"
        "a3.onmessage = () => heard.push('a3');"
        "other.onmessage = () => heard.push('other');"
        "a1.postMessage('hi');");
    timers.RunDue(*bound.interpreter, 0);
    // `a1` posted, so it does not hear its own message -- the specified rule,
    // and the one a naive "fan out to everyone with this name" would miss.
    // `other` is a different name, so it hears nothing either.
    ExpectEqString(js::ToString(bound.interpreter->Run("heard.slice().sort().join(',')").value),
                   "a2,a3", "the two other same-named channels heard it and nothing else did");
  });

  AddTest(tests, "BroadcastChannel/TheDeliveredEventIsARealMessageEvent", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    bound.interpreter->Run(
        "globalThis.kind = '';"
        "const a = new BroadcastChannel('n');"
        "const b = new BroadcastChannel('n');"
        "b.onmessage = e => { kind = (e instanceof MessageEvent) + ' ' + e.type; };"
        "a.postMessage(1);");
    timers.RunDue(*bound.interpreter, 0);
    ExpectEqString(js::ToString(bound.interpreter->Run("kind").value), "true message",
                   "a page checks both before trusting the event");
  });

  AddTest(tests, "BroadcastChannel/ClosePreventsFurtherReceiptAndFurtherSending", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    bound.interpreter->Run(
        "globalThis.count = 0;"
        "const a = new BroadcastChannel('c');"
        "const b = new BroadcastChannel('c');"
        "b.onmessage = () => { count++ };"
        "b.close();"
        "a.postMessage(1);");
    timers.RunDue(*bound.interpreter, 0);
    ExpectEqString(js::ToString(bound.interpreter->Run("'' + count").value), "0",
                   "a closed channel does not receive");
    // Posting from a closed channel is the specified failure, not a silent
    // no-op -- a page that closed by mistake should see it.
    ExpectEqString(
        js::ToString(
            bound.interpreter
                ->Run("const c = new BroadcastChannel('c2');"
                      "c.close();"
                      "try { c.postMessage(1); 'no throw' } catch (e) { e.message }")
                .value),
        "BroadcastChannel is closed", "a closed channel refuses to post");
  });

  AddTest(tests, "BroadcastChannel/MessageIsAStructuredCloneRatherThanAnAliasedObject", [] {
    Bound bound = Bind("<body></body>");
    bindings::TimerQueue timers;
    timers.Install(*bound.interpreter, 0);
    bound.interpreter->Run(
        "globalThis.seen = null;"
        "const a = new BroadcastChannel('m');"
        "const b = new BroadcastChannel('m');"
        "b.onmessage = e => { seen = e.data };"
        "const original = {list: [1, 2, 3]};"
        "a.postMessage(original);"
        "original.list.push(4);");
    timers.RunDue(*bound.interpreter, 0);
    ExpectEqString(
        js::ToString(
            bound.interpreter->Run("seen.list.length + ' ' + (seen.list === original.list)").value),
        "3 false", "the receiver's array is its own copy, taken before the later push");
  });

  AddTest(tests, "BroadcastChannel/AFunctionCannotBeCloned", [] {
    Bound bound = Bind("<body></body>");
    ExpectEqString(
        js::ToString(bound.interpreter
                         ->Run("const c = new BroadcastChannel('f');"
                               "try { c.postMessage(() => 1); 'no throw' } catch (e) { e.message }")
                         .value),
        "the message could not be cloned",
        "the specified failure for an unclonable value");
  });
}

}  // namespace microbrowser::tests
