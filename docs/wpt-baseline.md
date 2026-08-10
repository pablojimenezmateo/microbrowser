# The WPT baseline

**Generated**, by `microbrowser_wpt --summary docs/wpt-baseline.md`. Do not edit it: the
next run overwrites it, and that overwrite is the point -- the diff of this file is
what a session moved. The argument for the instrument is `docs/adr/0040`; the work it
sequences is `docs/wpt-plan.md`.

WPT revision: `4120ac0deb573634d8b7cd74c38ae9d647eebdb5`

6081 of 271274 subtests pass (2.2%) over 2867 tests.

**Do not quote that number.** Subtests are not comparable across areas: `encoding/legacy-mb-japanese` alone is 42% of every subtest here.
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
| `FileAPI` | 11 | 6 | 0 | 5 | 0 | 26 | 10 | 38.5 |
| `FileAPI/BlobURL` | 5 | 4 | 0 | 1 | 0 | 16 | 0 | 0.0 |
| `FileAPI/FileReader` | 2 | 1 | 0 | 1 | 0 | 2 | 0 | 0.0 |
| `FileAPI/blob` | 23 | 13 | 0 | 10 | 0 | 259 | 1 | 0.4 |
| `FileAPI/file` | 21 | 14 | 0 | 7 | 0 | 163 | 0 | 0.0 |
| `FileAPI/filelist-section` | 1 | 1 | 0 | 0 | 0 | 7 | 0 | 0.0 |
| `FileAPI/reading-data-section` | 26 | 13 | 0 | 13 | 0 | 48 | 0 | 0.0 |
| `FileAPI/url` | 15 | 3 | 0 | 12 | 0 | 40 | 12 | 30.0 |
| `console` | 19 | 12 | 0 | 7 | 0 | 29 | 6 | 20.7 |
| `cors` | 27 | 20 | 0 | 7 | 0 | 227 | 36 | 15.9 |
| `custom-elements` | 44 | 25 | 0 | 19 | 0 | 598 | 104 | 17.4 |
| `custom-elements/form-associated` | 18 | 14 | 3 | 1 | 0 | 103 | 1 | 1.0 |
| `custom-elements/htmlconstructor` | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0.0 |
| `custom-elements/parser` | 11 | 8 | 0 | 3 | 0 | 20 | 2 | 10.0 |
| `custom-elements/reactions` | 57 | 26 | 0 | 31 | 0 | 372 | 57 | 15.3 |
| `custom-elements/registries` | 40 | 28 | 2 | 10 | 0 | 2207 | 225 | 10.2 |
| `custom-elements/state` | 5 | 4 | 1 | 0 | 0 | 28 | 1 | 3.6 |
| `custom-elements/upgrading` | 7 | 2 | 0 | 5 | 0 | 16 | 1 | 6.2 |
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
| `domxpath` | 32 | 24 | 0 | 8 | 0 | 87 | 1 | 1.1 |
| `encoding` | 74 | 40 | 0 | 34 | 0 | 11801 | 68 | 0.6 |
| `encoding/legacy-mb-japanese` | 51 | 5 | 9 | 37 | 0 | 113882 | 1 | 0.0 |
| `encoding/legacy-mb-korean` | 26 | 1 | 0 | 25 | 0 | 66015 | 0 | 0.0 |
| `encoding/legacy-mb-schinese` | 6 | 4 | 0 | 2 | 0 | 660 | 2 | 0.3 |
| `encoding/legacy-mb-tchinese` | 23 | 2 | 0 | 21 | 0 | 40030 | 123 | 0.3 |
| `encoding/streams` | 13 | 12 | 0 | 1 | 0 | 111 | 0 | 0.0 |
| `hr-time` | 15 | 8 | 0 | 7 | 0 | 13 | 4 | 30.8 |
| `intersection-observer` | 106 | 87 | 0 | 19 | 0 | 180 | 56 | 31.1 |
| `intersection-observer/v2` | 38 | 23 | 0 | 15 | 0 | 54 | 18 | 33.3 |
| `mimesniff/media` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `mimesniff/mime-types` | 3 | 1 | 0 | 2 | 0 | 1939 | 2 | 0.1 |
| `mimesniff/sniffing` | 3 | 3 | 0 | 0 | 0 | 7 | 3 | 42.9 |
| `navigation-timing` | 58 | 25 | 0 | 33 | 0 | 97 | 63 | 64.9 |
| `performance-timeline` | 58 | 27 | 0 | 31 | 0 | 46 | 24 | 52.2 |
| `performance-timeline/not-restored-reasons` | 13 | 13 | 0 | 0 | 0 | 13 | 0 | 0.0 |
| `png` | 3 | 0 | 0 | 3 | 0 | 1 | 0 | 0.0 |
| `resize-observer` | 35 | 25 | 0 | 10 | 0 | 29 | 13 | 44.8 |
| `resource-timing` | 103 | 51 | 1 | 50 | 1 | 538 | 7 | 1.3 |
| `resource-timing/initiator-type` | 16 | 11 | 0 | 5 | 0 | 29 | 7 | 24.1 |
| `resource-timing/tentative` | 26 | 16 | 0 | 10 | 0 | 39 | 0 | 0.0 |
| `selection` | 83 | 34 | 34 | 15 | 0 | 143 | 0 | 0.0 |
| `selection/anonymous` | 3 | 2 | 0 | 1 | 0 | 12 | 0 | 0.0 |
| `selection/bidi` | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0.0 |
| `selection/caret` | 3 | 3 | 0 | 0 | 0 | 11 | 2 | 18.2 |
| `selection/contenteditable` | 10 | 6 | 0 | 4 | 0 | 61 | 0 | 0.0 |
| `selection/shadow-dom` | 12 | 8 | 0 | 4 | 0 | 51 | 1 | 2.0 |
| `selection/textcontrols` | 7 | 3 | 0 | 4 | 0 | 3 | 0 | 0.0 |
| `shadow-dom` | 66 | 58 | 0 | 8 | 0 | 728 | 71 | 9.8 |
| `shadow-dom/declarative` | 56 | 40 | 4 | 12 | 0 | 7648 | 5 | 0.1 |
| `shadow-dom/focus` | 37 | 31 | 1 | 5 | 0 | 78 | 5 | 6.4 |
| `shadow-dom/focus-navigation` | 45 | 44 | 0 | 1 | 0 | 89 | 2 | 2.2 |
| `shadow-dom/leaktests` | 4 | 3 | 0 | 1 | 0 | 16 | 6 | 37.5 |
| `shadow-dom/reference-target` | 15 | 14 | 0 | 1 | 0 | 768 | 0 | 0.0 |
| `shadow-dom/untriaged` | 54 | 52 | 0 | 2 | 0 | 248 | 157 | 63.3 |
| `storage` | 28 | 11 | 0 | 17 | 0 | 23 | 0 | 0.0 |
| `storage/buckets` | 10 | 8 | 0 | 2 | 0 | 48 | 0 | 0.0 |
| `streams` | 3 | 2 | 0 | 1 | 0 | 3 | 0 | 0.0 |
| `streams/piping` | 13 | 13 | 0 | 0 | 0 | 204 | 0 | 0.0 |
| `streams/readable-byte-streams` | 11 | 10 | 0 | 1 | 0 | 233 | 5 | 2.1 |
| `streams/readable-streams` | 22 | 21 | 0 | 1 | 0 | 362 | 17 | 4.7 |
| `streams/transferable` | 12 | 7 | 0 | 5 | 0 | 71 | 0 | 0.0 |
| `streams/transform-streams` | 12 | 12 | 0 | 0 | 0 | 134 | 0 | 0.0 |
| `streams/writable-streams` | 16 | 16 | 0 | 0 | 0 | 196 | 0 | 0.0 |
| `subresource-integrity` | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0.0 |
| `subresource-integrity/integrity-policy` | 5 | 0 | 0 | 5 | 0 | 37 | 0 | 0.0 |
| `subresource-integrity/signatures` | 15 | 14 | 0 | 1 | 0 | 177 | 43 | 24.3 |
| `subresource-integrity/unencoded-digest` | 9 | 6 | 0 | 3 | 0 | 84 | 28 | 33.3 |
| `uievents` | 3 | 3 | 0 | 0 | 0 | 5 | 2 | 40.0 |
| `uievents/click` | 7 | 0 | 0 | 7 | 0 | 3 | 0 | 0.0 |
| `uievents/constructors` | 2 | 2 | 0 | 0 | 0 | 20 | 0 | 0.0 |
| `uievents/interface` | 3 | 1 | 0 | 2 | 0 | 5 | 0 | 0.0 |
| `uievents/keyboard` | 5 | 1 | 0 | 4 | 0 | 1 | 1 | 100.0 |
| `uievents/legacy` | 1 | 1 | 0 | 0 | 0 | 4 | 0 | 0.0 |
| `uievents/legacy-domevents-tests` | 4 | 3 | 0 | 1 | 0 | 3 | 3 | 100.0 |
| `uievents/mouse` | 17 | 11 | 0 | 6 | 0 | 71 | 1 | 1.4 |
| `uievents/order-of-events` | 16 | 5 | 0 | 11 | 0 | 6 | 0 | 0.0 |
| `uievents/textInput` | 7 | 7 | 0 | 0 | 0 | 24 | 0 | 0.0 |
| `url` | 71 | 39 | 0 | 32 | 0 | 9395 | 2042 | 21.7 |
| `user-timing` | 58 | 29 | 0 | 29 | 0 | 180 | 64 | 35.6 |
| `web-animations` | 1 | 1 | 0 | 0 | 0 | 1 | 0 | 0.0 |
| `web-animations/animation-model` | 25 | 25 | 0 | 0 | 0 | 146 | 20 | 13.7 |
| `web-animations/animation-trigger` | 3 | 3 | 0 | 0 | 0 | 3 | 0 | 0.0 |
| `web-animations/interfaces` | 42 | 34 | 0 | 8 | 0 | 663 | 5 | 0.8 |
| `web-animations/responsive` | 40 | 39 | 0 | 1 | 0 | 99 | 4 | 4.0 |
| `web-animations/timing-model` | 28 | 24 | 0 | 4 | 0 | 394 | 6 | 1.5 |
| `webmessaging` | 60 | 15 | 1 | 44 | 0 | 34 | 12 | 35.3 |
| `webmessaging/broadcastchannel` | 14 | 6 | 0 | 8 | 0 | 34 | 13 | 38.2 |
| `webmessaging/message-channels` | 23 | 9 | 0 | 14 | 0 | 16 | 8 | 50.0 |
| `webmessaging/multi-globals` | 4 | 4 | 0 | 0 | 0 | 4 | 0 | 0.0 |
| `webmessaging/with-options` | 9 | 2 | 0 | 7 | 0 | 2 | 0 | 0.0 |
| `webmessaging/with-ports` | 24 | 11 | 0 | 13 | 0 | 17 | 1 | 5.9 |
| `webmessaging/without-ports` | 27 | 13 | 0 | 14 | 0 | 15 | 2 | 13.3 |
| `webstorage` | 54 | 34 | 1 | 19 | 0 | 1259 | 1164 | 92.5 |

## Why the harness never reported

Ranked by tests affected. One line here is worth more than a page of the table
above: a test whose harness failed reports *no* subtests, so these are invisible in
the pass rate and are the largest block of unrealised coverage in the suite.

| tests | cause | example |
|--:|---|---|
| 569 | TIMEOUT: the page never reported | `FileAPI/idlharness.worker.html` |
| 258 | TIMEOUT:  | `FileAPI/FileReaderSync.worker.html` |
| 30 | ERROR: ReferenceError: getSelection is not defined | `selection/addRange-08.html` |
| 30 | ERROR: TypeError: Illegal constructor: Document | `dom/nodes/Node-compareDocumentPosition.html` |
| 23 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: unexpected token '<' (line N) SyntaxError: unexpected token '<' ... | `custom-elements/Document-createElement-svg.svg` |
| 16 | TIMEOUT: killed after the wall-clock budget | `dom/nodes/Node-insertBefore.html` |
| 13 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: getSelection is not defined ReferenceError: getSelection is n... | `selection/getRangeAt.html` |
| 9 | ERROR: RangeError: script ran too long | `encoding/legacy-mb-japanese/iso-2022-jp/iso2022jp-encode-form-csiso2022jp.html` |
| 7 | TIMEOUT: the page never reported; first script error: ./support/helpers.js: SyntaxError: expected ')' to close a dynamic import (line N) SyntaxError:... | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect-iframe.html` |
| 6 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createDocument) is not a function TypeError: undefined ... | `dom/nodes/append-on-Document.html` |
| 5 | TIMEOUT: the page never reported; first script error: /trusted-types/support/helper.sub.js: SyntaxError: expected ';' (line N) SyntaxError: expected ... | `domparsing/tentative/stream-html-with-trusted-types-error-in-policy.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: target is not defined ReferenceError: target is not defined a... | `intersection-observer/grow-height-and-scrolled.html` |
| 5 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: Illegal constructor: CustomElementRegistry TypeError: Illegal cons... | `custom-elements/registries/Document-createElement.html` |
| 5 | TIMEOUT: the page never reported; first script error: support.js?pipe=sub: SyntaxError: expected a property name (line N) SyntaxError: expected a pro... | `cors/credentials-flag.htm` |
| 4 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot set property 'onerror' of undefined TypeError: cannot set p... | `custom-elements/cross-realm-callback-report-exception.html` |
| 4 | TIMEOUT: the page never reported; first script error: resources/webperftestharness.js: ReferenceError: ﻿ is not defined ReferenceError: ﻿ is not ... | `resource-timing/resource_connection_reuse_mixed_content.html` |
| 3 | ERROR: TypeError: undefined (createDocument) is not a function | `dom/nodes/Document-createAttribute.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'insertRule' of undefined TypeError: cannot r... | `dom/events/webkit-animation-end-event.html` |
| 3 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (write) is not a function TypeError: undefined (write) i... | `custom-elements/parser/parser-constructs-custom-element-in-document-write.html` |
| 2 | CRASH: killed by signal Segmentation fault | `dom/nodes/moveBefore/relevant-mutations.html` |
| 2 | ERROR: ReferenceError: DOMParser is not defined | `domparsing/DOMParser-parseFromString-html.html` |
| 2 | ERROR: Test named 'Repeated declarative shadow roots keep only the first' specified N 'cleanup' function, and N failed. | `shadow-dom/declarative/declarative-shadow-dom-repeats.html` |
| 2 | ERROR: [object Object] | `domparsing/DOMParser-parseFromString-encoding.html` |
| 2 | TIMEOUT: the page never reported; first script error: ../editing/include/editor-test-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expect... | `selection/move-by-word-korean.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: DOMParser is not defined ReferenceError: DOMParser is not def... | `domxpath/fn-lang.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: host is not defined ReferenceError: host is not defined at <a... | `dom/events/shadow-relatedTarget.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: input is not defined ReferenceError: input is not defined at ... | `selection/textcontrols/selectionchange-bubble.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: outside is not defined ReferenceError: outside is not defined... | `shadow-dom/focus/click-focus-delegatesFocus-click.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: scroller is not defined ReferenceError: scroller is not defin... | `dom/events/scrolling/scrollIntoView-in-onscroll-to-sticky.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: testN is not defined ReferenceError: testN is not defined at ... | `shadow-dom/event-post-dispatch.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: trustedTypes is not defined ReferenceError: trustedTypes is n... | `domparsing/tentative/positional-methods-with-parser-options.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'append' of undefined TypeError: cannot read ... | `resource-timing/TAO-match.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'customElements' of undefined TypeError: cann... | `dom/nodes/create-element-realm-after-adoption.html` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (createProcessingInstruction) is not a function TypeErro... | `dom/nodes/Node-isEqualNode-xhtml.xhtml` |
| 2 | TIMEOUT: the page never reported; first script error: inline script #N: [object Object] Error: undefined at get_stack (@N) at AssertionError (@N) at ... | `dom/nodes/Document-createCDATASection.html` |
| 1 | ERROR: N duplicate test name: "Should throw TypeError for function "function () { [source unavailable] }"." | `webstorage/missing_arguments.window.html` |
| 1 | ERROR: N duplicate test name: "sending ND canvas ImageBitmap to http://N.N.N.N:N" | `webmessaging/postMessage_cross_domain_image_transfer_2d.sub.htm` |
| 1 | ERROR: N duplicate test names: "a.classList in undefined namespace should be DOMTokenList.", "area.classList in undefined namespace should be DOMTo... | `dom/lists/DOMTokenList-coverage-for-attributes.html` |
| 1 | ERROR: N duplicate test names: "touchstart listener is passive by default for HTMLElement", "touchstart listener is passive with {passive:undefined... | `dom/events/passive-by-default.html` |
| 1 | ERROR: Test named 'Adoption with global registry' specified N 'cleanup' function, and N failed. | `custom-elements/registries/adoption.window.html` |
| 1 | ERROR: Test named 'Cloning with global registry' specified N 'cleanup' function, and N failed. | `custom-elements/registries/Document-importNode-cross-document.window.html` |
| 1 | ERROR: Test named 'Custom element with HTMLSubmitButtonBehavior is implicitly focusable' specified N 'cleanup' function, and N failed. | `custom-elements/form-associated/ElementInternals-behavior-accessibility.tentative.html` |
| 1 | ERROR: Test named 'Custom submit button with form method=dialog closes dialog with behavior value' specified N 'cleanup' function, and N failed. | `custom-elements/form-associated/ElementInternals-submit-behavior-dialog.tentative.html` |
| 1 | ERROR: Test named 'Declarative Shadow DOM: missing shadowrootslotassignment defaults to named' specified N 'cleanup' function, and N failed. | `shadow-dom/declarative/declarative-shadow-dom-slot-assignment.html` |
| 1 | ERROR: Test named 'Drag from contenteditable into out-of-flow user-select:none extends selection to editable boundary.' specified N 'cleanup' funct... | `selection/drag-selection-contenteditable-to-out-of-flow-user-select-none.html` |
| 1 | ERROR: Test named 'Drag rightward out of a floated block extends the selection forward, not backward to the start of the float's content.' specifie... | `selection/drag-out-of-floated-content.html` |
| 1 | ERROR: Test named 'Focused element is removed' specified N 'cleanup' function, and N failed. | `selection/selection-focused-element-becomes-nonfocusable.html` |
| 1 | ERROR: Test named 'HTMLSubmitButtonBehavior properties have correct default values' specified N 'cleanup' function, and N failed. | `custom-elements/form-associated/ElementInternals-submit-behavior.tentative.html` |
| 1 | ERROR: Test named 'Text with user-select:text is selectable even if it is inside a user-select:none element.' specified N 'cleanup' function, and N... | `selection/drag-selection-extend-to-user-select-none.html` |
| 1 | ERROR: Test named 'delegatesFocus shouldn't cause extra focus steps' specified N 'cleanup' function, and N failed. | `shadow-dom/focus/focus-scroll-under-delegatesFocus.html` |
| 1 | ERROR: Test named 'shadowrootslotassignment=manual is serialized and appears before shadowrootclonable and shadowrootserializable' specified N 'cle... | `shadow-dom/declarative/declarative-shadow-dom-slot-assignment-serialization.html` |
| 1 | ERROR: Test named 'state selector has influence on nth-of when state is applied' specified N 'cleanup' functions, and N failed. | `custom-elements/state/state-css-selector-nth-of.html` |
| 1 | ERROR: TypeError: undefined (createProcessingInstruction) is not a function | `dom/nodes/CharacterData-remove.html` |
| 1 | TIMEOUT: the page never reported; first script error: ../../editing/include/tests.js: SyntaxError: expected ';' (line N) SyntaxError: expected ';' (l... | `selection/contenteditable/initial-selection-on-focus.tentative.html` |
| 1 | TIMEOUT: the page never reported; first script error: /editing/include/editor-test-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expected... | `selection/deleteFromDocument-HTMLDetails.html` |
| 1 | TIMEOUT: the page never reported; first script error: /performance-timeline/webtiming-resolution.any.js: RangeError: script ran too long RangeError: ... | `performance-timeline/webtiming-resolution.any.html` |
| 1 | TIMEOUT: the page never reported; first script error: /resource-timing/sizes-redirect.any.js: TypeError: undefined (clearResourceTimings) is not a fu... | `resource-timing/sizes-redirect.any.html` |
| 1 | TIMEOUT: the page never reported; first script error: /resource-timing/supported_resource_type.any.js: ReferenceError: ﻿test is not defined Referen... | `resource-timing/supported_resource_type.any.html` |
| 1 | TIMEOUT: the page never reported; first script error: /service-workers/service-worker/resources/test-helpers.sub.js: SyntaxError: expected ';' (line ... | `streams/transferable/service-worker.https.html` |
| 1 | TIMEOUT: the page never reported; first script error: /streams/queuing-strategies.any.js: ReferenceError: CountQueuingStrategy is not defined Referen... | `streams/queuing-strategies.any.html` |
| 1 | TIMEOUT: the page never reported; first script error: /streams/transferable/transform-stream-members.any.js: ReferenceError: TransformStream is not d... | `streams/transferable/transform-stream-members.any.html` |
| 1 | TIMEOUT: the page never reported; first script error: /wai-aria/scripts/aria-utils.js: SyntaxError: expected ';' (line N) SyntaxError: expected ';' (... | `shadow-dom/reference-target/tentative/aria-labelledby.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: AnimationEffect is not defined ReferenceError: AnimationEffec... | `web-animations/interfaces/KeyframeEffect/style-change-events.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: DocumentTimeline is not defined ReferenceError: DocumentTimel... | `hr-time/raf-coarsened-time.https.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLAreaElement is not defined ReferenceError: HTMLAreaElemen... | `custom-elements/reactions/customized-builtins/HTMLAreaElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLBaseElement is not defined ReferenceError: HTMLBaseElemen... | `custom-elements/reactions/customized-builtins/HTMLBaseElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLDataElement is not defined ReferenceError: HTMLDataElemen... | `custom-elements/reactions/customized-builtins/HTMLDataElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLDetailsElement is not defined ReferenceError: HTMLDetails... | `custom-elements/reactions/customized-builtins/HTMLDetailsElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLEmbedElement is not defined ReferenceError: HTMLEmbedElem... | `custom-elements/reactions/customized-builtins/HTMLEmbedElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLFieldSetElement is not defined ReferenceError: HTMLFieldS... | `custom-elements/reactions/customized-builtins/HTMLFieldSetElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLMapElement is not defined ReferenceError: HTMLMapElement ... | `custom-elements/reactions/customized-builtins/HTMLMapElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLMetaElement is not defined ReferenceError: HTMLMetaElemen... | `custom-elements/reactions/customized-builtins/HTMLMetaElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLModElement is not defined ReferenceError: HTMLModElement ... | `custom-elements/reactions/customized-builtins/HTMLModElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLOptGroupElement is not defined ReferenceError: HTMLOptGro... | `custom-elements/reactions/customized-builtins/HTMLOptGroupElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLParamElement is not defined ReferenceError: HTMLParamElem... | `custom-elements/reactions/customized-builtins/HTMLParamElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLProgressElement is not defined ReferenceError: HTMLProgre... | `custom-elements/reactions/customized-builtins/HTMLProgressElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLQuoteElement is not defined ReferenceError: HTMLQuoteElem... | `custom-elements/reactions/customized-builtins/HTMLQuoteElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLSlotElement is not defined ReferenceError: HTMLSlotElemen... | `custom-elements/reactions/customized-builtins/HTMLSlotElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLSourceElement is not defined ReferenceError: HTMLSourceEl... | `custom-elements/reactions/customized-builtins/HTMLSourceElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLTableColElement is not defined ReferenceError: HTMLTableC... | `custom-elements/reactions/customized-builtins/HTMLTableColElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: HTMLTimeElement is not defined ReferenceError: HTMLTimeElemen... | `custom-elements/reactions/customized-builtins/HTMLTimeElement.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: SharedWorker is not defined ReferenceError: SharedWorker is n... | `resource-timing/tentative/initiator-url/post-message-shared-worker.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: XPathEvaluator is not defined ReferenceError: XPathEvaluator ... | `domxpath/xpathevaluatorbase-creatensresolver.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: container is not defined ReferenceError: container is not def... | `shadow-dom/focus/click-focus-slot-ancestor.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: hostN is not defined ReferenceError: hostN is not defined at ... | `selection/shadow-dom/tentative/Selection-getComposedRanges-collapsed.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: iframe is not defined ReferenceError: iframe is not defined a... | `intersection-observer/fixed-position-iframe-scroll.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: red is not defined ReferenceError: red is not defined at <ano... | `uievents/mouse/layout_change_should_fire_mouseover.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: released is not defined ReferenceError: released is not defin... | `uievents/order-of-events/mouse-events/mouseover-out.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: scheduler is not defined ReferenceError: scheduler is not def... | `resource-timing/tentative/initiator-url/post-task.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: targetDiv is not defined ReferenceError: targetDiv is not def... | `dom/events/scrolling/scrollend-event-fired-for-scroll-attr-change.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: targetXDiv is not defined ReferenceError: targetXDiv is not d... | `dom/events/scrolling/scrollend-event-fired-to-element-with-overscroll-behavior.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: ReferenceError: test is not defined ReferenceError: test is not defined at <a... | `resize-observer/svg-with-css-box-002.svg` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: SyntaxError: expected ')' after a for head (line N) SyntaxError: expected ')'... | `dom/nodes/Document-getElementsByTagName-xhtml.xhtml` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'ReadableStream' of undefined TypeError: cann... | `streams/readable-streams/global.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'defaultView' of undefined TypeError: cannot ... | `custom-elements/registries/define.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'querySelector' of null TypeError: cannot rea... | `selection/shadow-dom/cross-shadow-boundary-extend.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'ready' of undefined TypeError: cannot read p... | `resize-observer/svg.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: cannot read property 'set' of undefined TypeError: cannot read pro... | `resource-timing/content-encoding.https.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (getElementById) is not a function TypeError: undefined ... | `selection/drag-disabled-textarea-shadow-dom.html` |
| 1 | TIMEOUT: the page never reported; first script error: inline script #N: TypeError: undefined (getHTML) is not a function TypeError: undefined (getHTM... | `shadow-dom/declarative/gethtml-ordering.html` |

## Why subtests fail

Ranked by *distinct tests* affected rather than by subtests, because that is the
number a fix unblocks. Digits are collapsed to `N`; quoted values are not, because
`expected "block" but got "inline"` and `expected "Npx" but got "Npx"` are
different bugs and a bucket labelled `assert_equals` is not actionable.

| tests | subtests | message | example |
|--:|--:|---|---|
| 90 | 303 | Test timed out | `FileAPI/FileReader/workers.html` |
| 80 | 189490 | NOTRUN (no message) | `custom-elements/CustomElementRegistry.html` |
| 56 | 86 | promise_test: Unhandled rejection with value: object "Error: document.elementsFromPoint unsupported" | `custom-elements/form-associated/label-delegatesFocus.html` |
| 55 | 1912 | assert_throws_js: function "function TypeError() { [native code] }" is not an Error subtype | `custom-elements/HTMLElement-constructor.html` |
| 35 | 514 | Illegal constructor: ReadableStream | `streams/piping/close-propagation-backward.any.html` |
| 34 | 500 | assert_throws_js: function "function () { [source unavailable] }" did not throw | `FileAPI/blob/Blob-constructor.any.html` |
| 33 | 92 | assert_equals: expected N but got N | `custom-elements/attribute-changed-callback.html` |
| 32 | 48 | assert_true: Failed to create new rendered document expected true got false | `shadow-dom/untriaged/elements-and-dom-objects/extensions-to-element-interface/methods/test-002.html` |
| 32 | 64 | assert_true: expected true got false | `console/console-is-a-namespace.any.html` |
| 30 | 274 | promise_test: Unhandled rejection with value: object "TypeError: Illegal constructor: ReadableStream" | `encoding/streams/decode-ignore-bom.any.html` |
| 29 | 84 | promise_test: Unhandled rejection with value: object "Error: action_sequence() is not implemented by testdriver-vendor.js" | `dom/events/click-on-absolute-pseudo.html` |
| 25 | 1011 | assert_throws_dom: function "function () { [source unavailable] }" did not throw | `custom-elements/registries/global.window.html` |
| 21 | 93 | Illegal constructor: CustomElementRegistry | `custom-elements/registries/Construct.html` |
| 21 | 393 | KeyframeEffect is not defined | `web-animations/animation-model/animation-types/clamping-001.html` |
| 20 | 171 | WritableStream is not defined | `streams/piping/general.any.html` |
| 19 | 37 | assert_equals: expected (object) null but got (undefined) undefined | `custom-elements/registries/Document-customElementRegistry.html` |
| 19 | 19 | promise_test: Unhandled rejection with value: object "TypeError: cannot set property 'name' of undefined" | `FileAPI/idlharness.any.html` |
| 17 | 58 | XPathResult is not defined | `dom/xpath-result-single-node-value-nullable.html` |
| 17 | 85 | getSelection is not defined | `selection/modify-extend-word-trailing-inline-block.tentative.html` |
| 16 | 27 | assert_equals: entries.length expected N but got N | `intersection-observer/bounding-box.html` |
| 16 | 50 | undefined (createDocument) is not a function | `custom-elements/registries/scoped-registry-initialize-upgrades.html` |
| 15 | 15 | assert_equals: IntersectionObserverEntryCount expected N but got N | `intersection-observer/scroll-and-root-margin.html` |
| 15 | 40 | promise_test: Unhandled rejection with value: object "TypeError: undefined (open) is not a function" | `cors/preflight-cache-partitioning.sub.window.html` |
| 14 | 124 | CROSSDOMAIN is not defined | `cors/client-hint-request-headers-2.tentative.htm` |
| 14 | 136 | DOMParser is not defined | `dom/nodes/Node-normalize.html` |
| 14 | 42 | FileReader is not defined | `FileAPI/fileReader.any.html` |
| 14 | 68 | Illegal constructor: HTMLElement | `custom-elements/HTMLElement-attachInternals.html` |
| 14 | 95 | promise_test: Unhandled rejection with value: object "ReferenceError: Observable is not defined" | `dom/observable/tentative/crashtests/observable-gc.any.html` |
| 14 | 14 | promise_test: Unhandled rejection with value: object "ReferenceError: createStylesheetHost is not defined" | `shadow-dom/declarative/tentative/shadowrootadoptedstylesheets/shadowrootadoptedstylesheets-async-fetch-disconnect.html` |
| 13 | 29 | Illegal constructor: Document | `custom-elements/Document-createElementNS.html` |
| 13 | 39 | Illegal constructor: EventTarget | `dom/events/AddEventListenerOptions-once.any.html` |
| 13 | 42 | container is not defined | `custom-elements/form-associated/ElementInternals-validation.html` |
| 13 | 177 | undefined (createProcessingInstruction) is not a function | `dom/events/EventTarget-this-of-listener.html` |
| 13 | 27 | undefined (getElementById) is not a function | `dom/nodes/DocumentFragment-getElementById.html` |
| 12 | 114 | TransformStream is not defined | `streams/transferable/transform-stream.html` |
| 12 | 12 | cannot set property 'scrollTop' of undefined | `intersection-observer/disconnect.html` |
| 12 | 22 | promise_test: Unhandled rejection with value: object "Error: observe_entry: timeout" | `resource-timing/cross-origin-iframe.html` |
| 12 | 12 | promise_test: Unhandled rejection with value: object "ReferenceError: iframe is not defined" | `intersection-observer/cross-origin-tall-iframe.sub.html` |
| 12 | 12 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'append' of undefined" | `performance-timeline/not-restored-reasons/performance-navigation-timing-attributes.tentative.window.html` |
| 12 | 33 | undefined (createAttribute) is not a function | `custom-elements/attribute-changed-callback.html` |
| 12 | 100 | undefined (createValueRange) is not a function | `dom/ranges/tentative/OpaqueRange-auto-disconnect.html` |
| 11 | 132 | Observable is not defined | `dom/observable/tentative/observable-catch.any.html` |
| 11 | 13 | assert_equals: expected (object) object "[object Object]" but got (undefined) undefined | `custom-elements/registries/Document-customElementRegistry.html` |
| 11 | 30 | promise_test: Unhandled rejection with value: object "ReferenceError: KeyframeEffect is not defined" | `web-animations/animation-trigger/event-trigger-before-handlers.tentative.html` |
| 11 | 23 | promise_test: Unhandled rejection with value: object "TypeError: undefined (moveBefore) is not a function" | `dom/nodes/moveBefore/Node-moveBefore.html` |
| 11 | 4613 | undefined (setHTMLUnsafe) is not a function | `custom-elements/registries/ShadowRoot-init-declarative.html` |
| 10 | 147 | Failed to execute 'animate': keyframes are required | `web-animations/interfaces/Animation/oncancel.html` |
| 10 | 10 | assert_approx_equals: entries[N].boundingClientRect.left expected N +/- N but got N | `intersection-observer/clip-path-animation.html` |
| 10 | 56 | assert_unreached: Should have rejected: undefined Reached unreachable code | `subresource-integrity/signatures/tentative/header-component.window.html` |
| 10 | 96 | cannot read property 'getComputedTiming' of undefined | `web-animations/animation-model/animation-types/discrete.html` |
| 10 | 13 | undefined (open) is not a function | `FileAPI/url/url-charset.window.html` |
| 9 | 26 | The encoding label provided ('utf-Nle') is invalid. | `encoding/api-basics.any.html` |
| 9 | 53 | assert_array_equals: lengths differ, expected array ["constructed"] length N, got [] length N | `custom-elements/reactions/Node.html` |
| 9 | 48 | assert_equals: Response status is N. expected N but got N | `subresource-integrity/signatures/tentative/header-component.window.html` |
| 9 | 13 | assert_true: expected true got undefined | `intersection-observer/v2/delay-test.html` |
| 9 | 9 | cannot read property 'postMessage' of undefined | `dom/events/EventListener-incumbent-global-1.sub.html` |
| 9 | 15 | cannot read property 'then' of undefined | `web-animations/animation-model/keyframe-effects/effect-value-context.html` |
| 9 | 31 | promise_test: Unhandled rejection with value: object "TypeError: Failed to fetch" | `FileAPI/url/url-with-fetch.any.html` |
| 9 | 10 | promise_test: Unhandled rejection with value: object "TypeError: undefined (addEventListener) is not a function" | `resource-timing/buffer-full-add-then-clear.html` |
| 9 | 435 | promise_test: Unhandled rejection with value: object "TypeError: undefined is not a function" | `domparsing/tentative/stream-html-custom-element.html` |
| 9 | 43 | undefined (getSelection) is not a function | `custom-elements/reactions/Selection.html` |
| 9 | 15 | undefined (moveBefore) is not a function | `dom/nodes/moveBefore/Node-moveBefore.html` |
| 8 | 3025 | The encoding label provided ('iso-N-N') is invalid. | `encoding/single-byte-decoder.any.html?TextDecoder` |
| 8 | 65 | assert_array_equals: lengths differ, expected array ["attributeChanged"] length N, got [] length N | `custom-elements/reactions/AriaMixin-string-attributes.tentative.html` |
| 8 | 45 | assert_unreached: Script should not fail. Reached unreachable code | `subresource-integrity/signatures/tentative/csp.window.html` |
| 8 | 14 | cannot read property 'length' of undefined | `FileAPI/filelist-section/filelist.html` |
| 8 | 26 | host is not defined | `custom-elements/registries/CustomElementRegistry-initialize.html` |
| 8 | 17 | promise_test: Unhandled rejection with value: object "TypeError: undefined (updatePlaybackRate) is not a function" | `web-animations/timing-model/animations/pausing-an-animation.html` |
| 7 | 20 | The encoding label provided ('utf-Nbe') is invalid. | `encoding/api-basics.any.html` |
| 7 | 2588 | The encoding label provided ('windows-N') is invalid. | `encoding/single-byte-decoder.any.html?TextDecoder` |
| 7 | 14 | assert_equals: expected "Npx" but got "" | `web-animations/responsive/assorted-lengths.html` |
| 7 | 12 | assert_false: expected false got true | `cors/preflight-failure.htm` |
| 7 | 9 | assert_true: Initially visible expected true got undefined | `intersection-observer/v2/svg-foreign-object-filter-occlusion.html` |
| 7 | 28 | cannot read property 'document' of undefined | `dom/events/event-global-extra.window.html` |
| 7 | 22 | cannot read property 'getElementById' of null | `domxpath/xpath-shadow-dom.html` |
| 7 | 16 | document.elementsFromPoint unsupported | `uievents/order-of-events/mouse-events/click-cancel.html` |
| 7 | 64 | promise_test: Unhandled rejection with value: object "ReferenceError: DataTransfer is not defined" | `FileAPI/file/send-file-form-controls.html` |
| 7 | 46 | promise_test: Unhandled rejection with value: object "ReferenceError: getSelection is not defined" | `dom/nodes/moveBefore/selection-preserve.html` |
| 7 | 20 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'currentTime' of undefined" | `web-animations/interfaces/Animatable/getAnimations.html` |
| 7 | 7 | promise_test: Unhandled rejection with value: object "TypeError: cannot read property 'estimate' of undefined" | `storage/estimate-parallel.https.any.html` |
| 7 | 7 | promise_test: Unhandled rejection with value: object "TypeError: undefined (clearResourceTimings) is not a function" | `resource-timing/304-response-recorded.html` |
| 7 | 7 | target is not defined | `dom/events/mouse-event-retarget.html` |
| 6 | 22 | Illegal constructor: Text | `dom/nodes/adoption.window.html` |
| 6 | 260 | The encoding label provided ('koiN-u') is invalid. | `encoding/single-byte-decoder.any.html?TextDecoder` |
| 6 | 260 | The encoding label provided ('x-mac-cyrillic') is invalid. | `encoding/single-byte-decoder.any.html?TextDecoder` |
| 6 | 53 | assert_array_equals: lengths differ, expected array ["constructed", "attributeChanged"] length N, got ["constructed"] length N | `custom-elements/reactions/AriaMixin-string-attributes.tentative.html` |
| 6 | 17 | assert_equals: expected "Npx" but got "N.Npx" | `web-animations/interfaces/KeyframeEffect/setKeyframes.html` |
| 6 | 900 | assert_equals: username expected (string) "" but got (undefined) undefined | `url/url-constructor.any.html?include=javascript` |
| 6 | 8 | assert_false: expected false got null | `intersection-observer/transformed-iframe-001-same-origin.html` |
| 6 | 6 | assert_not_equals: window.KeyframeEffect got disallowed value undefined | `web-animations/animation-model/animation-types/accumulation-per-property-002.html` |
| 6 | 6 | assert_not_equals: window.performance.navigation is defined got disallowed value undefined | `navigation-timing/test-navigation-attributes-exist.html` |
| 6 | 19 | assert_throws_exactly: function "function () { [source unavailable] }" did not throw | `FileAPI/blob/Blob-constructor-dom.window.html` |
| 6 | 3428 | assert_throws_js: function "function RangeError() { [native code] }" is not an Error subtype | `encoding/api-replacement-encodings.any.html` |
| 6 | 8 | new_parent is not defined | `dom/nodes/moveBefore/fire-focusin-focusout.html` |
| 6 | 22 | promise_test: Unhandled rejection with value: object "ReferenceError: WritableStream is not defined" | `streams/transferable/transfer-with-messageport.window.html` |
| 6 | 9 | promise_test: Unhandled rejection with value: object "ReferenceError: container is not defined" | `selection/onselectionchange-on-document.html` |
| 6 | 287 | undefined is not a function | `FileAPI/blob/Blob-newobject.any.html` |
| 5 | 5 | DataCloneError: transferring objects is not supported | `FileAPI/blob/Blob-constructor.any.html` |
| 5 | 720 | Failed to construct URL: invalid URL | `url/url-constructor.any.html?include=file` |
| 5 | 9 | HTMLCollection is not defined | `dom/nodes/Document-getElementsByClassName.html` |

9124 distinct subtest messages and 104 distinct harness messages behind these numbers.
