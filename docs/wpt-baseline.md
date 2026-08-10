# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

1911 of 19799 subtests pass (9.7%) over 1105 tests.

**Do not quote that number.** Subtests are not comparable across areas: `shadow-dom/declarative` alone is 39% of every subtest here.
A suite that tests one index table entry per code point counts differently from
one that tests an algorithm. The per-area column is the measurement; the aggregate
is an artefact of how the suite is written.

A test with no subtests at all -- a reftest, or a testharness page whose harness
died before it ran anything -- contributes nothing to that percentage, so the
harness columns below are the ones to read first. A `TIMEOUT` is not a slow test;
it is a page that never reported, which almost always means something threw before
`done()`.

## Per area

| area | tests | ok | error | timeout | crash | subtests | passed | % |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| `console` | 19 | 12 | 0 | 7 | 0 | 29 | 6 | 20.7 |
| `cors` | 27 | 20 | 0 | 7 | 0 | 227 | 36 | 15.9 |
| `dom` | 10 | 10 | 0 | 0 | 0 | 125 | 66 | 52.8 |
| `dom/abort` | 10 | 6 | 0 | 4 | 0 | 37 | 6 | 16.2 |
| `dom/collections` | 10 | 10 | 0 | 0 | 0 | 53 | 7 | 13.2 |
| `dom/events` | 178 | 85 | 1 | 92 | 0 | 552 | 156 | 28.3 |
| `dom/lists` | 5 | 4 | 1 | 0 | 0 | 49 | 30 | 61.2 |
| `dom/nodes` | 327 | 232 | 7 | 87 | 1 | 5158 | 1153 | 22.4 |
| `dom/observable` | 52 | 25 | 0 | 27 | 0 | 244 | 0 | 0.0 |
| `dom/ranges` | 57 | 32 | 24 | 1 | 0 | 242 | 18 | 7.4 |
| `dom/traversal` | 18 | 14 | 3 | 1 | 0 | 55 | 29 | 52.7 |
| `domparsing` | 34 | 21 | 3 | 10 | 0 | 290 | 72 | 24.8 |
| `domparsing/tentative` | 26 | 17 | 0 | 9 | 0 | 905 | 6 | 0.7 |
| `hr-time` | 15 | 8 | 0 | 7 | 0 | 13 | 4 | 30.8 |
| `mimesniff/media` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `mimesniff/mime-types` | 3 | 1 | 0 | 2 | 0 | 1939 | 2 | 0.1 |
| `mimesniff/sniffing` | 3 | 3 | 0 | 0 | 0 | 7 | 3 | 42.9 |
| `png` | 3 | 0 | 0 | 3 | 0 | 1 | 0 | 0.0 |
| `shadow-dom` | 66 | 58 | 0 | 8 | 0 | 728 | 71 | 9.8 |
| `shadow-dom/declarative` | 56 | 40 | 4 | 12 | 0 | 7648 | 5 | 0.1 |
| `shadow-dom/focus` | 37 | 31 | 1 | 5 | 0 | 78 | 5 | 6.4 |
| `shadow-dom/focus-navigation` | 45 | 44 | 0 | 1 | 0 | 89 | 2 | 2.2 |
| `shadow-dom/leaktests` | 4 | 3 | 0 | 1 | 0 | 16 | 6 | 37.5 |
| `shadow-dom/reference-target` | 15 | 14 | 0 | 1 | 0 | 768 | 0 | 0.0 |
| `shadow-dom/untriaged` | 54 | 52 | 0 | 2 | 0 | 248 | 157 | 63.3 |
| `subresource-integrity` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `subresource-integrity/integrity-policy` | 5 | 0 | 0 | 5 | 0 | 37 | 0 | 0.0 |
| `subresource-integrity/signatures` | 15 | 14 | 0 | 1 | 0 | 177 | 43 | 24.3 |
| `subresource-integrity/unencoded-digest` | 9 | 6 | 0 | 3 | 0 | 84 | 28 | 33.3 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 170 | TIMEOUT: the page never reported | `console/console-label-conversion.any.worker.html` |
| 59 | TIMEOUT:  | `dom/abort/abort-signal-any.any.worker.html` |
| 30 | ERROR: TypeError: Illegal constructor: Document | `dom/nodes/Node-compareDocumentPosition.html` |
| 18 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: unexpected token '<' (line N) SyntaxError: unexpected token '<' ... | `dom/nodes/Element-childElement-null-xhtml.xhtml` |
| 7 | TIMEOUT: the page never reported; first script error: ./support/helpers.js: SyntaxError: expected ')' to close a dynamic import (line N) SyntaxError:... | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect-iframe.html` |
| 5 | TIMEOUT: the page never reported; first script error: /trusted-types/support/helper.sub.js: SyntaxError: expected ';' (line N) SyntaxError: expected ... | `domparsing/tentative/stream-html-with-trusted-types-error-in-policy.html` |
| 5 | TIMEOUT: the page never reported; first script error: support.js?pipe=sub: SyntaxError: expected a property name (line N) SyntaxError: expected a pro... | `cors/credentials-flag.htm` |
| 4 | TIMEOUT: killed after the wall-clock budget | `dom/nodes/Node-insertBefore.html` |
| 3 | ERROR: TypeError: undefined (createDocument) is not a function | `dom/nodes/Document-createAttribute.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'insertRule' of undefined TypeError: cannot r... | `dom/events/webkit-animation-end-event.html` |
| 2 | ERROR: ReferenceError: DOMParser is not defined | `domparsing/DOMParser-parseFromString-html.html` |
| 2 | ERROR: Test named 'Repeated declarative shadow roots keep only the first' specified N 'cleanup' function, and N failed. | `shadow-dom/declarative/declarative-shadow-dom-repeats.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: outside is not defined ReferenceError: outside is not defined... | `shadow-dom/focus/click-focus-delegatesFocus-click.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: scroller is not defined ReferenceError: scroller is not defin... | `dom/events/scrolling/scrollIntoView-in-onscroll-to-sticky.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testN is not defined ReferenceError: testN is not defined at ... | `shadow-dom/event-post-dispatch.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: trustedTypes is not defined ReferenceError: trustedTypes is n... | `domparsing/tentative/positional-methods-with-parser-options.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'customElements' of undefined TypeError: cann... | `dom/nodes/create-element-realm-after-adoption.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createDocument) is not a function TypeError: undefined ... | `dom/nodes/append-on-Document.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createProcessingInstruction) is not a function TypeErro... | `dom/nodes/Node-isEqualNode-xhtml.xhtml` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: [object Object] Error: undefined at get_stack (@N) at AssertionError (@N) at ... | `dom/nodes/Document-createCDATASection.html` |
| 1 | CRASH: killed by signal Segmentation fault | `dom/nodes/moveBefore/relevant-mutations.html` |
| 1 | ERROR: N duplicate test names: "a.classList in undefined namespace should be DOMTokenList.", "area.classList in undefined namespace should be DOMTo... | `dom/lists/DOMTokenList-coverage-for-attributes.html` |
| 1 | ERROR: N duplicate test names: "touchstart listener is passive by default for HTMLElement", "touchstart listener is passive with {passive:undefined... | `dom/events/passive-by-default.html` |
| 1 | ERROR: Test named 'Declarative Shadow DOM: missing shadowrootslotassignment defaults to named' specified N 'cleanup' function, and N failed. | `shadow-dom/declarative/declarative-shadow-dom-slot-assignment.html` |
| 1 | ERROR: Test named 'delegatesFocus shouldn't cause extra focus steps' specified N 'cleanup' function, and N failed. | `shadow-dom/focus/focus-scroll-under-delegatesFocus.html` |
| 1 | ERROR: Test named 'shadowrootslotassignment=manual is serialized and appears before shadowrootclonable and shadowrootserializable' specified N 'cle... | `shadow-dom/declarative/declarative-shadow-dom-slot-assignment-serialization.html` |
| 1 | ERROR: TypeError: undefined (createProcessingInstruction) is not a function | `dom/nodes/CharacterData-remove.html` |
| 1 | ERROR: [object Object] | `domparsing/DOMParser-parseFromString-encoding.html` |
| 1 | TIMEOUT: the page never reported; first script error: /wai-aria/scripts/aria-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expected ';' (... | `shadow-dom/reference-target/tentative/aria-labelledby.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: DocumentTimeline is not defined ReferenceError: DocumentTimel... | `hr-time/raf-coarsened-time.https.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: container is not defined ReferenceError: container is not def... | `shadow-dom/focus/click-focus-slot-ancestor.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: host is not defined ReferenceError: host is not defined at <a... | `dom/events/shadow-relatedTarget.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: targetDiv is not defined ReferenceError: targetDiv is not def... | `dom/events/scrolling/scrollend-event-fired-for-scroll-attr-change.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: targetXDiv is not defined ReferenceError: targetXDiv is not d... | `dom/events/scrolling/scrollend-event-fired-to-element-with-overscroll-behavior.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: expected ')' after a for head (line N) SyntaxError: expected ')'... | `dom/nodes/Document-getElementsByTagName-xhtml.xhtml` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: Illegal constructor: CustomElementRegistry TypeError: Illegal cons... | `domparsing/tentative/stream-html-custom-element-sanitizer.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot set property 'onerror' of undefined TypeError: cannot set p... | `dom/nodes/MutationObserver-cross-realm-callback-report-exception.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (getHTML) is not a function TypeError: undefined (getHTM... | `shadow-dom/declarative/gethtml-ordering.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (parseHTMLUnsafe) is not a function TypeError: undefined... | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-same-document.html` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 48 | 70 | promise_test: Unhandled rejection with value: object "Error: document.elementsFromPoint unsupported" | `dom/events/pointer-event-document-move.html` |
| 32 | 48 | assert_true: Failed to create new rendered document expected true got false | `shadow-dom/untriaged/elements-and-dom-objects/extensions-to-element-interface/methods/test-002.html` |
| 29 | 68 | Test timed out | `dom/events/Event-dispatch-on-disabled-elements.html` |
| 19 | 66 | assert_equals: expected N but got N | `dom/collections/HTMLCollection-live-mutations.window.html` |
| 19 | 48 | assert_throws_js: function "function TypeError() { [native code] }" is not an Error subtype | `dom/events/EventTarget-dispatchEvent.html` |
| 19 | 39 | assert_true: expected true got false | `console/console-is-a-namespace.any.html` |
| 15 | 737 | NOTRUN (no message) | `dom/events/Event-dispatch-on-disabled-elements.html` |
| 15 | 47 | undefined (createDocument) is not a function | `dom/historical.html` |
| 14 | 124 | CROSSDOMAIN is not defined | `cors/client-hint-request-headers-2.tentative.htm` |
| 14 | 136 | DOMParser is not defined | `dom/nodes/Node-normalize.html` |
| 14 | 755 | assert_throws_dom: function "function () { [source unavailable] }" did not throw | `dom/events/EventTarget-dispatchEvent.html` |
| 14 | 95 | promise_test: Unhandled rejection with value: object "ReferenceError: Observable is not defined" | `dom/observable/tentative/crashtests/observable-gc.any.html` |
| 14 | 14 | promise_test: Unhandled rejection with value: object "ReferenceError: createStylesheetHost is not defined" | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect.html` |
| 13 | 39 | Illegal constructor: EventTarget | `dom/events/AddEventListenerOptions-once.any.html` |
| 13 | 177 | undefined (createProcessingInstruction) is not a function | `dom/events/EventTarget-this-of-listener.html` |
| 12 | 24 | assert_equals: expected (object) null but got (undefined) undefined | `dom/events/Event-constructors.any.html` |
| 12 | 13 | promise_test: Unhandled rejection with value: object "Error: action_sequence() is not implemented by testdriver-vendor.js" | `dom/events/click-on-absolute-pseudo.html` |
| 12 | 100 | undefined (createValueRange) is not a function | `dom/ranges/tentative/OpaqueRange-auto-disconnect.html` |
| 12 | 26 | undefined (getElementById) is not a function | `dom/nodes/DocumentFragment-getElementById.html` |
| 11 | 132 | Observable is not defined | `dom/observable/tentative/observable-catch.any.html` |
| 11 | 26 | assert_throws_js: function "function () { [source unavailable] }" did not throw | `dom/collections/HTMLCollection-as-prototype.html` |
| 11 | 23 | promise_test: Unhandled rejection with value: object "TypeError: undefined (moveBefore) is not a function" | `dom/nodes/moveBefore/Node-moveBefore.html` |
| 10 | 56 | assert_unreached: Should have rejected: undefined Reached unreachable code | `subresource-integrity/signatures/tentative/header-component.window.html` |
| 9 | 48 | assert_equals: Response status is N. expected N but got N | `subresource-integrity/signatures/tentative/header-component.window.html` |
| 9 | 435 | promise_test: Unhandled rejection with value: object "TypeError: undefined is not a function" | `domparsing/tentative/stream-html-custom-element.html` |
| 9 | 16 | undefined (createAttribute) is not a function | `dom/attributes-are-nodes.html` |
| 9 | 15 | undefined (moveBefore) is not a function | `dom/nodes/moveBefore/Node-moveBefore.html` |
| 9 | 4608 | undefined (setHTMLUnsafe) is not a function | `shadow-dom/shadow-root-clonable.html` |
| 8 | 18 | Illegal constructor: Document | `dom/events/Event-dispatch-bubbles-false.html` |
| 8 | 45 | assert_unreached: Script should not fail. Reached unreachable code | `subresource-integrity/signatures/tentative/csp.window.html` |
| 7 | 28 | cannot read property 'document' of undefined | `dom/events/event-global-extra.window.html` |
| 6 | 22 | Illegal constructor: Text | `dom/nodes/adoption.window.html` |
| 6 | 8 | new_parent is not defined | `dom/nodes/moveBefore/fire-focusin-focusout.html` |
| 5 | 9 | HTMLCollection is not defined | `dom/nodes/Document-getElementsByClassName.html` |
| 5 | 36 | assert_equals: expected Document node with N child but got Document node with N children | `dom/nodes/Document-importNode.html` |
| 5 | 9 | assert_false: expected false got true | `cors/preflight-failure.htm` |
| 5 | 5 | promise_test: Unhandled rejection with value: object "Error: '?feature=bidi' is missing when importing testdriver.js but the test is using W... | `console/console-count-logging.html` |
| 5 | 25 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'getElementById' of null" | `shadow-dom/reference-target/tentative/dom-mutation.html` |
| 5 | 5 | promise_test: Unhandled rejection with value: object "TypeError: cannot set property 'name' of undefined" | `console/idlharness.any.html` |
| 5 | 7 | promise_test: Unhandled rejection with value: object "TypeError: undefined (getElementById) is not a function" | `shadow-dom/accesskey.tentative.html` |
| 5 | 28 | promise_test: Unhandled rejection with value: object "TypeError: undefined (setHTMLUnsafe) is not a function" | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-shared.html` |
| 5 | 5 | target is not defined | `dom/events/mouse-event-retarget.html` |
| 5 | 53 | testN is not defined | `shadow-dom/event-composed-path-with-related-target.html` |
| 5 | 283 | undefined is not a function | `dom/nodes/Document-createCDATASection-xhtml.xhtml` |
| 4 | 4 | DocumentType is not defined | `dom/nodes/Document-doctype.html` |
| 4 | 4 | NodeList is not defined | `dom/nodes/Document-getElementsByTagName.html` |
| 4 | 6 | assert_equals: expected "" but got "null" | `dom/nodes/CharacterData-data.html` |
| 4 | 5 | cannot read property 'adoptedStyleSheets' of null | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-basic.html` |
| 4 | 19 | cannot read property 'getElementById' of null | `shadow-dom/reference-target/tentative/commandfor.html` |
| 4 | 10 | cannot read property 'length' of undefined | `dom/nodes/insertion-removing-steps/Node-appendChild-script-with-mutation-observer-takeRecords.html` |
| 4 | 15 | container is not defined | `dom/nodes/moveBefore/moveBefore-id-map.html` |
| 4 | 22 | promise_test: Unhandled rejection with value: object "TypeError: Failed to fetch" | `cors/access-control-expose-headers-parsing.window.html` |
| 4 | 12 | undefined (getAttributeNodeNS) is not a function | `dom/nodes/Attr-prefix-xhtml.xhtml` |
| 4 | 52 | undefined (getElementsByTagNameNS) is not a function | `dom/collections/HTMLCollection-empty-name.html` |
| 4 | 7 | undefined (item) is not a function | `dom/collections/HTMLCollection-supported-property-indices.html` |
| 4 | 10 | undefined (namedItem) is not a function | `dom/collections/HTMLCollection-empty-name.html` |
| 3 | 4 | Illegal constructor: DocumentFragment | `dom/nodes/DocumentFragment-constructor.html` |
| 3 | 1635 | assert_equals: expected (boolean) false but got (undefined) undefined | `shadow-dom/event-composed.html` |
| 3 | 1634 | assert_equals: expected (boolean) true but got (undefined) undefined | `shadow-dom/event-composed.html` |
| 3 | 12 | assert_equals: expected (object) Element node <div></div> but got (undefined) undefined | `dom/events/Event-dispatch-other-document.html` |
| 3 | 4 | assert_equals: expected (object) object "[object Object]" but got (undefined) undefined | `dom/events/event-global.html` |
| 3 | 4 | assert_object_equals: unexpected property "N" | `dom/collections/HTMLCollection-live-mutations.window.html` |
| 3 | 3 | cannot read property 'N' of undefined | `dom/nodes/getElementsByClassName-20.htm` |
| 3 | 20 | cannot read property 'clear' of undefined | `dom/ranges/tentative/OpaqueRange-highlight.html` |
| 3 | 3 | iframe is not defined | `dom/abort/abort-signal-timeout.html` |
| 3 | 17 | promise_rejects_js: function "function TypeError() { [native code] }" is not an Error subtype | `subresource-integrity/signatures/tentative/client-initiated.cross-origin.window.html` |
| 3 | 9 | promise_test: Unhandled rejection with value: object "ReferenceError: getSelection is not defined" | `dom/nodes/moveBefore/selection-preserve.html` |
| 3 | 3 | promise_test: Unhandled rejection with value: object "ReferenceError: host is not defined" | `shadow-dom/focus/text-selection-with-delegatesFocus-on-slotted-content.html` |
| 3 | 3 | promise_test: Unhandled rejection with value: object "TypeError: Illegal constructor: DocumentFragment" | `dom/nodes/insertion-removing-steps/Node-append-meta-referrer-and-script-from-fragment.html` |
| 3 | 30 | promise_test: Unhandled rejection with value: object "TypeError: undefined (createValueRange) is not a function" | `dom/ranges/tentative/OpaqueRange-interactive-overlap-and-selection.html` |
| 3 | 8 | undefined (abort) is not a function | `dom/abort/AbortSignal.any.html` |
| 3 | 20 | undefined (deleteData) is not a function | `dom/nodes/CharacterData-deleteData.html` |
| 3 | 3 | undefined (getElementsByName) is not a function | `dom/nodes/moveBefore/moveBefore-name-map.html` |
| 3 | 383 | undefined (getHTML) is not a function | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-serialization.html` |
| 3 | 22 | undefined (insertData) is not a function | `dom/nodes/CharacterData-insertData.html` |
| 3 | 4 | undefined (intersectsNode) is not a function | `dom/ranges/Range-attribute-nodes.html` |
| 3 | 52 | undefined (replaceData) is not a function | `dom/nodes/CharacterData-replaceData.html` |
| 3 | 8 | undefined is not a constructor | `dom/events/Event-subclasses-constructors.html` |
| 2 | 2 | assert_array_equals: Ascii lowercase input lengths differ, expected array [] length N, got [Element node <test:aU+cN></test:aU+cN>] length N | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 2 | assert_array_equals: actual_targets lengths differ, expected array [] length N, got [object "[object Object]"] length N | `dom/events/Event-dispatch-bubble-canceled.html` |
| 2 | 2 | assert_array_equals: lengths differ, expected array ["sN", "sN", "sN"] length N, got [] length N | `dom/nodes/insertion-removing-steps/Node-appendChild-three-scripts-from-fragment.html` |
| 2 | 2 | assert_array_equals: lengths differ, expected array [Element node <div id="parent" style="display: none"> <input id="ta...] length N, got []... | `dom/events/Event-dispatch-multiple-cancelBubble.html` |
| 2 | 2 | assert_array_equals: lengths differ, expected array [Element node <html class="a"><head> <title>document.getElementsByCla...] length N, got ... | `dom/nodes/getElementsByClassName-03.htm` |
| 2 | 2 | assert_array_equals: lengths differ, expected array [] length N, got [Element node <aU+cN></aU+cN>] length N | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 4 | assert_array_equals: lengths differ, expected array [] length N, got [Element node <st></st>] length N | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 4 | assert_array_equals: lengths differ, expected array [] length N, got [Element node <te:st></te:st>] length N | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 25 | assert_equals: defaultPrevented expected false but got true | `dom/events/Event-defaultPrevented.html` |
| 2 | 2 | assert_equals: expected "I" but got "i" | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 2 | assert_equals: expected "function" but got "undefined" | `dom/nodes/attributes-namednodemap.html` |
| 2 | 4 | assert_equals: expected "pass" but got "[object Object]" | `domparsing/innerhtml-07.html` |
| 2 | 2 | assert_equals: expected "svg" but got "SVG" | `dom/nodes/Element-tagName.html` |
| 2 | 2 | assert_equals: expected (object) Document node with N children but got (undefined) undefined | `dom/nodes/Node-mutation-adoptNode.html` |
| 2 | 2 | assert_equals: expected (object) Element node <pre id="x"></pre> but got (undefined) undefined | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 2 | assert_equals: expected (undefined) undefined but got (string) "foopy" | `dom/nodes/Document-getElementsByTagName.html` |
| 2 | 2 | assert_equals: expected Element node <div></div> but got Element node <input></input> | `shadow-dom/focus/focus-tabindex-order-shadow-varying-tabindex-3.html` |
| 2 | 4 | assert_equals: mutation records must match expected N but got N | `dom/nodes/MutationObserver-inner-outer.html` |
| 2 | 5 | assert_equals: nodeValue expected (string) "pass" but got (undefined) undefined | `dom/nodes/Element-removeAttributeNS.html` |
| 2 | 49 | assert_equals: prefix expected (object) null but got (undefined) undefined | `dom/nodes/Document-createElement.html` |
| 2 | 2 | assert_false: expected false got undefined | `dom/events/EventTarget-dispatchEvent-returnvalue.html` |
| 2 | 3 | assert_throws_dom: function "function () { [source unavailable] }" threw object "TypeError: ShadowRoot nodes cannot be adopted" that is not ... | `dom/nodes/Document-adoptNode-DocumentFragment-with-host.window.html` |

944 distinct subtest messages and 39 distinct harness messages behind these numbers.
