#include "engine/PageScript.h"

#include <algorithm>
#include <string>
#include <utility>
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

bool IsModule(const dom::Element& element) {
  const std::string* type = element.GetAttribute("type");
  return type != nullptr && *type == "module";
}

// When this element's script runs.
//
// `defer` and `async` are ignored on an inline classic script, which is what
// the specification says and is not a shortcut: honouring them would reorder
// scripts a page wrote expecting them in order, and an inline script has
// nothing to wait for in any case.
PageScript::Timing TimingFor(const dom::Element& element, bool external, bool module) {
  using Timing = PageScript::Timing;
  if (!external) {
    return module ? Timing::Deferred : Timing::Blocking;
  }
  if (element.GetAttribute("async") != nullptr) {
    return Timing::Async;
  }
  if (module || element.GetAttribute("defer") != nullptr) {
    return Timing::Deferred;
  }
  return Timing::Blocking;
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

void PageScript::Detach() {
  // The binding layer first: it is the one holding the reference.
  bindings_.reset();
  interpreter_.reset();
  slots_.clear();
  pending_urls_.clear();
  pending_slots_.clear();
  errors_.clear();
  ran_ = false;
  timers_ = bindings::TimerQueue{};
  frames_ = bindings::AnimationFrames{};
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
    const bool external = src != nullptr && !src->empty();
    const bool module = IsModule(element);

    Slot slot;
    slot.timing = TimingFor(element, external, module);
    slot.module = module;
    if (!external) {
      slot.source = node.TextContent();
      slots_.push_back(std::move(slot));
      return;
    }
    // A slot now, filled later. Its position is what keeps document order
    // across the two kinds, and an external script that never arrives leaves
    // an empty slot rather than moving everything after it.
    pending_urls_.push_back(*src);
    pending_slots_.push_back(slots_.size());
    slots_.push_back(std::move(slot));
  });
}

bool PageScript::IsAsync(std::size_t index) const {
  return index < pending_slots_.size() &&
         slots_[pending_slots_[index]].timing == Timing::Async;
}

void PageScript::AddFetched(std::size_t index, std::string source) {
  if (index >= pending_slots_.size()) {
    return;
  }
  slots_[pending_slots_[index]].source = std::move(source);
}

void PageScript::EnsureInterpreter(dom::Document& document, const std::string& url,
                                   std::int64_t now_ms) {
  if (interpreter_ != nullptr) {
    return;
  }
  interpreter_ = std::make_unique<js::Interpreter>();
  bindings_ = std::make_unique<bindings::DomBindings>(*interpreter_, document, url, geometry_);
  bindings_->Install();
  timers_.Install(*interpreter_, now_ms);
  frames_.Install(*interpreter_, now_ms);
}

bool PageScript::RunTiming(Timing timing) {
  bool ran_any = false;
  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    Slot& entry = slots_[slot];
    if (entry.timing != timing || !entry.source.has_value()) {
      continue;  // a different point in the lifecycle, or a script that never arrived
    }
    // Taken rather than read: a slot that has run and a slot that never
    // arrived are the same thing to everything downstream, and emptying it
    // here is what makes running one twice impossible rather than unlikely.
    const std::string source = std::move(*entry.source);
    entry.source.reset();
    ran_any = true;

    // A script that throws does not stop the page: the next one still runs,
    // and so does the rest of the load. That is what a browser does, and it is
    // why one broken analytics tag does not blank a site.
    //
    // It is also why the throw has to be recorded. Continuing past a failure
    // and saying nothing about it is how nine failed scripts and no scripts at
    // all come to look the same from outside.
    const js::Result result = entry.module
                                  ? interpreter_->RunModule(source, SourceName(slot))
                                  : interpreter_->Run(source);
    if (result.completion != js::Completion::Throw) {
      continue;
    }
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
  return ran_any;
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
  EnsureInterpreter(document, url, now_ms);

  // The three points in the lifecycle, in the order they happen. Blocking
  // first, because that is document order; deferred after every blocking one,
  // which is what `defer` promises; async last, and only those that happen to
  // have arrived -- the rest run through RunReadyAsync when they do.
  RunTiming(Timing::Blocking);
  RunTiming(Timing::Deferred);
  // Then the parse is over as far as a page can tell: `readyState` becomes
  // "interactive" and `DOMContentLoaded` fires. Before the `async` scripts,
  // which is what the attribute means -- an async script is one the page said
  // the document need not wait for, so it must not hold this back.
  //
  // A page that registers a `DOMContentLoaded` listener and is never told is
  // a page that does nothing at all, which is the state reddit's interstitial
  // was in.
  bindings_->NotifyDomContentLoaded();
  RunTiming(Timing::Async);
}

bool PageScript::RunReadyAsync() {
  return interpreter_ != nullptr && RunTiming(Timing::Async);
}

std::optional<std::uint32_t> PageScript::NextWakeDelay(std::int64_t now_ms) const {
  // A page that ran no script can have neither timers nor frames, and asking
  // costs nothing -- which is what keeps a static document from ever waking
  // the loop.
  if (interpreter_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> timer = timers_.NextDelay(now_ms);
  const std::optional<std::uint32_t> frame = frames_.NextDelay(now_ms);
  if (!timer.has_value()) {
    return frame;
  }
  return frame.has_value() ? std::optional<std::uint32_t>(std::min(*timer, *frame)) : timer;
}

bool PageScript::RunDueWork(std::int64_t now_ms) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Timers first, then the frame. That is the order the event loop defines and
  // it is the useful one: a timer that moves something should be reflected by
  // the frame that draws it, in the same turn rather than 16ms later.
  const bool timers = timers_.RunDue(*interpreter_, now_ms);
  const bool frame = frames_.RunDue(*interpreter_, now_ms);
  return timers || frame;
}

bool PageScript::DispatchSubmit(dom::Element& form) {
  return bindings_ != nullptr && bindings_->DispatchSubmit(form);
}

std::optional<bindings::PendingSubmit> PageScript::TakePendingSubmit() {
  return bindings_ == nullptr ? std::nullopt : bindings_->TakePendingSubmit();
}

bool PageScript::NotifyLoad() {
  return bindings_ != nullptr && bindings_->NotifyLoad();
}

bool PageScript::DispatchClick(dom::Element& target, const bindings::PointerInput& pointer) {
  // No script, no handlers: a page that ran nothing cannot have registered
  // anything, and building an interpreter to find that out would make every
  // click on a static page cost one.
  return bindings_ != nullptr && bindings_->DispatchClick(target, pointer);
}

bool PageScript::DispatchKey(dom::Node* target, const bindings::KeyInput& key) {
  return bindings_ != nullptr && bindings_->DispatchKey(target, key);
}

bool PageScript::MoveFocus(dom::Element* target, bool visible) {
  return bindings_ != nullptr && bindings_->MoveFocus(target, visible);
}

bool PageScript::DispatchScroll(dom::Element* target) {
  return bindings_ != nullptr && bindings_->DispatchScroll(target);
}

bool PageScript::DeliverViewObservations(std::int64_t now_ms) {
  return bindings_ != nullptr && bindings_->DeliverViewObservations(frames_.Timestamp(now_ms));
}

const std::vector<std::string>& PageScript::ConsoleOutput() const {
  static const std::vector<std::string> kNone;
  return interpreter_ == nullptr ? kNone : interpreter_->ConsoleOutput();
}

}  // namespace microbrowser::engine
