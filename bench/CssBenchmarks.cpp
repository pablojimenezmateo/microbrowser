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

Page BuildPage(std::string_view extra_rules = {}) {
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
  page.resolver.AddStyleSheet(
      css::ParseStyleSheet(SyntheticStyleSheet() + std::string(extra_rules), {}),
      css::Origin::Author);
  return page;
}

// The same element count, arranged as a *spine*: forty nested wrappers, each
// carrying a hundred leaves.
//
// This exists because the wide page above cannot price `:has()` at all. A
// descendant search costs the size of the subtree it walks, so on a tree where
// almost every element is a leaf it costs nothing -- the four `:has()` rows
// over `BuildPage()` come out inside the noise of the plain cascade. The cost
// is a function of the *sum of subtree sizes*, which is the element count times
// the average depth, and that is what a real page's div nesting supplies.
// Forty is roughly youtube's depth.
Page BuildDeepPage(std::string_view extra_rules = {}) {
  Page page;
  page.document = std::make_unique<dom::Document>();
  auto& root = static_cast<dom::Element&>(
      page.document->Append(std::make_unique<dom::Element>("html")));
  auto* spine = &static_cast<dom::Element&>(root.Append(std::make_unique<dom::Element>("body")));
  page.elements = 2;
  for (std::size_t level = 0; level < 40; ++level) {
    auto& wrapper =
        static_cast<dom::Element&>(spine->Append(std::make_unique<dom::Element>("div")));
    wrapper.SetAttribute("class", "box" + std::to_string(level % 24));
    ++page.elements;
    for (std::size_t leaf = 0; leaf < 100; ++leaf) {
      auto& child =
          static_cast<dom::Element&>(wrapper.Append(std::make_unique<dom::Element>("span")));
      child.SetAttribute("class", "cell" + std::to_string(leaf % 24));
      child.Append(std::make_unique<dom::Text>("content"));
      ++page.elements;
    }
    spine = &wrapper;
  }
  page.resolver.AddStyleSheet(css::ParseStyleSheet(std::string(css::UserAgentStyleSheet()), {}),
                              css::Origin::UserAgent);
  page.resolver.AddStyleSheet(
      css::ParseStyleSheet(SyntheticStyleSheet() + std::string(extra_rules), {}),
      css::Origin::Author);
  return page;
}

}  // namespace

void RegisterCssBenchmarks(std::vector<Benchmark>& benchmarks) {
  static const Page page = BuildPage();
  static const Page deep = BuildDeepPage();

  // Per *element*, not per call: what the browser does more of when a page gets
  // bigger is resolve one more style, and the number that has to come down is
  // the cost of one.
  //
  // **Read this before adding a row here.** `StyleResolver::StyleFor` caches a
  // computed style per element (TD-0021), keyed on the cascade generation, the
  // document's structure version, the element's attribute version and state,
  // and its parent's style id. None of those change between two iterations of
  // this benchmark, so **every iteration after the first is a cache hit** and
  // this row has not measured the cascade since that cache landed. It is still
  // a useful row -- resolving a cached style is what a repeated layout actually
  // does -- but a change to *matching* will not move it, which is why the
  // selector rows below exist and are written against `Selector::Matches`
  // rather than against the resolver.
  const auto cascade = [&benchmarks](const char* name, const Page& subject) {
    AddBenchmark(benchmarks, name, subject.elements, "element", [&subject] {
      std::size_t resolved = 0;
      subject.resolver.ForEachStyledElement(
          *subject.document, [&resolved](const dom::Element&, const css::ComputedStyle&) {
            ++resolved;
          });
      // Read the count back so the whole walk cannot be optimised away.
      if (resolved == 0) {
        std::abort();
      }
    });
  };
  cascade("css/cascade-document", page);

  // What one selector costs, asked of every element -- which is what the
  // cascade does for a rule the index cannot narrow.
  //
  // ADR 0016 §1 said `:has()` should land "behind a measurement, and if it is
  // expensive it stays behind one". These rows are that measurement, and
  // `css::kMaxHasCandidates` is what came of it. Two things the numbers say
  // that reading the code does not:
  //
  //  * The **shape of the tree** decides, not the element count. The wide page
  //    and the deep page hold the same 4,400 elements; a descendant search
  //    costs the sum of the subtree sizes, which is the element count times the
  //    average depth, so the deep page costs an order of magnitude more for the
  //    identical selector.
  //  * A **failing** `:has()` is the expensive one. A match stops at the first
  //    candidate; a miss visits every one of them. So the rule that costs the
  //    most is the rule that is doing nothing, which is the opposite of the
  //    intuition, and is why the bound is on candidates rather than on matches.
  //
  // The recorded numbers are beside `css::kMaxHasCandidates`, and so is the
  // warning that goes with them: **compare rows within one run**. This machine
  // is shared and every row moves by 3x between a quiet run and a loaded one,
  // together -- so `deep-has-miss` against `deep-class` from the same run is a
  // measurement, and either against a figure written down an hour ago is not.
  const auto match_all = [&benchmarks](const char* name, const Page& subject,
                                       std::string_view selector_text) {
    // Owned by the closure rather than by a container that could reallocate
    // under the closures registered before it, which is a segfault and was one.
    const auto selectors =
        std::make_shared<std::vector<css::Selector>>(css::ParseSelectorList(selector_text));
    AddBenchmark(benchmarks, name, subject.elements, "element", [&subject, selectors] {
      std::size_t matched = 0;
      subject.document->ForEachDescendant([&](const dom::Node& node) {
        if (!node.IsElement()) {
          return;
        }
        for (const css::Selector& selector : *selectors) {
          matched += selector.Matches(static_cast<const dom::Element&>(node)) ? std::size_t{1}
                                                                             : std::size_t{0};
        }
      });
      // Read the count back so the whole walk cannot be optimised away.
      if (matched == ~std::size_t{0}) {
        std::abort();
      }
    });
  };
  match_all("css/selector-class", page, ".cell0");
  match_all("css/selector-has-hit", page, ":has(.cell0)");
  match_all("css/selector-has-miss", page, ":has(.no-element-has-this-class)");
  match_all("css/selector-deep-class", deep, ".cell0");
  match_all("css/selector-deep-has-hit", deep, ":has(.cell0)");
  match_all("css/selector-deep-has-miss", deep, ":has(.no-element-has-this-class)");
}

}  // namespace microbrowser::bench
