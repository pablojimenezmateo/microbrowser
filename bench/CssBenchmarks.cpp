// The cascade: resolving a computed style for every element of a document.
//
// This is the hottest non-JavaScript code in the browser -- on
// en.wikipedia.org/wiki/CSS it is `engine::BuildBoxTree` plus
// `engine::CollectImages`, which between them resolve 84,731 styles per load --
// and until this file there was no way to measure it that did not involve a
// network. That matters more than it sounds: the machine this is developed on
// is shared, and a page-load measurement taken while something else is
// compiling reads three times slower than the same code measured alone. A
// benchmark is the only honest instrument for a change of a few per cent.
//
// The document and the stylesheet are synthetic but shaped like a real page:
// a few hundred rules of which most are class selectors, a tree deep enough
// that inheritance actually runs, and elements carrying the id/class/tag mix
// the rule index buckets on.

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "BenchSupport.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"

namespace microbrowser::bench {

namespace {

struct Page {
  std::unique_ptr<dom::Document> document;
  css::StyleResolver resolver;
  std::size_t elements = 0;
};

std::string Words(std::size_t index, const char* const* table, std::size_t count) {
  return table[index % count];
}

// A stylesheet with the shape authors write: mostly single-class rules, some
// descendant selectors, a handful of ids, and a few that inherit.
std::string SyntheticStyleSheet() {
  static constexpr const char* kClasses[] = {"box",  "row",   "cell",  "title", "meta",
                                             "link", "byline", "body", "note",  "tag"};
  std::string css;
  css += ":root { --fg: #202028; --bg: #ffffff; --gap: 8px }\n";
  for (std::size_t i = 0; i < 240; ++i) {
    const std::string name = Words(i, kClasses, 10) + std::to_string(i % 24);
    css += "." + name +
           " { color: var(--fg); margin: 4px; padding: 2px 4px; font-size: 14px;"
           " background-color: #FAFAFA; border: 1px solid #DDD }\n";
  }
  for (std::size_t i = 0; i < 60; ++i) {
    css += ".box" + std::to_string(i % 24) + " ." + Words(i, kClasses, 10) +
           std::to_string(i % 24) + " { font-weight: bold; text-align: left }\n";
  }
  for (std::size_t i = 0; i < 40; ++i) {
    css += "div p span { line-height: 1.4 }\n";
  }
  return css;
}

Page BuildPage() {
  static constexpr const char* kTags[] = {"div", "p", "span", "a", "li", "td"};
  static constexpr const char* kClasses[] = {"box",  "row",   "cell",  "title", "meta",
                                             "link", "byline", "body", "note",  "tag"};
  Page page;
  page.document = std::make_unique<dom::Document>();

  // A tree that is wide and a few levels deep, which is what inheritance has to
  // walk: a flat document would make every style resolve against the initial
  // one and measure the wrong thing.
  auto& root = static_cast<dom::Element&>(
      page.document->Append(std::make_unique<dom::Element>("html")));
  auto& body = static_cast<dom::Element&>(
      root.Append(std::make_unique<dom::Element>("body")));
  std::size_t counter = 0;
  for (std::size_t section = 0; section < 60; ++section) {
    auto& outer = static_cast<dom::Element&>(
        body.Append(std::make_unique<dom::Element>("div")));
    outer.SetAttribute("class", "box" + std::to_string(section % 24));
    ++page.elements;
    for (std::size_t row = 0; row < 12; ++row) {
      auto& middle = static_cast<dom::Element&>(
          outer.Append(std::make_unique<dom::Element>(Words(row, kTags, 6))));
      middle.SetAttribute("class", Words(row + section, kClasses, 10) +
                                       std::to_string((row + section) % 24));
      ++page.elements;
      for (std::size_t leaf = 0; leaf < 6; ++leaf) {
        auto& inner = static_cast<dom::Element&>(
            middle.Append(std::make_unique<dom::Element>(Words(leaf, kTags, 6))));
        inner.SetAttribute("class", Words(leaf, kClasses, 10) + std::to_string(leaf % 24));
        // One element in eight carries an inline style, which is roughly the
        // rate real markup does and is its own cost: the attribute is parsed
        // per resolve.
        if (++counter % 8 == 0) {
          inner.SetAttribute("style", "color: #123456; margin-top: 3px");
        }
        inner.Append(std::make_unique<dom::Text>("content"));
        ++page.elements;
      }
    }
  }

  page.resolver.AddStyleSheet(css::ParseStyleSheet(std::string(css::UserAgentStyleSheet()), {}),
                              css::Origin::UserAgent);
  page.resolver.AddStyleSheet(css::ParseStyleSheet(SyntheticStyleSheet(), {}),
                              css::Origin::Author);
  return page;
}

}  // namespace

void RegisterCssBenchmarks(std::vector<Benchmark>& benchmarks) {
  static const Page page = BuildPage();

  // Per *element*, not per call: what the browser does more of when a page gets
  // bigger is resolve one more style, and the number that has to come down is
  // the cost of one.
  AddBenchmark(benchmarks, "css/cascade-document", page.elements, "element", [] {
    std::size_t resolved = 0;
    page.resolver.ForEachStyledElement(
        *page.document, [&resolved](const dom::Element&, const css::ComputedStyle&) {
          ++resolved;
        });
    // Read the count back so the whole walk cannot be optimised away.
    if (resolved == 0) {
      std::abort();
    }
  });
}

}  // namespace microbrowser::bench
