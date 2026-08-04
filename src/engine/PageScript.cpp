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

std::string PageScript::SourceName(std::size_t slot) const {
  // An external script is worth naming by its URL: on a page that loads nine
  // of them, "the fourth script" is not something anyone can act on.
  for (std::size_t i = 0; i < pending_slots_.size(); ++i) {
    if (pending_slots_[i] == slot) {
      return pending_urls_[i];
    }
  }
  return "inline script #" + std::to_string(slot);
}

void PageScript::Collect(dom::Document& document) {
  slots_.clear();
  pending_urls_.clear();
  pending_slots_.clear();
  ran_ = false;

  // Gathered before any of them runs, because running one can add elements to
  // the tree -- and a walk that collected as it went would then try to run
  // whatever a script had just written.
  document.ForEachDescendant([this](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() != "script" || !IsJavaScript(element)) {
      return;
    }
    const std::string* src = element.GetAttribute("src");
    if (src == nullptr || src->empty()) {
      slots_.push_back(node.TextContent());
      return;
    }
    // A slot now, filled later. Its position is what keeps document order
    // across the two kinds, and an external script that never arrives leaves
    // an empty slot rather than moving everything after it.
    pending_urls_.push_back(*src);
    pending_slots_.push_back(slots_.size());
    slots_.emplace_back();
  });
}

void PageScript::AddFetched(std::size_t index, std::string source) {
  if (index >= pending_slots_.size()) {
    return;
  }
  slots_[pending_slots_[index]] = std::move(source);
}

void PageScript::Run(dom::Document& document, const std::string& url,
                     std::int64_t now_ms) {
  if (ran_) {
    return;
  }
  ran_ = true;
  errors_.clear();
  if (slots_.empty()) {
    return;  // no script, no interpreter: a document that runs nothing costs nothing
  }
  interpreter_ = std::make_unique<js::Interpreter>();
  bindings_ = std::make_unique<bindings::DomBindings>(*interpreter_, document, url);
  bindings_->Install();
  timers_.Install(*interpreter_, now_ms);

  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    const std::optional<std::string>& source = slots_[slot];
    if (!source.has_value()) {
      continue;  // an external script that did not arrive
    }
    // A script that throws does not stop the page: the next one still runs,
    // and so does the rest of the load. That is what a browser does, and it is
    // why one broken analytics tag does not blank a site.
    //
    // It is also why the throw has to be recorded. Continuing past a failure
    // and saying nothing about it is how nine failed scripts and no scripts at
    // all come to look the same from outside.
    const js::Result result = interpreter_->Run(*source);
    if (result.completion == js::Completion::Throw) {
      std::string report = SourceName(slot) + ": " + js::ToString(result.value);
      // The stack when the thrown value carries one, which every error the
      // engine makes now does. "undefined is not a function" names the fault
      // and not the place, and on a page with a megabyte of script the place
      // is the entire question.
      if (result.value.IsObject()) {
        if (const js::Value* stack = result.value.object->Get("stack")) {
          if (stack->type == js::ValueType::String) {
            report += "\n    " + stack->AsString();
          }
        }
      }
      errors_.push_back(std::move(report));
    }
  }
}

std::optional<std::uint32_t> PageScript::NextTimerDelay(std::int64_t now_ms) const {
  // A page that ran no script can have no timers, and asking costs nothing --
  // which is what keeps a static document from ever waking the loop.
  return interpreter_ == nullptr ? std::nullopt : timers_.NextDelay(now_ms);
}

bool PageScript::RunDueTimers(std::int64_t now_ms) {
  return interpreter_ != nullptr && timers_.RunDue(*interpreter_, now_ms);
}

bool PageScript::DispatchClick(dom::Element& target) {
  // No script, no handlers: a page that ran nothing cannot have registered
  // anything, and building an interpreter to find that out would make every
  // click on a static page cost one.
  return bindings_ != nullptr && bindings_->DispatchClick(target);
}

const std::vector<std::string>& PageScript::ConsoleOutput() const {
  static const std::vector<std::string> kNone;
  return interpreter_ == nullptr ? kNone : interpreter_->ConsoleOutput();
}

}  // namespace microbrowser::engine
