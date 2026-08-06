#include "engine/PageScript.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

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
      return pending_urls_[i].url;
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
  pending_imports_.clear();
  module_fetches_.clear();
  requested_modules_.clear();
  modules_.Clear();
  frames_ = bindings::AnimationFrames{};
  performance_ = bindings::Performance{};
}

void PageScript::Collect(dom::Document& document, const DocumentPolicy& policy) {
  slots_.clear();
  pending_urls_.clear();
  pending_slots_.clear();
  ran_ = false;

  // Gathered before any of them runs, because running one can add elements to
  // the tree -- and a walk that collected as it went would then try to run
  // whatever a script had just written.
  document.ForEachDescendant([this, &policy](const dom::Node& node) {
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
    const std::string* nonce_attribute = element.GetAttribute("nonce");
    const std::string_view nonce =
        nonce_attribute == nullptr ? std::string_view{} : std::string_view(*nonce_attribute);

    // `script-src`. An external script is judged by its URL and an inline one
    // by its text, and a nonce answers for either -- which is CSP2's change and
    // what makes reddit's `default-src 'none'; script-src 'nonce-…'` a page that
    // runs rather than a page that does not.
    if (external) {
      if (!policy.AllowsUrl(csp::Directive::Script, *src, nonce)) {
        return;
      }
    } else if (!policy.AllowsInline(csp::Directive::Script, nonce, node.TextContent())) {
      AddPerformanceCounter(PerfCounterId::CspInlineBlocked);
      return;
    }

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
    SubresourceRequest request;
    request.url = *src;
    if (const std::string* integrity = element.GetAttribute("integrity")) {
      request.integrity = *integrity;
    }
    if (const std::string* cross_origin = element.GetAttribute("crossorigin")) {
      request.cross_origin = *cross_origin;
    }
    pending_urls_.push_back(std::move(request));
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
  const std::size_t slot = pending_slots_[index];
  if (slots_[slot].module) {
    // A module script's own source goes into the graph too, keyed by the URL it
    // came from, so that a *static* `import` inside it can be resolved -- and so
    // that asking the graph what is missing names that import before anything is
    // evaluated. See PageModules.cpp: the resolver cannot fetch, so the graph has
    // to be closed first.
    modules_.Add(pending_urls_[index].url, source);
    RefreshModuleFetches();
  }
  slots_[slot].source = std::move(source);
}

void PageScript::EnsureInterpreter(dom::Document& document, const std::string& url,
                                   std::int64_t now_ms) {
  if (interpreter_ != nullptr) {
    return;
  }
  interpreter_ = std::make_unique<js::Interpreter>();
  bindings_ = std::make_unique<bindings::DomBindings>(*interpreter_, document, url,
                                                     geometry_, network_, history_, storage_,
                                                     sockets_, media_, canvas_, workers_);
  bindings_->Install();
  timers_.Install(*interpreter_, now_ms);
  frames_.Install(*interpreter_, now_ms);
  performance_.Install(*interpreter_, now_ms);
  // After the interpreter exists and before anything runs: a module's first
  // `import` is resolved during evaluation, and the resolver has to be there.
  InstallModuleHost(url);
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
  performance_.Tick(*interpreter_, now_ms);

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

bool PageScript::DeliverFetchResponse(std::uint64_t id,
                                      const bindings::ScriptResponse& response) {
  // No interpreter means no `fetch` was ever declared, so nothing can be
  // waiting -- and building one here to deliver into would be a second way to
  // create a page's global scope.
  return bindings_ != nullptr && bindings_->DeliverFetchResponse(id, response);
}

void PageScript::TickClock(std::int64_t now_ms) {
  if (interpreter_ != nullptr) {
    performance_.Tick(*interpreter_, now_ms);
  }
}

void PageScript::SetNavigationTiming(double dom_content_loaded_ms, double load_event_ms,
                                     double duration_ms) {
  // Not gated on there being an interpreter: `Performance` holds what arrives
  // early as plain data and flushes it when one exists. Every subresource of a
  // document completes before its first script runs -- that is what
  // render-blocking means -- so the entries a page observes with
  // `buffered: true` are exactly the ones a gate here would have dropped.
  performance_.SetNavigationTiming(interpreter_.get(), dom_content_loaded_ms, load_event_ms,
                                   duration_ms);
}

void PageScript::AddResourceTiming(const std::string& name, const std::string& initiator,
                                   double start_ms, double response_end_ms,
                                   std::size_t encoded_size, std::size_t decoded_size) {
  performance_.AddResourceTiming(interpreter_.get(), name, initiator, start_ms, response_end_ms,
                                 encoded_size, decoded_size);
}

void PageScript::SetDocumentUrl(const std::string& url) {
  if (bindings_ != nullptr) {
    bindings_->SetDocumentUrl(url);
  }
}

bool PageScript::NotifyPopState() {
  return bindings_ != nullptr && bindings_->DispatchPopState();
}

bool PageScript::NotifyHashChange(const std::string& old_url, const std::string& new_url) {
  return bindings_ != nullptr && bindings_->DispatchHashChange(old_url, new_url);
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
  if (interpreter_ != nullptr) {
    performance_.Tick(*interpreter_, now_ms);
  }
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
  if (bindings_ == nullptr) {
    return false;
  }
  // Both kinds of observer at the one place a frame is produced, and the
  // performance ones second: a geometry callback can `mark`, and an entry
  // produced during a delivery belongs to the next one rather than to the
  // delivery that is running.
  bool ran = bindings_->DeliverViewObservations(frames_.Timestamp(now_ms));
  ran = performance_.DeliverObservations(*interpreter_) || ran;
  // And `slotchange`, at the same one place and for the same reason: a page that
  // could make one fire from inside its own handler controls how deep that goes.
  // ADR 0019 §2 -- assignment is the one piece of eager state in the flat-tree
  // design, and this is where the change in it is noticed.
  ran = bindings_->DeliverSlotChanges() || ran;
  return ran;
}

const std::vector<std::string>& PageScript::ConsoleOutput() const {
  static const std::vector<std::string> kNone;
  return interpreter_ == nullptr ? kNone : interpreter_->ConsoleOutput();
}

}  // namespace microbrowser::engine
