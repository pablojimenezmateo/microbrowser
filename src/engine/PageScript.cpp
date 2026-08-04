#include "engine/PageScript.h"

#include <string>
#include <vector>

namespace microbrowser::engine {

namespace {

// A `type` that is not JavaScript is data the page put in a script tag so the
// parser would leave it alone -- a template, a JSON blob -- and running it
// would be worse than ignoring it.
bool IsJavaScript(const dom::Element& element) {
  const std::string* type = element.GetAttribute("type");
  return type == nullptr || type->empty() || *type == "text/javascript" ||
         *type == "application/javascript" || *type == "module";
}

}  // namespace

void PageScript::Run(dom::Document& document) {
  interpreter_ = std::make_unique<js::Interpreter>();
  bindings_ = std::make_unique<bindings::DomBindings>(*interpreter_, document);
  bindings_->Install();

  // Gathered before any of them runs, because running one can add elements to
  // the tree -- and a walk that collected as it went would then try to run
  // whatever a script had just written.
  std::vector<std::string> sources;
  document.ForEachDescendant([&sources](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() == "script" && !element.HasAttribute("src") &&
        IsJavaScript(element)) {
      sources.push_back(node.TextContent());
    }
  });

  for (const std::string& source : sources) {
    // A script that throws does not stop the page: the next one still runs,
    // and so does the rest of the load. That is what a browser does, and it is
    // why one broken analytics tag does not blank a site.
    (void)interpreter_->Run(source);
  }
}

const std::vector<std::string>& PageScript::ConsoleOutput() const {
  static const std::vector<std::string> kNone;
  return interpreter_ == nullptr ? kNone : interpreter_->ConsoleOutput();
}

}  // namespace microbrowser::engine
