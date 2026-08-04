# Survey — reddit.com, youtube.com and Plex, 2026-08-04

The measurement behind ADRs 0015–0030. ADR 0007 says the method: load it, look at what breaks,
write it down. This is the writing-down, kept separate from the decisions so that a decision can be
argued with without re-fetching 16MB of script.

Everything here was taken on **2026-08-04** with a Chrome user-agent string, `--compressed`, and a
cookie jar. It is one day, one region, one logged-out session. Treat every number as the reason a
decision was made, not as a number to defend.

## How the counts were taken, and what they are worth

The API counts below are **textual occurrences of a name in a minified bundle**. That over-counts
(a name inside a string literal, a polyfill defining the thing it polyfills) and under-counts (a
dynamic access like `el["scroll" + "Top"]`, which minifiers produce). It is a *ranking* signal and
nothing finer. ADR 0014 counted CSS the same way and the ranking it produced held up, which is the
only evidence that this method is worth anything.

Where a number decides something, the ADR that uses it says what would have to be true for it to be
wrong.

## 1. reddit.com has a door before it has a page

`GET https://www.reddit.com/` does not return reddit. It returns **8,424 bytes** titled
"Reddit - Please wait for verification", with

    content-security-policy: default-src 'none'; script-src 'nonce-…'; style-src 'unsafe-inline';
                             img-src https://www.redditstatic.com; form-action 'self';
    set-cookie: edgebucket=…; Domain=reddit.com; Max-Age=63071999

and one inline script whose entire job is to prove a browser ran it:

```js
document.addEventListener("DOMContentLoaded", async function () {
  var e = document.forms[0],
      n = (e.onsubmit = function (t) {
        return new URLSearchParams(document.location.search)
                 .forEach((e, n) => t.target.appendChild(Object.assign(
                    document.createElement("input"), {name: n, type: "hidden", value: e}))), !0
      },
      await (async e => e + e)("0eeeb3f7ebcde213"));
  e.elements.namedItem("solution").value = n;
  e.requestSubmit();
}, { once: !0 });
```

The hidden form carries `token`, `js_challenge=1`, `jsc_orig_r` and the `solution` the script fills
in. Submitting it with the cookie from the first response returns the real page: **405,179 bytes**.

Read the list of what that costs, because it is short and every item is load-bearing:

| Needed | Have it? |
|---|---|
| `DOMContentLoaded` fires, and `{once: true}` is honoured | listeners yes, this event no |
| `document.forms`, and `form.elements.namedItem()` | no |
| `URLSearchParams`, `location.search` | no |
| `Object.assign` onto an element (setting `name`/`type`/`value` as IDL attributes) | partial |
| `form.requestSubmit()`, and the navigation it causes | no |
| a cookie set on the first response, sent on the second | yes |
| `async` arrow function, `await` | yes |

**This is the finding that reorders everything.** "Render reddit.com" was scheduled as a layout and
CSS problem. It is not reachable at all until a handful of small, dull bindings exist, and a browser
that renders reddit's markup perfectly still shows a spinner.

## 2. reddit's page is mostly not in its page

The 405KB document parses to **1,454 tags**. Of those:

| | |
|---|---|
| distinct custom-element names | **67** |
| custom-element instances | 218 |
| `<script>` | 35 (48 CSP nonces) |
| `<template>` | 5 |
| `slot="…"` attributes | 70 |
| inline `style="…"` | 98 |
| `srcset` | 6 |
| `loading="lazy"` | 27 |
| distinct `/svc/shreddit/…` HTML-fragment endpoints | 31 |
| **server-rendered posts in the feed** | **3** |

The top custom elements are `faceplate-loader` (36), `faceplate-partial` (29),
`faceplate-tracker` (19) and `shreddit-async-loader` (16). `faceplate-partial` names an endpoint and
a load trigger:

```html
<faceplate-partial name="HamburgerMenu_nwYSWB"
                   src="/svc/shreddit/hamburger-menu?selected-page-type=popular"
                   loading="programmatic">
```

So reddit is HTML-over-the-wire: script fetches HTML fragments and inserts them into the tree. The
feed itself is three posts of SSR plus a partial. **Rendering reddit and running reddit are the same
task**, which is not what ADR 0007 assumed when it put "new reddit" in tier 4 as a React
application. It is not React, and it is not less demanding for that.

Two more shapes worth naming:

- `<script type="module" src="data:text/javascript,…">` — module scripts from a `data:` URL, three
  of them.
- dynamic `import()` of `apply-polyfill-BcpMVdvg.js`, which is
  [`@virtualstate/navigation`](https://www.npmjs.com/package/@virtualstate/navigation) — a polyfill
  for `window.navigation`, the **Navigation API**. Reddit's own routing is built on it.

### reddit's stylesheet

`styles-css-CSasxzfw.css`: **14,729 bytes on the wire, 111,667 decompressed** — a 7.6:1 ratio, which
is ADR 0010's argument restated on a second site.

| Feature | Uses | Supported |
|---|---|---|
| `var(--…)` | 751 | **yes** |
| `[attr…]` selectors | 221 | yes |
| `:not(` | **136** | **no — never matches** |
| `:is(` / `:where(` | **52** | **no — never matches** |
| `linear-gradient` | 36 | yes |
| flex properties | 31 | yes |
| `calc(` | 29 | no |
| `:hover` | 17 | **no — never matches** |
| `transform:` | 14 | no |
| `@media` | 14 | yes |
| `aspect-ratio` | 11 | no |
| `:focus` | 7 | **no — never matches** |
| grid | 8 | no |
| `prefers-color-scheme` | 5 | no |

The four "never matches" rows are one bug, not four. `src/css/StyleSheet.cpp` ends its pseudo-class
chain with a comment that says an unimplemented pseudo-class must not match — which is the right
call and produces, on this stylesheet, 212 rules that silently do not apply.

## 3. youtube.com is a skeleton, and the page is in the bundle

The document is **909,022 bytes** and contains **421 tags**, of which exactly **two** are custom
elements: `<ytd-app>` and `<ytd-masthead>`. Everything a person sees is constructed by script.

The application bundle (`ytmainappweb.kevlar_base…`) is **10,732,237 bytes**.

Its stylesheet list starts with

    //fonts.googleapis.com/css2?family=Roboto:wght@300;400;500;700&family=YouTube+Sans:…&display=swap

which resolves to `@font-face` rules of the form

```css
@font-face {
  font-family: 'Roboto'; font-style: normal; font-weight: 300;
  font-stretch: 100%; font-display: swap;
  src: url(https://fonts.gstatic.com/s/roboto/v51/…woff2) format('woff2');
  unicode-range: U+0460-052F, U+1C80-1C8A, U+20B4, …;
}
```

`@font-face`, WOFF2, `unicode-range`, `font-display` — none of which exist here, and WOFF2 needs
brotli.

## 4. Plex is an empty page and two bundles

**30,228 bytes** of shell. `<body>` holds one empty `<div id="plex">`, a preloader div, and a modal
root. Two stylesheets and two scripts, all four with **Subresource Integrity** and
`crossorigin="anonymous"`:

    main-…-plex-4.160.0-….js    3,504,036 bytes
    9054-…-plex-4.160.0-….js    1,968,560 bytes

The very first inline script is

```js
const SESSION_STORAGE_KEY = "splashScreenViewed";
try { … window.sessionStorage.getItem(SESSION_STORAGE_KEY) … } catch {}
```

which is the polite version. The application's sign-in token does not go in a `try`/`catch`.

## 5. What 16.2MB of script asks the platform for

Occurrences across `ytapp.js`, `plexmain.js` and `plexvendor.js`. Blank means zero.

| API | youtube | plex-main | plex-vendor | total |
|---|---|---|---|---|
| `addEventListener` | 655 | 69 | 206 | **930** |
| `classList` | 266 | 5 | 10 | 281 |
| `CustomEvent` | 281 | | 12 | 293 |
| `HTMLElement` | 256 | 4 | 14 | 274 |
| `scrollTop` | 119 | 98 | 37 | **254** |
| `focus()` | 157 | 28 | 47 | 232 |
| `clientWidth`/`clientHeight` | 176 | 10 | 40 | **226** |
| `keydown` | 116 | 20 | 33 | 169 |
| `offsetWidth`/`offsetHeight` | 92 | 21 | 45 | 158 |
| `getBoundingClientRect` | 114 | 11 | 27 | **152** |
| `navigator.*` | 90 | 26 | 26 | 142 |
| `fetch(` | 28 | 97 | 5 | 130 |
| `requestAnimationFrame` | 82 | 6 | 25 | 113 |
| `getComputedStyle` | 64 | 12 | 25 | 101 |
| `ResizeObserver` | 86 | 3 | 7 | **96** |
| `pointerdown/move/up` | 42 | | 48 | 90 |
| `FormData` | 61 | 3 | 17 | 81 |
| `touchstart` | 57 | 7 | 10 | 74 |
| `matchMedia` | 50 | | 2 | 52 |
| `IntersectionObserver` | 46 | 1 | 6 | 53 |
| `sessionStorage` | 50 | 1 | | 51 |
| `BigInt` | 50 | | 1 | 51 |
| `postMessage` | 41 | | 6 | 47 |
| `scrollIntoView`/`scrollTo(` | 23 | 14 | 7 | 44 |
| `localStorage` | 33 | 4 | 7 | 44 |
| `MutationObserver` | 29 | 1 | 10 | 40 |
| `XMLHttpRequest` | 13 | 3 | 21 | 37 |
| `TextEncoder`/`TextDecoder` | 32 | | 3 | 35 |
| `innerHTML` | 15 | | 19 | 34 |
| `.play()` | 28 | 3 | 3 | 34 |
| `visibilitychange` | 28 | 2 | 4 | 34 |
| `MediaSource` | 10 | 23 | | **33** |
| `clipboard` | 19 | 8 | 4 | 31 |
| `devicePixelRatio` | 18 | 1 | 9 | 28 |
| `WebGL` | 14 | 3 | 8 | 25 |
| `WebSocket` | 2 | 15 | 7 | 24 |
| `getContext('2d')` | 19 | 5 | | 24 |
| `ShadowRoot` | 13 | 4 | 7 | 24 |
| `history.pushState` | 16 | 3 | 4 | 23 |
| `customElements.` | 21 | | | 21 |
| `serviceWorker` | 20 | | | 20 |
| `EventSource` | 12 | 2 | 6 | 20 |
| `AbortController` | 12 | 1 | 4 | 17 |
| `.animate(` | 9 | 5 | 3 | 17 |
| `adoptedStyleSheets` | 16 | | | 16 |
| `attachShadow` | 15 | | | 15 |
| `caches` / `CacheStorage` | 14 | | | 14 |
| `FileReader` | 6 | 3 | 4 | 13 |
| `Notification` | 8 | 3 | 2 | 13 |
| `KeyframeEffect` | 11 | | | 11 |
| `popstate` | 5 | 2 | 4 | 11 |
| `contentEditable` | 4 | | 7 | 11 |
| `CSS.supports` | 7 | | 2 | 9 |
| `crypto.getRandomValues` | 3 | | 5 | 8 |
| `indexedDB` | 6 | | 1 | 7 |
| `Intl.` | 5 | 2 | | 7 |
| `geolocation` | 5 | | | 5 |
| `assignedNodes`/`assignedElements` | 4 | | | 4 |
| `DOMParser` | | | 4 | 4 |
| `SourceBuffer` | | 2 | 1 | 3 |
| `new Worker(` | 1 | | 1 | 2 |
| `requestMediaKeySystemAccess` | | **2** | | 2 |
| `crypto.subtle` | 2 | | | 2 |
| `new CSSStyleSheet` | 2 | | | 2 |

`iframe` appears 282 times across the three and is excluded from the table because the string is too
generic to count honestly; what is true is that youtube.com's document contains one.

### The five things that table says

1. **Geometry is the largest single ask.** `clientWidth`/`clientHeight`, `scrollTop`,
   `offsetWidth`/`offsetHeight`, `getBoundingClientRect` and `getComputedStyle` sum to **891
   occurrences** — more than any other category, and every one of them is script asking layout a
   question. ADR 0008 forbids `src/bindings` from including `src/layout`. That is the seam to
   decide, and ADR 0015 decides it.

2. **`scrollTop` alone (254) outranks `getBoundingClientRect`.** Scrolling is not chrome polish; it
   is an API these applications read and write constantly.

3. **Shadow DOM is youtube-only, and youtube is not optional.** 15 `attachShadow`, 16
   `adoptedStyleSheets`, 21 `customElements.`, 4 `assignedNodes`. reddit contributes zero to that
   column but writes `slot="…"` 70 times in its markup — light-DOM children addressed to a shadow
   tree its components create.

4. **Plex is the DRM question and youtube is not.** `requestMediaKeySystemAccess` appears twice, in
   `plexmain.js`, and nowhere else. `MediaSource` is 33, mostly Plex's.

5. **Nobody asked for WebGL more than they asked for canvas.** 25 against 24, and both are far below
   the interaction APIs. That is an argument about ordering, made in ADR 0029.

## 6. What is missing from this browser, as a flat list

Taken from the above against the code as it stands on 2026-08-04:

- **Images**: PNG and SVG decode. JPEG, WebP, GIF and AVIF do not. reddit's front page references
  25 PNG and 8 JPEG.
- **Fonts**: no `@font-face`, no WOFF/WOFF2.
- **Encodings**: `<meta charset>` is not read and no legacy decoder exists. Everything is treated as
  UTF-8.
- **Selectors**: no `:not()`, `:is()`, `:where()`, `:nth-child()`, `:hover`, `:focus`, `:active`,
  `:checked`, `:disabled`.
- **Geometry from script**: none of it.
- **Scrolling**: a wheel delta reaches the engine; no element scroll offset, no scroll event, no
  `scrollTop`.
- **Input**: `PointerMessage` (Move/Down/Up, no modifiers), `TextInputMessage`, and an
  `InputCommandMessage` with three commands. No DOM key events, no focus model, no composition.
- **Script networking**: no `fetch`, no `XMLHttpRequest`, no `WebSocket`, no `EventSource`.
- **Storage**: none. No `localStorage`, `sessionStorage`, IndexedDB or Cache API.
- **Shadow DOM**: none. `<template>` is not special-cased.
- **Navigation**: no `history.pushState`, no `popstate`, no Navigation API.
- **Media**: none at all.
- **Canvas**: none.
- **Iframes**: `<iframe>` is a tag with no browsing context behind it.
- **Incremental rendering**: a page appears when its load finishes.

## Reproducing this

```bash
UA='Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36'

# reddit, through the challenge
curl -sS --compressed -A "$UA" -c cj.txt -o r1.html https://www.reddit.com/
#   read `token` and the seed string out of r1.html, then
curl -sS --compressed -A "$UA" -b cj.txt -c cj.txt -L -o reddit.html \
  "https://www.reddit.com/?solution=<seed><seed>&js_challenge=1&token=<token>&jsc_orig_r="

curl -sS --compressed -A "$UA" -o yt.html   https://www.youtube.com/
curl -sS --compressed -A "$UA" -o plex.html https://app.plex.tv/desktop/
```

The challenge parameters rotate. The shape has been stable; the values in this document have not
been re-checked since the day they were taken.
