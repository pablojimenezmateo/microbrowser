#include "css/StyleResolver.h"

namespace microbrowser::css {

std::string_view UserAgentStyleSheet() {
  // Without this, `<div>` is inline and every document is one long line. The
  // values are the ones the HTML specification's rendering section gives.
  return R"CSS(
html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, section, article,
header, footer, nav, aside, main, blockquote, pre, form, figure, figcaption,
hr, dl, dt, dd, fieldset, legend, details, summary, dialog, address, center,
menu, dir, optgroup, hgroup, search, noscript {
  display: block;
}
/* A block, not an inline: a <center> holding a table is the classic 1990s page
   layout, and treating it as inline puts the table's rows on one line. The
   alignment value is the one that also centres block children, which is the
   whole point of the element and which `text-align: center` cannot do. */
center { text-align: -microbrowser-center }
/* The HTML rendering spec resets alignment at a table for exactly this reason:
   `text-align` inherits, so without it a <center> or a centred div would centre
   the text of every cell in every table inside it -- which is not what any page
   wrapping a layout table in <center> is asking for. */
table { text-align: left }
li { display: list-item }
input, button, textarea, select {
  display: inline-block; background-color: white; border: 1px solid gray
}
/* HTML §15.3.1. A `<button>` is a block container whose children are real boxes -- it is not a
   replaced element -- so the centring its label gets has to be said here. The vertical half has
   no CSS spelling and lives in LayoutBlock (ReplacedBoxes::CentersContentVertically). */
button { text-align: center; padding: 1px 6px }
table { display: table }
caption { display: table-caption }
colgroup { display: table-column-group }
col { display: table-column }
thead { display: table-header-group }
tbody { display: table-row-group }
tfoot { display: table-footer-group }
tr { display: table-row }
td, th { display: table-cell }
head, style, script, title, meta, link, source { display: none }
/* HTML §15.3.1. Without this, `input` is inline-block from the rule above and
   BuildBoxTree still skips hidden inputs via IsHiddenInput — so every
   RestyleWithoutLayout saw "generates a box but has none" and rebuilt the
   whole tree (280 times on youtube /results, TD-0021 / TD-0033). */
input[type=hidden] { display: none !important }
/* HTML's UA rule. `!important` is load-bearing: an author `display:flex` on the
   same element must lose, or Polymer/boolean `hidden` is a no-op against a
   component stylesheet (youtube's `#content` inside expandable metadata). */
[hidden] { display: none !important }
body { margin: 8px }
p { margin: 1em 0 }
h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0 }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0 }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0 }
h4 { font-weight: bold; margin: 1.33em 0 }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0 }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0 }
b, strong { font-weight: bold }
i, em { font-style: italic }
small { font-size: 0.83em }
big { font-size: 1.17em }
code, kbd, samp, tt, pre { font-family: monospace }
a:link { color: #0000EE }
ul, ol { margin: 1em 0; padding-left: 40px }
blockquote { margin: 1em 40px }
pre { white-space: pre; margin: 1em 0 }
hr { margin: 0.5em 0; border-width: 1px; border-color: gray }
/* The two bidi elements, whose whole meaning is a `unicode-bidi` value. `<bdo>` overrides the
   algorithm for its contents; `<bdi>` isolates them, which is what makes an untrusted user name
   safe to put in the middle of a sentence -- without it, a name that starts with an Arabic letter
   reorders the text around it. `<bdi>`'s `dir` also defaults to `auto` rather than inheriting,
   which is the point of the element and is handled where `dir` is read. */
bdo { unicode-bidi: bidi-override }
bdi { unicode-bidi: isolate }
)CSS";
}

}  // namespace microbrowser::css
